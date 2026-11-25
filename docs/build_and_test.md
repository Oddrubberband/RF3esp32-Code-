# Build and verify RF3

## R28 firmware

Follow [the board setup guide](../START_HERE.md) for the current profiles, verified staging, and manual flash commands. The PlatformIO core version used by the build workflow is recorded in [requirements.txt](../requirements.txt).

The current source provides hardware-free Reliable resume, diagnostics and DATA retry tests in the board directories. With g++ on PATH, for example:

```powershell
python boards/custom-pcb-sense/tools/tests/test_resume_runtime.py --compiler g++ --output .test-output/custom-resume
python boards/devboard-ebyte/tools/tests/test_resume_runtime.py --compiler g++ --output .test-output/ebyte-resume
```

## R24 Field Desktop

From the repository root, open PowerShell:

```powershell
Set-Location field-desktop
py -3.11 -m pip install -r host_gui/requirements.txt
$env:QT_QPA_PLATFORM = 'offscreen'
Push-Location host_gui
py -3.11 -m unittest discover -s tests
Pop-Location
py -3.11 -m PyInstaller --noconfirm RF3Desktop.spec
py -3.11 package.py
```

The packager checks the source modules bundled in the executable and starts the extracted Windows program in both serial and Practice modes. Open `release/` for the portable ZIP and its checksum.

## November 20 legacy build

The following commands reproduce the November 20 audit release using the earlier root-level firmware and desktop. Use the current commands above for the R24/R28 pair.

Run from the source repository root. Firmware uses ESP-IDF through PlatformIO.
GUI dependencies include PySide6, pyserial and PyInstaller. Native tests require
g++ in PATH. CI uses Python3.11 and the pinned requirements files.

```powershell
py -3.11 -m venv .venv-host
.\.venv-host\Scripts\python.exe -m pip install -r requirements.txt -r host_gui/requirements.txt
$env:PYTHONPATH = "$PWD/host_gui"
$env:QT_QPA_PLATFORM = 'offscreen'
New-Item -ItemType Directory -Force '.test-output' | Out-Null
$env:TEMP = "$PWD/.test-output"
$env:TMP = $env:TEMP
.\.venv-host\Scripts\python.exe -m unittest discover -s host_gui/tests -v
.\.venv-host\Scripts\python.exe -m unittest discover -s test -p 'test_*.py' -v
.\.venv-host\Scripts\python.exe -m platformio test -e native
.\.venv-host\Scripts\python.exe tools/tests/test_host_runtime_contract.py --compiler g++ --output .test-output/runtime
.\.venv-host\Scripts\python.exe tools/tests/test_rx_command_guard.py --compiler g++ --output .test-output/rx
.\.venv-host\Scripts\python.exe tools/tests/test_file_type_metadata.py --compiler g++ --output .test-output/extensions
.\.venv-host\Scripts\python.exe tools/tests/test_no_ack_runtime.py --compiler g++ --output .test-output/noack
.\.venv-host\Scripts\python.exe tools/tests/test_resume_runtime.py --compiler g++ --output .test-output/resume
.\.venv-host\Scripts\python.exe tools/tests/test_diagnostics_runtime.py --compiler g++ --output .test-output/diagnostics
.\.venv-host\Scripts\python.exe -m platformio run -e rf3_custom_pcb
.\.venv-host\Scripts\python.exe -m platformio run -d profiles/usb-devboard -e rf3_usb_fem
.\.venv-host\Scripts\python.exe -m platformio test -d profiles/usb-devboard -e native
.\.venv-host\Scripts\python.exe -m PyInstaller --noconfirm RF3Desktop.spec
```

On Linux, use the corresponding virtual-environment Python executable and shell
syntax. CI also runs each tools/tests/host_*.cpp helper with strict compiler
warnings. The production-method harnesses use controlled clocks/platform adapters;
they are not RF or battery hardware tests. Inspect each command's exit status.

Clear QT_QPA_PLATFORM before interactive use. Launch_GUI.cmd in a built source
checkout starts dist/RF3Desktop/RF3Desktop.exe; a release ZIP has its own launcher.

## Portable releases

After checks/builds succeed, package from this exact Git checkout:

```powershell
.\.venv-host\Scripts\python.exe tools/package_current_release.py --kind all
```

Use a new empty --output-dir if an earlier package exists. A source export needs
--commit with its exact source commit; do not use a different commit for modified
code. The packager generates matching Firmware_Images.json inside the firmware
ZIP from actual build bytes. The repository's placeholder image manifest does
not authorize flashing locally rebuilt images without packaging/verification.

GitHub builds the firmware on Linux and the GUI on Windows. The existing trusted
main release workflow publishes only after both jobs pass and release is explicitly
selected. Publication checks both bundles' commit/tag identity and every hash.

Manual flashing remains separate: use the complete firmware ZIP's helper, USB
port selection and BOOT/EN prompts. No test/build command above opens hardware.
