from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# The ESP32 firmware expects raw 8-bit unsigned pulse-code modulation (PCM) at
# 8 kHz. These constants define both the audio conversion format and the Serial
# Peripheral Interface Flash File System (SPIFFS) image sizing rules used to
# validate whether a new track will fit before it is copied into data/.
SAMPLE_RATE_HZ = 8000
SPIFFS_BLOCK_SIZE = 4096
SPIFFS_PAGE_SIZE = 256
SUPPORTED_EXTENSION = ".mp3"
DEFAULT_MARGIN_BYTES = 64 * 1024


def parse_args() -> argparse.Namespace:
    # The script supports both a mostly automatic "pick a file" flow and a more
    # explicit command-line interface (CLI) flow for repeatable automation.
    parser = argparse.ArgumentParser(
        description=(
            "Choose an MP3, convert it to 8-bit 8 kHz mono PCM for the RF3 demo, "
            "and verify that the resulting SPIFFS image will fit."
        )
    )
    parser.add_argument("source", nargs="?", help="Path to the source .mp3 file")
    parser.add_argument("--pick", action="store_true", help="Open a file picker if no source path is supplied")
    parser.add_argument("--output-name", help="Optional destination filename inside data/")
    parser.add_argument(
        "--replace-existing-audio",
        action="store_true",
        help="Remove other .u8 files from the staged image before validating fit",
    )
    parser.add_argument("--data-dir", default="data", help="Path to the data/ directory")
    parser.add_argument("--partitions", default="partitions.csv", help="Path to the partition table")
    parser.add_argument("--ffmpeg", help="Explicit path to ffmpeg")
    parser.add_argument("--mkspiffs", help="Explicit path to mkspiffs")
    return parser.parse_args()


def fail(message: str) -> int:
    # Keep all fatal error reporting consistent and easy to grep in continuous
    # integration (CI) logs.
    print(f"Error: {message}", file=sys.stderr)
    return 1


def human_bytes(value: int) -> str:
    # Small helper for readable console output such as "1.2 MB".
    units = ["B", "KB", "MB", "GB"]
    size = float(value)
    for unit in units:
        if size < 1024.0 or unit == units[-1]:
            return f"{size:.1f} {unit}"
        size /= 1024.0
    return f"{value} B"


def parse_partition_size(size_text: str) -> int:
    # Partition comma-separated values (CSV) may be hex, raw bytes, or K/M suffixes.
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
    # Walk the partition table and pull out the SPIFFS entry so fit checks stay
    # synchronized with the firmware's actual flash layout.
    for raw_line in partitions_path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue

        fields = [field.strip() for field in line.split(",")]
        if len(fields) >= 5 and fields[2].lower() == "spiffs":
            return parse_partition_size(fields[4])

    raise ValueError(f"No SPIFFS partition entry was found in {partitions_path}")


def sanitize_output_name(name: str) -> str:
    # Convert arbitrary source file names into safe SPIFFS-friendly American
    # Standard Code for Information Interchange (ASCII) names.
    stem = Path(name).stem if Path(name).suffix else name
    cleaned = re.sub(r"[^A-Za-z0-9_-]+", "_", stem).strip("_")
    if not cleaned:
        cleaned = "track"
    return f"{cleaned}.u8"


def select_source_path(args: argparse.Namespace) -> Path | None:
    # Source file selection precedence:
    # 1. explicit command-line interface (CLI) path
    # 2. DEMO_AUDIO_SOURCE environment variable
    # 3. graphical user interface (GUI) picker
    # 4. interactive stdin prompt
    if args.source:
        return Path(args.source)

    env_source = os.environ.get("DEMO_AUDIO_SOURCE")
    if env_source:
        return Path(env_source)

    if args.pick or not sys.stdin.isatty():
        try:
            import tkinter as tk
            from tkinter import filedialog

            root = tk.Tk()
            root.withdraw()
            selected = filedialog.askopenfilename(
                title="Choose an MP3 for the RF3 demo",
                filetypes=[("MP3 files", "*.mp3")],
            )
            root.destroy()
            return Path(selected) if selected else None
        except Exception:
            pass

    if not sys.stdin.isatty():
        return None

    entered = input("Enter the path to an MP3 file: ").strip()
    return Path(entered) if entered else None


def locate_tool(explicit_path: str | None, project_root: Path, candidate_names: list[str]) -> str | None:
    # Search common locations for host tools so the script works both in a plain
    # shell and when launched from PlatformIO.
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


def run_ffmpeg(ffmpeg_path: str, source: Path, output_path: Path) -> tuple[bool, str]:
    # Convert the source MPEG Audio Layer III (MP3) file into the exact raw
    # pulse-code modulation (PCM) format the firmware expects.
    command = [
        ffmpeg_path,
        "-y",
        "-hide_banner",
        "-loglevel",
        "error",
        "-i",
        str(source),
        "-ac",
        "1",
        "-ar",
        str(SAMPLE_RATE_HZ),
        "-f",
        "u8",
        str(output_path),
    ]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        return False, result.stderr.strip() or "ffmpeg conversion failed"
    return True, ""


def stage_data_directory(
    data_dir: Path,
    staged_name: str,
    converted_file: Path,
    replace_existing_audio: bool,
) -> Path:
    # Build a temporary snapshot of what data/ would look like after adding the
    # new file. The fit check runs against this staged copy so the script can
    # refuse oversized uploads without mutating the real project tree first.
    temp_root = Path(tempfile.mkdtemp(prefix="rf3-audio-stage-"))
    stage_dir = temp_root / "data"
    stage_dir.mkdir(parents=True, exist_ok=True)

    for entry in data_dir.iterdir():
        if not entry.is_file():
            continue
        if entry.name == staged_name:
            continue
        if replace_existing_audio and entry.suffix.lower() == ".u8":
            continue
        shutil.copy2(entry, stage_dir / entry.name)

    shutil.copy2(converted_file, stage_dir / staged_name)
    return stage_dir


def validate_spiffs_fit(stage_dir: Path, spiffs_size: int, mkspiffs_path: str | None) -> tuple[bool, str]:
    # Prefer validating with mkspiffs because it matches the real filesystem
    # packer. If the tool is unavailable, fall back to a conservative byte
    # budget with an extra safety margin.
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
            f"Converted files total {human_bytes(total_bytes)}, which exceeds the conservative "
            f"SPIFFS budget of {human_bytes(conservative_budget)} without mkspiffs validation"
        )
    return True, (
        f"mkspiffs was not found, so fit was checked conservatively against "
        f"{human_bytes(conservative_budget)}"
    )


def main() -> int:
    # The main flow is:
    # 1. locate the project data/ folder and partition table
    # 2. choose an MP3
    # 3. convert it to raw PCM
    # 4. verify the resulting SPIFFS image would fit
    # 5. copy the finished .u8 file into data/
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
    if not source.exists():
        return fail(f"Source file does not exist: {source}")
    if source.suffix.lower() != SUPPORTED_EXTENSION:
        return fail(f"Only {SUPPORTED_EXTENSION} files are supported. Got: {source.name}")

    ffmpeg_path = args.ffmpeg or os.environ.get("DEMO_FFMPEG") or shutil.which("ffmpeg")
    if not ffmpeg_path:
        return fail("ffmpeg is required for conversion but was not found on PATH")

    mkspiffs_path = locate_tool(args.mkspiffs, project_root, ["mkspiffs_espressif32_espidf", "mkspiffs"])
    spiffs_size = read_spiffs_partition_size(partitions_path)
    output_name = sanitize_output_name(args.output_name or source.name)
    output_path = data_dir / output_name

    with tempfile.TemporaryDirectory(prefix="rf3-audio-convert-") as temp_root:
        converted_path = Path(temp_root) / output_name
        ok, details = run_ffmpeg(ffmpeg_path, source, converted_path)
        if not ok:
            return fail(details)
        if not converted_path.exists() or converted_path.stat().st_size == 0:
            return fail("ffmpeg did not produce a usable output file")

        stage_dir = stage_data_directory(
            data_dir=data_dir,
            staged_name=output_name,
            converted_file=converted_path,
            replace_existing_audio=args.replace_existing_audio,
        )

        try:
            fits, fit_details = validate_spiffs_fit(stage_dir, spiffs_size, mkspiffs_path)
            if not fits:
                return fail(fit_details)
            shutil.copy2(converted_path, output_path)
        finally:
            # Temporary staging data is disposable regardless of success/failure.
            shutil.rmtree(stage_dir.parent, ignore_errors=True)

    converted_bytes = output_path.stat().st_size
    print(f"Prepared {output_path.name}")
    print(f"  Source:   {source}")
    print(f"  Output:   {output_path}")
    print(f"  Size:     {human_bytes(converted_bytes)}")
    print(f"  Duration: {converted_bytes / SAMPLE_RATE_HZ:.1f} s")
    print(f"  Fit:      {fit_details}")
    print()
    print("Next steps:")
    print("  1. Upload the filesystem image to the ESP32.")
    print("  2. Flash the firmware if needed.")
    print(f"  3. In the serial monitor, use FILES and then TX {output_path.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
