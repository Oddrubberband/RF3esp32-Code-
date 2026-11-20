# Build and verify RF3

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
