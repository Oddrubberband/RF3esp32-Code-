"""Assemble a clean two-board flash kit after the build and hash checks."""
import hashlib
import json
import shutil
from pathlib import Path
from pack_images import ROOT, TARGETS


def main():
    dest = ROOT / "flash-kit"
    dest.mkdir(exist_ok=True)
    shutil.copy2(ROOT / "START_HERE.md", dest)
    for folder, env, build in TARGETS:
        src = ROOT / folder
        out = dest / folder
        fw = out / "firmware" / env
        fw.mkdir(parents=True, exist_ok=True)
        manifest = json.loads((src / "Firmware_Images.json").read_bytes())
        if manifest["Build"] != build:
            raise ValueError(f"Unexpected ID for {folder}")
        expected = manifest["Current"][env]
        for name in ("bootloader.bin", "partitions.bin", "firmware.bin"):
            image = src / "firmware" / env / name
            if hashlib.sha256(image.read_bytes()).hexdigest().upper() != expected[name]:
                raise ValueError(f"Hash mismatch for {folder}/{name}")
            shutil.copy2(image, fw)
        for name in ("Firmware_Images.json", "Flash_Manual.ps1",
                     "Flash_Custom_PCB.cmd" if folder.startswith("custom") else "Flash_Devboard.cmd"):
            shutil.copy2(src / name, out)
    print("Verified and packaged both board image sets.", flush=True)


if __name__ == "__main__":
    main()
