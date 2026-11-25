"""Prepare portable, checksum-verified RF3 release archives; never flash or publish."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
ENVIRONMENTS = ("rf3_custom_pcb", "rf3_esp32_devboard")
IMAGE_NAMES = ("bootloader.bin", "partitions.bin", "firmware.bin")
GUI_ARCHIVE = "RF3-Desktop-Windows.zip"
FIRMWARE_ARCHIVE = "RF3-Firmware.zip"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def json_bytes(value: object) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")


def release_identity(root: Path, commit: str | None = None) -> tuple[str, str]:
    config = json.loads((root / "CURRENT_RELEASE.json").read_text(encoding="utf-8"))
    tag = config.get("release_tag")
    if not isinstance(tag, str) or re.fullmatch(r"v[0-9][A-Za-z0-9._-]{0,95}", tag) is None:
        raise ValueError("CURRENT_RELEASE.json must contain a safe version release_tag")
    commit = commit or os.environ.get("GITHUB_SHA")
    if not commit:
        top = subprocess.run(["git", "rev-parse", "--show-toplevel"], cwd=root, check=True,
                             capture_output=True, text=True).stdout.strip()
        if Path(top).resolve() != root.resolve():
            raise ValueError("Packaging root is not the Git checkout root; provide --commit explicitly")
        commit = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root, check=True,
                                capture_output=True, text=True).stdout.strip()
    if not isinstance(commit, str) or re.fullmatch(r"[0-9a-fA-F]{40}", commit) is None:
        raise ValueError("An exact 40-character source commit is required")
    return tag, commit.lower()


def checked_file(path: Path) -> Path:
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"Required regular build file is missing: {path.name}")
    return path


def zip_bytes(source: Path | bytes) -> bytes:
    return source.read_bytes() if isinstance(source, Path) else source


def write_archive(destination: Path, members: dict[str, Path | bytes]) -> dict[str, str]:
    """Write only an explicitly selected payload; publish its name exclusively."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    hashes = {}
    with tempfile.NamedTemporaryFile(prefix=".rf3-package-", suffix=".zip",
                                     dir=destination.parent, delete=False) as temporary:
        temporary_path = Path(temporary.name)
    try:
        with zipfile.ZipFile(temporary_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for name, source in sorted(members.items()):
                if name.startswith("/") or ".." in Path(name).parts or "\\" in name:
                    raise ValueError("Unsafe archive member name")
                data = zip_bytes(source)
                info = zipfile.ZipInfo(name, date_time=(2000, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o100644 << 16
                archive.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
                hashes[name] = sha256(data)
        with zipfile.ZipFile(temporary_path) as archive:
            if archive.testzip() is not None:
                raise ValueError("Archive integrity check failed")
            for name, digest in hashes.items():
                if sha256(archive.read(name)) != digest:
                    raise ValueError("Archive member changed during packaging")
        # Exclusive creation works on restricted Windows accounts too. Build
        # metadata is emitted only after the complete verified copy succeeds.
        created = False
        try:
            with destination.open("xb") as target, temporary_path.open("rb") as source:
                created = True
                shutil.copyfileobj(source, target)
                target.flush()
                os.fsync(target.fileno())
        except Exception:
            if created:
                destination.unlink(missing_ok=True)
            raise
    finally:
        temporary_path.unlink(missing_ok=True)
    return hashes


def package(root: Path, output: Path, kind: str, *, commit: str | None = None,
            gui_dir: Path | None = None, firmware_root: Path | None = None) -> list[Path]:
    tag, commit = release_identity(root, commit)
    if kind not in {"gui", "firmware", "all"}:
        raise ValueError("Package kind must be gui, firmware or all")
    build_info = json_bytes({"schema": 1, "release_tag": tag, "source_commit": commit})
    plans: list[tuple[str, str, dict[str, Path | bytes]]] = []
    if kind in {"gui", "all"}:
        distribution = gui_dir or root / "dist" / "RF3Desktop"
        exe = checked_file(distribution / "RF3Desktop.exe")
        internal = distribution / "_internal"
        if internal.is_symlink() or not internal.is_dir():
            raise ValueError("The complete PyInstaller _internal directory is required")
        members: dict[str, Path | bytes] = {"RF3Desktop/RF3Desktop.exe": exe}
        for path in sorted(internal.rglob("*")):
            if path.is_symlink():
                raise ValueError("Symbolic links are not allowed in a portable GUI package")
            if path.is_file():
                members["RF3Desktop/" + path.relative_to(distribution).as_posix()] = path
        if len(members) == 1:
            raise ValueError("The PyInstaller _internal directory is empty")
        members["Launch_GUI.cmd"] = (b'@echo off\r\nsetlocal\r\ncd /d "%~dp0"\r\n'
                                      b'start "" "RF3Desktop\\RF3Desktop.exe" --backend serial\r\n')
        members["README.txt"] = ("RF3 Desktop for Windows\n\nExtract the entire ZIP to a folder. "
                                  "Keep RF3Desktop.exe together with its _internal directory. "
                                  "Open Launch_GUI.cmd; select the intended board and transport. "
                                  "Each board needs its matching firmware. USB power and Bluetooth "
                                  "control can be used together. Firmware flashing remains manual.\n"
                                  f"Version: {tag}\nSource commit: {commit}\n").encode("utf-8")
        members["BUILD_INFO.json"] = build_info
        plans.append(("gui", GUI_ARCHIVE, members))
    if kind in {"firmware", "all"}:
        builds = firmware_root or root / ".pio" / "build"
        members = {}
        images = {}
        for environment in ENVIRONMENTS:
            images[environment] = {}
            for name in IMAGE_NAMES:
                path = checked_file(builds / environment / name)
                if path.stat().st_size == 0:
                    raise ValueError("Firmware images must not be empty")
                members[f"firmware/{environment}/{name}"] = path
                images[environment][name] = sha256(path.read_bytes())
        members["Flash_Manual.ps1"] = checked_file(root / "Flash_Manual.ps1")
        members["Firmware_Images.json"] = json_bytes({"Current": images, "source_commit": commit,
                                                       "release_tag": tag})
        members["BUILD_INFO.json"] = build_info
        members["README.txt"] = ("RF3 firmware: manual flashing only\n\nExtract this ZIP. Connect the "
                                  "intended board by USB and close programs using its USB serial port. "
                                  "In PowerShell run:\n  .\\Flash_Manual.ps1 -Board custom -Port COMn\n"
                                  "or choose -Board devboard. Replace COMn with that board's USB port. "
                                  "Use the helper's manual BOOT/EN prompts; reset manually afterward. "
                                  "Do not use a Bluetooth COM port for flashing. The board profiles "
                                  "have different pins and flash sizes. This package contains only "
                                  "bootloader, partition and application images; it does not include "
                                  "a SPIFFS image or perform a full erase.\n"
                                  f"Version: {tag}\nSource commit: {commit}\n").encode("utf-8")
        plans.append(("firmware", FIRMWARE_ARCHIVE, members))
    output.mkdir(parents=True, exist_ok=True)
    # Refuse an accidental rebuild over a previous candidate before writing any package.
    for item_kind, filename, _ in plans:
        if (output / filename).exists() or (output / f"{item_kind.upper()}-BUILD.json").exists():
            raise FileExistsError("Output already exists; select an empty packaging folder")
    result = []
    for item_kind, filename, members in plans:
        target = output / filename
        member_hashes = write_archive(target, members)
        manifest = {"schema": 1, "kind": item_kind, "source_commit": commit,
                    "release_tag": tag, "archive": filename,
                    "sha256": sha256(target.read_bytes()), "size": target.stat().st_size,
                    "members": member_hashes}
        manifest_path = output / f"{item_kind.upper()}-BUILD.json"
        with manifest_path.open("xb") as stream:
            stream.write(json_bytes(manifest))
        result += [target, manifest_path]
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "release-dist")
    parser.add_argument("--kind", choices=("gui", "firmware", "all"), default="all")
    parser.add_argument("--commit")
    parser.add_argument("--gui-dir", type=Path)
    parser.add_argument("--firmware-root", type=Path)
    args = parser.parse_args(argv)
    for path in package(args.root, args.output_dir, args.kind, commit=args.commit,
                        gui_dir=args.gui_dir, firmware_root=args.firmware_root):
        print(path.name)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
