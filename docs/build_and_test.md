# Build and verify the current implementation

Run commands from the repository root. Firmware uses ESP-IDF through PlatformIO; the desktop GUI requires Windows for its Bluetooth SPP backend. Python 3.11 is used by release CI.

```powershell
py -3.11 -m venv .venv-host
.\.venv-host\Scripts\python.exe -m pip install -r host_gui\requirements.txt -r requirements.txt
$env:PYTHONPATH = "$PWD\host_gui"
$env:QT_QPA_PLATFORM = 'offscreen'
.\.venv-host\Scripts\python.exe -m unittest discover -s host_gui/tests -v
.\.venv-host\Scripts\python.exe -m unittest discover -s test -p 'test_*.py' -v
.\.venv-host\Scripts\python.exe -m platformio test -e native
.\.venv-host\Scripts\python.exe -m platformio run -e rf3_custom_pcb -e rf3_esp32_devboard
.\.venv-host\Scripts\python.exe -m PyInstaller --noconfirm RF3Desktop.spec
```

Native tests require a C++ compiler available as `g++`. The production-method harnesses below compile the selected production methods with controlled platform adapters. They supplement the native suite; they are not RF hardware tests.

```powershell
python tools/tests/test_host_runtime_contract.py --output .test-output/runtime
python tools/tests/test_rx_command_guard.py --output .test-output/rx
python tools/tests/test_file_type_metadata.py --output .test-output/extensions
python tools/check_repository_hygiene.py
```

Use `--help` on a harness to select its compiler or retain evidence. The old flag-zero codec used for compatibility checks is bundled under `test/fixtures/legacy_v2`; no previous private checkout is needed.

## Portable packages

After building, use the packaging tool. In a Git checkout it records the exact HEAD; an extracted source tree requires `--commit` with its exact source commit. Do not label locally modified code with an unrelated commit.

```powershell
python tools/package_current_release.py --help
```

GitHub validation builds both firmware profiles on Linux and tests/packages the GUI on Windows. A release runs only after successful validation on an explicitly marked main push or manual dispatch. The release contains a portable Windows GUI, both sets of firmware images, exact-source manifests and SHA-256 checksums. Generated packages are not committed to source control.

## Manual programming

The Windows manual helper expects the VS Code PlatformIO extension and its default user-profile Python/esptool installation. Install that extension before using the helper; the virtual environment above is for builds and tests. Clear `$env:QT_QPA_PLATFORM` after offscreen tests before launching the GUI interactively.

Use the complete firmware release package and its `Flash_Manual.ps1`; its image hash manifest must match the three selected images. Select the correct board and its USB programming port, not its Bluetooth port. BOOT and EN remain manual before and after flashing. See the root README for the sequence. A normal application update does not erase or upload SPIFFS.

For rollback, extract a previous complete release to a separate directory. Launch its GUI directly or pass that directory explicitly with `-Version Previous -PreviousRoot`. Preserve local files separately: changing firmware does not undo SPIFFS mutations.

The current verification record is [current_verification.md](verification/current_verification.md), with each change and its evidence in [CHANGELOG.md](../CHANGELOG.md).
