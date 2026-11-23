"""Validate both PlatformIO outputs and stage the manual flash package."""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
TARGETS = (
    ("custom-pcb-usb-bench","rf3_custom_usb_bench","RF3-NAG-C1-20261122"),
    ("devboard-ebyte","rf3_usb_ebyte","RF3-NAG-E1-20261122"),
)
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build",action="store_true")
    args = parser.parse_args()
    for folder, env, version in TARGETS:
        base = ROOT/folder
        if args.build:
            subprocess.run([sys.executable,"-m","platformio","run","-d",str(base),"-e",env],check=True)
        source = base/".pio/build"/env
        target = base/"firmware"/env
        target.mkdir(exist_ok=True,parents=True)
        hashes = {}
        for name in ("bootloader.bin","partitions.bin","firmware.bin"):
            raw = (source/name).read_bytes()
            if name != "partitions.bin":
                if raw[0]!=0xe9 or raw[23]!=1 or hashlib.sha256(raw[:-32]).digest()!=raw[-32:]:
                    raise ValueError(f"{env}/{name}: invalid ESP image or appended SHA256")
            if name == "firmware.bin":
                if raw[48:80].split(b"\0")[0].decode()!=version or len(raw)>0x180000:
                    raise ValueError(f"{env}: wrong application version or size")
                if version.encode() not in raw[256:]:
                    raise ValueError(f"{env}: missing runtime build identity")
            if name == "partitions.bin":
                entries = {raw[p+12:p+28].split(b"\0")[0]: (int.from_bytes(raw[p+4:p+8],"little"),int.from_bytes(raw[p+8:p+12],"little"))
                           for p in range(0,len(raw)-31,32) if raw[p:p+2]==b"\xaa\x50"}
                if entries.get(b"factory")!=(0x10000,0x180000) or entries.get(b"spiffs")!=(0x190000, 0xe70000 if folder.startswith("custom") else 0x270000):
                    raise ValueError(f"{env}: partition mismatch")
            (target/name).write_bytes(raw)
            hashes[name] = hashlib.sha256(raw).hexdigest().upper()
        (base/"Firmware_Images.json").write_text(json.dumps({"Current":{env:hashes},"Build":version},indent=2))
        print(env,version,hashes)
if __name__=="__main__":
    main()
