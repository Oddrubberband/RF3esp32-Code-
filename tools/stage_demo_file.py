from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SPIFFS_BLOCK_SIZE = 4096
SPIFFS_PAGE_SIZE = 256
DEFAULT_MARGIN_BYTES = 64 * 1024


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Stage a host file into data/ for RF3 packet transfer and verify "
            "that the resulting SPIFFS image will fit."
        )
    )
    parser.add_argument("source", nargs="?", help="Path to the source file to stage")
    parser.add_argument("--pick", action="store_true", help="Open a file picker if no source path is supplied")
    parser.add_argument("--output-name", help="Optional destination filename inside data/")
    parser.add_argument(
        "--replace-existing-files",
        action="store_true",
        help="Remove other staged files from the temporary fit check before validating",
    )
    parser.add_argument(
        "--replace-existing-audio",
        action="store_true",
        help=argparse.SUPPRESS,
    )
    parser.add_argument("--data-dir", default="data", help="Path to the data/ directory")
    parser.add_argument("--partitions", default="partitions.csv", help="Path to the partition table")
    parser.add_argument("--mkspiffs", help="Explicit path to mkspiffs")
    return parser.parse_args()


def fail(message: str) -> int:
    print(f"Error: {message}", file=sys.stderr)
    return 1


def human_bytes(value: int) -> str:
    units = ["B", "KB", "MB", "GB"]
    size = float(value)
    for unit in units:
        if size < 1024.0 or unit == units[-1]:
            return f"{size:.1f} {unit}"
        size /= 1024.0
    return f"{value} B"


def parse_partition_size(size_text: str) -> int:
    cleaned = size_text.strip()
    upper = cleaned.upper()
    if upper.startswith("0X"):
        return int(upper, 16)
    if upper.endswith("K"):
        return int(upper[:-1]) * 1024
    if upper.endswith("M"):
        return int(upper[:-1]) * 1024 * 1024
    return int(upper)


def read_spiffs_partition_size(partitions_path: Path) -> int:
    for raw_line in partitions_path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue

        fields = [field.strip() for field in line.split(",")]
        if len(fields) >= 5 and fields[2].lower() == "spiffs":
            return parse_partition_size(fields[4])

    raise ValueError(f"No SPIFFS partition entry was found in {partitions_path}")


def sanitize_output_name(name: str) -> str:
    raw_name = Path(name).name if Path(name).name else name
    stem = Path(raw_name).stem if Path(raw_name).suffix else raw_name
    suffix = Path(raw_name).suffix.lower()

    cleaned_stem = re.sub(r"[^A-Za-z0-9_-]+", "_", stem).strip("_")
    if not cleaned_stem:
        cleaned_stem = "payload"

    if not re.fullmatch(r"\.[A-Za-z0-9]{1,8}", suffix or ""):
        suffix = ".bin"

    return f"{cleaned_stem}{suffix}"


def select_source_path(args: argparse.Namespace) -> Path | None:
    if args.source:
        return Path(args.source)

    env_source = os.environ.get("RF3_STAGE_SOURCE") or os.environ.get("DEMO_AUDIO_SOURCE")
    if env_source:
        return Path(env_source)

    if args.pick or not sys.stdin.isatty():
        try:
            import tkinter as tk
            from tkinter import filedialog

            root = tk.Tk()
            root.withdraw()
            selected = filedialog.askopenfilename(title="Choose a file to stage for RF3")
            root.destroy()
            return Path(selected) if selected else None
        except Exception:
            pass

    if not sys.stdin.isatty():
        return None

    entered = input("Enter the path to a file to stage: ").strip()
    return Path(entered) if entered else None


def locate_tool(explicit_path: str | None, project_root: Path, candidate_names: list[str]) -> str | None:
    if explicit_path:
        explicit = Path(explicit_path).expanduser()
        return str(explicit) if explicit.exists() else None

    for name in candidate_names:
        resolved = shutil.which(name)
        if resolved:
            return resolved

    local_candidates = [
        project_root / ".pio-home" / "packages" / "tool-mkspiffs" / "mkspiffs_espressif32_espidf.exe",
        Path.home() / ".platformio" / "packages" / "tool-mkspiffs" / "mkspiffs_espressif32_espidf.exe",
    ]
    for candidate in local_candidates:
        if candidate.exists():
            return str(candidate)

    return None


def stage_data_directory(
    data_dir: Path,
    staged_name: str,
    source_file: Path,
    replace_existing_files: bool,
) -> Path:
    temp_root = Path(tempfile.mkdtemp(prefix="rf3-file-stage-"))
    stage_dir = temp_root / "data"
    stage_dir.mkdir(parents=True, exist_ok=True)

    for entry in data_dir.iterdir():
        if not entry.is_file():
            continue
        if entry.name == staged_name:
            continue
        if replace_existing_files:
            continue
        shutil.copy2(entry, stage_dir / entry.name)

    shutil.copy2(source_file, stage_dir / staged_name)
    return stage_dir


def validate_spiffs_fit(stage_dir: Path, spiffs_size: int, mkspiffs_path: str | None) -> tuple[bool, str]:
    if mkspiffs_path:
        image_path = stage_dir.parent / "spiffs.bin"
        command = [
            mkspiffs_path,
            "-c",
            str(stage_dir),
            "-b",
            str(SPIFFS_BLOCK_SIZE),
            "-p",
            str(SPIFFS_PAGE_SIZE),
            "-s",
            str(spiffs_size),
            str(image_path),
        ]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0:
            details = result.stderr.strip() or result.stdout.strip() or "mkspiffs rejected the staged image"
            return False, details
        return True, f"Validated with mkspiffs ({human_bytes(image_path.stat().st_size)} image)"

    total_bytes = sum(entry.stat().st_size for entry in stage_dir.iterdir() if entry.is_file())
    conservative_budget = max(0, spiffs_size - DEFAULT_MARGIN_BYTES)
    if total_bytes > conservative_budget:
        return False, (
            f"Staged files total {human_bytes(total_bytes)}, which exceeds the conservative "
            f"SPIFFS budget of {human_bytes(conservative_budget)} without mkspiffs validation"
        )
    return True, (
        f"mkspiffs was not found, so fit was checked conservatively against "
        f"{human_bytes(conservative_budget)}"
    )


def main() -> int:
    args = parse_args()
    project_root = Path(__file__).resolve().parent.parent
    data_dir = (project_root / args.data_dir).resolve() if not Path(args.data_dir).is_absolute() else Path(args.data_dir).resolve()
    partitions_path = (project_root / args.partitions).resolve() if not Path(args.partitions).is_absolute() else Path(args.partitions).resolve()

    if not data_dir.exists():
        return fail(f"Data directory does not exist: {data_dir}")
    if not partitions_path.exists():
        return fail(f"Partition table does not exist: {partitions_path}")

    source = select_source_path(args)
    if not source:
        return fail("No source file was selected")

    source = source.expanduser().resolve()
    if not source.exists() or not source.is_file():
        return fail(f"Source file does not exist: {source}")

    mkspiffs_path = locate_tool(args.mkspiffs, project_root, ["mkspiffs_espressif32_espidf", "mkspiffs"])
    spiffs_size = read_spiffs_partition_size(partitions_path)
    replace_existing_files = args.replace_existing_files or args.replace_existing_audio
    output_name = sanitize_output_name(args.output_name or source.name)
    output_path = data_dir / output_name

    stage_dir = stage_data_directory(
        data_dir=data_dir,
        staged_name=output_name,
        source_file=source,
        replace_existing_files=replace_existing_files,
    )

    try:
        fits, fit_details = validate_spiffs_fit(stage_dir, spiffs_size, mkspiffs_path)
        if not fits:
            return fail(fit_details)
    finally:
        shutil.rmtree(stage_dir.parent, ignore_errors=True)

    if source.resolve() != output_path.resolve():
        shutil.copy2(source, output_path)

    staged_bytes = output_path.stat().st_size
    print(f"Staged {output_path.name}")
    print(f"  Source: {source}")
    print(f"  Output: {output_path}")
    print(f"  Size:   {human_bytes(staged_bytes)}")
    print(f"  Fit:    {fit_details}")
    print()
    print("Next steps:")
    print("  1. Upload the filesystem image to the ESP32.")
    print("  2. Flash the firmware if needed.")
    print(f"  3. In the serial monitor, use FILES and then TX {output_path.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
