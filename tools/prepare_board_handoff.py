"""Build and inspect RF3 bench images. Never connects to, erases, or flashes a board."""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import zlib
from datetime import datetime, timezone
from pathlib import Path

from stage_demo_file import parse_partition_size, validate_transfer_size

ROOT = Path(__file__).resolve().parent.parent
PROFILES = {
    "rf3_custom_pcb": {"id": 1, "flash_mb": 16, "ce": 17, "csn": 27, "irq": 16},
    "rf3_esp32_devboard": {"id": 2, "flash_mb": 4, "ce": 27, "csn": 5, "irq": 26},
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def inventory(directory: Path) -> dict:
    result = {}
    for path in sorted(directory.iterdir()):
        require(path.is_file(), f"Expected a flat file directory: {path}")
        size, crc, sha = 0, 0, hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(65536), b""):
                size += len(chunk)
                crc = zlib.crc32(chunk, crc)
                sha.update(chunk)
        result[path.name] = {"bytes": size, "crc32": f"{crc:08X}", "sha256": sha.hexdigest()}
    return result


def parse_layout(text: str, flash_bytes: int) -> dict:
    partitions = {}
    for row in csv.reader(line for line in text.splitlines() if line.strip() and not line.lstrip().startswith("#")):
        require(len(row) >= 5, "Malformed partition entry")
        name, kind, subtype, offset, size = (value.strip() for value in row[:5])
        require(name not in partitions, "Duplicate partition name")
        require(bool(offset), "Explicit partition offsets are required for the RF3 handoff")
        start, length = parse_partition_size(offset), parse_partition_size(size)
        require(start >= 0x9000 and length > 0 and start + length <= flash_bytes, "Partition exceeds selected flash")
        partitions[name] = {"type": kind, "subtype": subtype, "offset": start, "size": length}
    ordered = sorted(partitions.values(), key=lambda item: item["offset"])
    require(all(a["offset"] + a["size"] <= b["offset"] for a, b in zip(ordered, ordered[1:])),
            "Partition overlap")
    require("factory" in partitions and "spiffs" in partitions, "Missing application or SPIFFS partition")
    require(partitions["factory"]["type"] == "app" and partitions["factory"]["subtype"] == "factory",
            "Unexpected factory partition type")
    require(partitions["spiffs"]["type"] == "data" and partitions["spiffs"]["subtype"] == "spiffs",
            "Unexpected SPIFFS partition type")
    return partitions


def check_spiffs_config(sdk: dict, tool_version: str) -> None:
    expected = {"SPIFFS_OBJ_NAME_LEN": sdk["CONFIG_SPIFFS_OBJ_NAME_LEN"],
                "SPIFFS_OBJ_META_LEN": sdk["CONFIG_SPIFFS_META_LENGTH"],
                "SPIFFS_USE_MAGIC": "1" if sdk.get("CONFIG_SPIFFS_USE_MAGIC") == "y" else "0",
                "SPIFFS_USE_MAGIC_LENGTH": "1" if sdk.get("CONFIG_SPIFFS_USE_MAGIC_LENGTH") == "y" else "0",
                "SPIFFS_ALIGNED_OBJECT_INDEX_TABLES": "0"}
    for key, value in expected.items():
        require(re.search(rf"^\s*{key}:\s*{value}\s*$", tool_version, re.MULTILINE) is not None,
                f"mkspiffs configuration does not match firmware: {key}={value}")
    require(sdk["CONFIG_SPIFFS_PAGE_SIZE"] == "256", "Unexpected SPIFFS page size")


def check_flags(flags: list[str], profile: dict) -> None:
    expected = {"RF3_HARDWARE_PROFILE_ID": profile["id"], "RF3_WIFI_CONTROL_ENABLED": 0,
                "RF3_NRF24_TX_CE_PULSE_US": 40,
                "NRF24_CE_PIN": profile["ce"], "NRF24_CSN_PIN": profile["csn"],
                "NRF24_IRQ_PIN": profile["irq"], "NRF24_SCK_PIN": 18,
                "NRF24_MOSI_PIN": 23, "NRF24_MISO_PIN": 19}
    for name, value in expected.items():
        definitions = [flag for flag in flags if flag.startswith(f"-D{name}=")]
        require(definitions == [f"-D{name}={value}"], f"Unexpected or conflicting board flag: {name}")


def shell_command(args: list[str]) -> str:
    if os.name == "nt":
        return "& " + " ".join("'" + arg.replace("'", "''") + "'" for arg in args)
    return shlex.join(args)


def run(command: list[str], log: Path, env: dict | None = None) -> str:
    result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, timeout=600)
    log.write_text(result.stdout + result.stderr, encoding="utf-8")
    require(result.returncode == 0, f"Command failed; inspect {log.relative_to(ROOT)}")
    return result.stdout


def build_profile(name: str, output: Path, config, env: dict) -> dict:
    profile = PROFILES[name]
    directory = output / name
    directory.mkdir()
    print(f"Building and checking {name} (no board access)...", flush=True)
    run([sys.executable, "-m", "platformio", "run", "-e", name], directory / "firmware-build.log", env)
    run([sys.executable, "-m", "platformio", "run", "-e", name, "-t", "buildfs"], directory / "canonical-buildfs.log", env)
    build = ROOT / ".pio/build" / name
    description = json.loads((build / "project_description.json").read_text(encoding="utf-8"))
    sdk = dict(line.split("=", 1) for line in Path(description["config_file"]).read_text().splitlines()
               if line.startswith("CONFIG_") and "=" in line)
    section = f"env:{name}"
    flags = config.get(section, "build_flags")
    check_flags(flags, profile)
    require(sdk["CONFIG_ESPTOOLPY_FLASHSIZE"].strip('"') == f"{profile['flash_mb']}MB", "Wrong generated flash size")
    require(sdk["CONFIG_ESPTOOLPY_FLASHMODE"] == '"dio"' and sdk["CONFIG_ESPTOOLPY_FLASHFREQ"] == '"40m"',
            "Unexpected generated flash mode/frequency")
    require(int(sdk["CONFIG_PARTITION_TABLE_OFFSET"], 0) == 0x8000, "Unexpected partition table offset")
    packages = Path(description["idf_path"]).parent
    esptool = packages / "tool-esptoolpy/esptool.py"
    mkspiffs_name = "mkspiffs_espressif32_espidf" + (".exe" if os.name == "nt" else "")
    mkspiffs = packages / "tool-mkspiffs" / mkspiffs_name
    version = run([str(mkspiffs), "--version"], directory / "mkspiffs-version.log")
    check_spiffs_config(sdk, version)
    decoded = run([sys.executable, str(Path(description["idf_path"]) / "components/partition_table/gen_esp32part.py"),
                   str(build / "partitions.bin")], directory / "decoded-partitions.csv")
    layout = parse_layout(decoded, profile["flash_mb"] * 1024 * 1024)
    declared = parse_layout((ROOT / config.get(section, "board_build.partitions")).read_text(),
                            profile["flash_mb"] * 1024 * 1024)
    require(layout == declared, "Built partition table differs from selected source table")
    files = directory / "images"
    files.mkdir()
    for filename in ("bootloader.bin", "partitions.bin", "firmware.bin"):
        shutil.copyfile(build / filename, files / filename)
    require(0 < (files / "bootloader.bin").stat().st_size <= 0x8000 - 0x1000, "Bootloader exceeds reserved region")
    require(0 < (files / "partitions.bin").stat().st_size <= 0x1000, "Partition table exceeds reserved region")
    require(0 < (files / "firmware.bin").stat().st_size <= layout["factory"]["size"], "Application does not fit")
    for filename in ("bootloader.bin", "firmware.bin"):
        info = run([sys.executable, str(esptool), "--chip", "esp32", "image_info", str(files / filename)],
                   directory / f"{filename}.info.log")
        require(re.search(r"Checksum: .*\(valid\)", info) is not None and
                re.search(r"Validation Hash: .*\(valid\)", info) is not None, f"Invalid ESP32 image: {filename}")
    role_inventories = {}
    for role in ("sender", "receiver"):
        data = directory / f"{role}-data"
        data.mkdir()
        if role == "sender":
            for source in sorted((ROOT / "data").iterdir()):
                require(source.is_file(), f"Handoff expects flat staged files: {source.name}")
                if not source.name.endswith(".part"):
                    require(source.name.isascii() and len(source.name) <= 31, "Invalid SPIFFS filename")
                    require(validate_transfer_size(source.stat().st_size)[0], "Staged file exceeds protocol maximum")
                    shutil.copyfile(source, data / source.name)
        # Test vector is isolated; data/ and previous packages remain untouched.
        one = data / "one.bin"
        if one.exists():
            require(one.read_bytes() == b"\x5a", "data/one.bin differs from the bench vector; refusing to replace it")
        else:
            one.write_bytes(b"\x5a")
        image = files / f"spiffs-{role}.bin"
        common = ["-b", "4096", "-p", "256", "-s", str(layout["spiffs"]["size"])]
        run([str(mkspiffs), "-c", str(data), *common, str(image)], directory / f"{role}-pack.log")
        require(image.stat().st_size == layout["spiffs"]["size"], "SPIFFS image has wrong length")
        extracted = directory / f"{role}-unpacked"
        extracted.mkdir()
        # mkspiffs 0.2.3 prefixes './' to -u paths, so use a relative path
        # with forward slashes (absolute Windows drive paths fail in that tool).
        run([str(mkspiffs), "-u", extracted.relative_to(ROOT).as_posix(), *common, str(image)],
            directory / f"{role}-unpack.log")
        role_inventories[role] = inventory(data)
        require(role_inventories[role] == inventory(extracted), "SPIFFS pack/unpack content mismatch")
    # A planning budget, not a claim about measured free space or flash latency.
    reserve = int(layout["spiffs"]["size"] * 0.75) - sum(x["bytes"] for x in role_inventories["receiver"].values())
    require(reserve >= 1310720 + 65536, "Receiver planning budget cannot accommodate a maximum transfer")
    reset_before = config.get(section, "board_upload.before_reset", "default_reset")
    require(reset_before == ("no_reset" if profile["id"] == 1 else "default_reset"), "Unexpected reset policy")
    tool = [os.path.relpath(sys.executable, ROOT), os.path.relpath(esptool, ROOT), "--chip", "esp32",
            "--port", "COM_REPLACE_ME" if os.name == "nt" else "/dev/REPLACE_ME", "--baud", "115200",
            "--before", reset_before, "--after", "hard_reset"]
    flash_commands = {}
    for role in ("sender", "receiver"):
        command = tool + ["write_flash", "--flash_mode", "dio", "--flash_freq", "40m",
                          "--flash_size", f"{profile['flash_mb']}MB"]
        for offset, filename in ((0x1000, "bootloader.bin"), (0x8000, "partitions.bin"),
                                 (layout["factory"]["offset"], "firmware.bin"),
                                 (layout["spiffs"]["offset"], f"spiffs-{role}.bin")):
            command.extend([hex(offset), os.path.relpath(files / filename, ROOT)])
        flash_commands[role] = shell_command(command)
    instructions = [f"# {name}: prepared image commands (NOT executed)", "",
                    "Run from the repository root only after the electrical/boot gates in docs/board_bringup.md.",
                    "Replace the port placeholder with a positively identified board port. NEVER guess COM1.",
                    "The custom PCB requires GPIO0 low during EN reset before EACH esptool command.",
                    "Release BOOT for normal execution; reset manually after flashing if DTR/RTS cannot do so.",
                    "Back up wanted board data first: these writes REPLACE its filesystem contents.", "",
                    "Read the flash identity first; stop if the physical flash size differs from this profile:",
                    "```", shell_command(tool + ["flash_id"]), "```"]
    for role, command in flash_commands.items():
        instructions.extend(["", f"## {role} image set", "", "```", command, "```"])
    instructions.extend(["", "Sender includes staged fixtures + one.bin; receiver contains only one.bin.",
                         "Start with receiver RX, then sender TX one.bin; both use CHANNEL 76 and POWER 0.",
                         "Do not use the default uploadfs command for the lean receiver image.",
                         "Image checks establish host artifacts only; live SPIFFS/RF/PCB validation is pending.", ""])
    (directory / "FLASH_INSTRUCTIONS.md").write_text("\n".join(instructions), encoding="utf-8")
    return {"profile": profile, "build_flags": flags, "partitions": layout,
            "reset_before": reset_before, "images": inventory(files), "contents": role_inventories,
            "receiver_75_percent_payload_budget_bytes": reserve, "flash_commands_not_executed": flash_commands}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--environment", choices=list(PROFILES), action="append", help="Default: both canonical profiles")
    parser.add_argument("--core-dir", type=Path, help="PlatformIO package cache; otherwise use environment/local/default cache")
    args = parser.parse_args()
    env = os.environ.copy()
    core = args.core_dir or (Path(env["PLATFORMIO_CORE_DIR"]) if env.get("PLATFORMIO_CORE_DIR") else None)
    if core is None and (ROOT / ".pio/core/platforms").is_dir():
        core = ROOT / ".pio/core"
    if core is not None:
        env["PLATFORMIO_CORE_DIR"] = str(core.resolve())
        os.environ["PLATFORMIO_CORE_DIR"] = str(core.resolve())
    parent = ROOT / ".pio/board_handoff"
    parent.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="run-", dir=parent))
    manifest = {"created_utc": datetime.now(timezone.utc).isoformat(), "hardware_validated": False,
                "status": "INCOMPLETE", "profiles": {}}
    try:
        from platformio.project.config import ProjectConfig

        config = ProjectConfig.get_instance(str(ROOT / "platformio.ini"))
        manifest["git_head"] = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
        manifest["git_status"] = subprocess.check_output(["git", "status", "--short"], cwd=ROOT, text=True)
        for name in dict.fromkeys(args.environment or PROFILES):
            manifest["profiles"][name] = build_profile(name, output, config, env)
        manifest["status"] = "HOST_ARTIFACT_CHECKS_PASS"
    except (OSError, ValueError, KeyError, ImportError, subprocess.SubprocessError) as error:
        manifest["error"] = str(error)
        print(f"HANDOFF PREPARATION FAILED: {error}", file=sys.stderr)
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Handoff: {output.relative_to(ROOT)}")
    print(f"Result: {manifest['status']}; no device was connected, erased, or flashed.")
    return 0 if manifest["status"] == "HOST_ARTIFACT_CHECKS_PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
