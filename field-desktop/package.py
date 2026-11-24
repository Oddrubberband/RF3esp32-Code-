"""Package the Windows app and test the actual extracted executable."""
from pathlib import Path
import hashlib
import os
import shutil
import subprocess
import tempfile
import time
import zipfile

from PyInstaller.archive.readers import CArchiveReader

ROOT = Path(__file__).resolve().parent
PRODUCT = "RF3-Field-R20-Windows-x64.zip"


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
            proc = subprocess.Popen(args, cwd=folder, env=env)
            try:
                for _ in range(60):
                    if proc.poll() is not None:
                        raise RuntimeError(f"{backend} startup failed: {proc.returncode}")
                    if any("GUI build=RF3-FIELD-R20" in f.read_text(errors="replace")
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
    dist = ROOT / "dist/RF3Desktop"
    output = ROOT / "release"
    output.mkdir(exist_ok=True)
    archive = output / PRODUCT
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
        for f in sorted(dist.rglob("*")):
            if f.is_file():
                z.write(f, f.relative_to(dist).as_posix())
        for name in ("START-HERE.txt", "Practice.cmd"):
            z.write(ROOT / name, name)
    with tempfile.TemporaryDirectory() as temp, zipfile.ZipFile(archive) as z:
        if z.testzip() is not None:
            raise ValueError("ZIP integrity failure")
        z.extractall(temp)
        check_exe(Path(temp))
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    (output / "SHA256SUMS.txt").write_text(f"{digest}  {PRODUCT}\n", encoding="ascii")
    print(f"{archive.stat().st_size} bytes, SHA256 {digest}")


if __name__ == "__main__":
    main()
