# Build and test RF3

RF3 uses PlatformIO Core 6.1.19 and pins the Espressif32 platform to 7.0.1.
Run commands from the repository root. Build output and generated sdkconfig
files remain under the ignored .pio directory. The only active repository root
on this machine is the direct path below; no directory junction is required:

```powershell
Set-Location 'C:\Users\Pare\Desktop\SCHOOL\fall_2026\404\CodE_project\RF3esp32-Code-clean'
```

Documentation-only edits do not trigger any of these builds or packaging.
When code validation or delivery requires a build, reuse the existing output
directories. GUI packaging uses only `RF3Desktop.spec`, updates
`dist/RF3DesktopFixed/` in place, and reuses `build/RF3Desktop/`:

```powershell
.\.venv-host\Scripts\python.exe -m PyInstaller --noconfirm --distpath dist --workpath build RF3Desktop.spec
```

## Native validation

    platformio test -e native -v
    python -m unittest discover -s test -p "test_*.py"
    git diff --check

The native environment requires a host C and C++ compiler (gcc/g++, Clang, or
an appropriately configured MSVC toolchain).

## Firmware

    platformio run -e rf3_custom_pcb -t clean
    platformio run -e rf3_custom_pcb
    platformio run -e rf3_esp32_devboard -t clean
    platformio run -e rf3_esp32_devboard

The custom PCB is the ESP32-WROOM-32UE-N16 target. The devboard profile is the
4 MB ESP32 development-board target. Their pin mappings are intentionally
different and compile-time checked.

## SPIFFS images

PlatformIO packs the repository data directory:

    platformio run -e rf3_custom_pcb -t buildfs
    platformio run -e rf3_esp32_devboard -t buildfs

Stage an arbitrary binary file before building:

    python tools\stage_demo_file.py C:\path\to\payload.bin

The stage helper validates the resulting directory against the selected
partition capacity. Files ending in .part are never staged.

## Local upload and monitor ports

Ports are deliberately not stored in platformio.ini:

    platformio run -e rf3_custom_pcb -t upload --upload-port COMx
    platformio run -e rf3_custom_pcb -t uploadfs --upload-port COMx
    platformio device monitor --baud 115200 --port COMx
