"""Package the Windows app and test the actual extracted executable."""
import argparse
from pathlib import Path
import hashlib
import os
import subprocess
import tempfile
import time
import zipfile

from PyInstaller.archive.readers import CArchiveReader

ROOT = Path(__file__).resolve().parent
PRODUCT = "RF3-Field-R24-Windows-x64.zip"


def check_exe(folder):
    exe = folder / "RF3Desktop.exe"
    modules = CArchiveReader(str(exe)).open_embedded_archive("PYZ.pyz")
    for source in (ROOT / "host_gui/rf3_gui").rglob("*.py"):
        parts = source.relative_to(ROOT / "host_gui").with_suffix("").parts
        name = ".".join(parts[:-1] if parts[-1] == "__init__" else parts)
        actual = modules.extract(name)
        if actual is None or actual != compile(source.read_bytes(), actual.co_filename, "exec", dont_inherit=True):
            raise ValueError(f"Executable differs from GUI source: {name}")
    for backend in ("mock", "serial"):
        with tempfile.TemporaryDirectory() as logs:
            env = dict(os.environ, QT_QPA_PLATFORM="offscreen", RF3_LOG_DIR=logs)
            args = [exe, "--backend", "mock"] if backend == "mock" else [exe]
            proc = subprocess.Popen(args, cwd=folder, env=env, creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                for _ in range(60):
                    if proc.poll() is not None:
                        raise RuntimeError(f"{backend} startup failed: {proc.returncode}")
                    if any("GUI build=RF3-FIELD-R24" in f.read_text(errors="replace")
                           for f in Path(logs).glob("connection-*.log")):
                        break
                    time.sleep(.5)
                else:
                    raise RuntimeError(f"{backend} startup timed out")
                time.sleep(3)
                if proc.poll() is not None:
                    raise RuntimeError(f"{backend} exited during startup")
            finally:
                if proc.poll() is None:
                    proc.terminate()
                proc.wait(timeout=15)
        print(f"Extracted {backend} startup passed", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dist", type=Path, default=ROOT / "dist/RF3Desktop")
    parser.add_argument("--output", type=Path, default=ROOT / "release")
    args = parser.parse_args()
    dist, output = args.dist.resolve(), args.output.resolve()
    if not (dist / "RF3Desktop.exe").is_file() or not (dist / "_internal").is_dir():
        parser.error(f"Expected RF3Desktop.exe and _internal in {dist}")
    output.mkdir(parents=True, exist_ok=True)
    archive = output / PRODUCT
    with tempfile.TemporaryDirectory(dir=output) as temp:
        candidate = Path(temp) / PRODUCT
        with zipfile.ZipFile(candidate, "w", zipfile.ZIP_DEFLATED) as z:
            for f in sorted(dist.rglob("*")):
                if f.is_file():
                    z.write(f, f.relative_to(dist).as_posix())
            for name in ("START-HERE.txt", "Practice.cmd"):
                z.write(ROOT / name, name)
        extracted = Path(temp) / "extract"
        with zipfile.ZipFile(candidate) as z:
            if z.testzip() is not None:
                raise ValueError("ZIP integrity failure")
            z.extractall(extracted)
        check_exe(extracted)
        candidate.replace(archive)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    (output / "SHA256SUMS.txt").write_text(f"{digest}  {PRODUCT}\n", encoding="ascii")
    print(f"{archive.stat().st_size} bytes, SHA256 {digest}")


if __name__ == "__main__":
    main()
