from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
MACHINE_PATH_EXCLUSIONS = {
    "docs/codex_repository_audit.md",
    "docs/integration_readiness_report.md",
}
ALLOWED_SDKCONFIG = {"sdkconfig.defaults", "sdkconfig.devboard.defaults"}
SECRET_PATTERNS = {
    "GitHub token": re.compile(r"\bgh[opsu]_[A-Za-z0-9]{30,}\b"),
    "AWS access key": re.compile(r"\bAKIA[0-9A-Z]{16}\b"),
    "private key": re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
    "embedded Wi-Fi credential": re.compile(
        r'(?m)^\s*#define\s+RF3_WIFI_(?:SSID|PASSWORD)\s+'
        r'"(?!YOUR_WIFI_(?:SSID|PASSWORD)"\s*$)[^"\r\n]+"\s*$'
    ),
}
MACHINE_PATTERNS = {
    "user-specific path": re.compile(r"[A-Za-z]:\\Users\\[^\\\r\n]+"),
    "hard-coded upload/monitor port": re.compile(
        r"(?im)^\s*(?:upload_port|monitor_port)\s*=\s*COM\d+\s*$"
    ),
}


def tracked_files() -> list[str]:
    result = subprocess.run(
        ["git", "ls-files", "-z"],
        cwd=ROOT,
        check=True,
        capture_output=True,
    )
    return [item.decode("utf-8") for item in result.stdout.split(b"\0") if item]


def main() -> int:
    failures: list[str] = []
    paths = tracked_files()
    generated = [
        path
        for path in paths
        if Path(path).name.startswith("sdkconfig.")
        and Path(path).name not in ALLOWED_SDKCONFIG
    ]
    if generated:
        failures.append("tracked generated sdkconfig: " + ", ".join(generated))

    for relative in paths:
        path = ROOT / relative
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue

        for label, pattern in SECRET_PATTERNS.items():
            if pattern.search(text):
                failures.append(f"{relative}: possible {label}")
        if relative not in MACHINE_PATH_EXCLUSIONS:
            machine_text = text.replace("\\\\", "\\")
            for label, pattern in MACHINE_PATTERNS.items():
                if pattern.search(machine_text):
                    failures.append(f"{relative}: {label}")

    if failures:
        print("Repository hygiene check failed:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print("Repository hygiene check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
