# Build and test RF3

RF3 uses PlatformIO Core 6.1.19 and pins the Espressif32 platform to 7.0.1.
Run commands from the repository root. Build output and generated sdkconfig
files remain under the ignored .pio directory.

Create an isolated host environment from the tracked Core requirement:

    python -m venv .venv
    .venv\Scripts\python -m pip install -r requirements.txt
    .venv\Scripts\Activate.ps1

The firmware environment additionally locks ESP-IDF, the Xtensa compiler, and
all build/image tools in `platformio.ini`; a separate VS Code ESP-IDF install is
not read by these PlatformIO builds.

## Native validation

    platformio test -e native -v
    python -m unittest discover -s test -p "test_*.py"
    git diff --check

The native environment requires a host C and C++ compiler (gcc/g++, Clang, or
an appropriately configured MSVC toolchain).

## Software-only firmware qualification

    python tools/run_firmware_qualification.py

This separate C++17 executable requires GCC or Clang (`--cxx clang++` selects
Clang; `CXX` may also name the compiler executable). It exercises the production
Protocol v2 file-transfer service and sessions with deterministic fake transport
faults and host files. It prints measured PASS/FAIL results and returns nonzero
on failure. JSON records, exact fault rules, checksums, logs, and final host files
are retained in a unique ignored `.pio/qualification/run-*` directory.

See [firmware_qualification.md](firmware_qualification.md) for the campaign,
reproducibility settings, and the explicit limits of this software evidence.

## Firmware

    platformio run -e rf3_custom_pcb -t clean
    platformio run -e rf3_custom_pcb
    platformio run -e rf3_esp32_devboard -t clean
    platformio run -e rf3_esp32_devboard

The custom PCB is the ESP32-WROOM-32UE-N16 target. The devboard profile is the
4 MB ESP32 development-board target. Their pin mappings are intentionally
different and compile-time checked.

## SPIFFS images

For physical bring-up, `python tools/prepare_board_handoff.py` builds and checks
both profiles and creates separate sender/receiver images without modifying
`data/` or accessing a board. See [board_bringup.md](board_bringup.md). The lean
receiver image avoids filling a devboard's SPIFFS with the sender's fixtures.

PlatformIO packs the repository data directory:

    platformio run -e rf3_custom_pcb -t buildfs
    platformio run -e rf3_esp32_devboard -t buildfs

Stage an arbitrary binary file before building:

    python tools\stage_demo_file.py C:\path\to\payload.bin

The stage helper validates the resulting directory against the selected
partition capacity and Protocol v2's 1,310,720-byte per-transfer limit. It
bounds names to the generated SPIFFS object-name limit. Files ending in .part
are never staged.

## Readiness and bench validation

The current software assessment is in `docs/pre_hardware_readiness.md`. The
ordered physical procedure, including objective pass/fail criteria and the
shortest critical path, is in `docs/hardware_validation.md`.

## Local upload and monitor ports

Ports are deliberately not stored in platformio.ini:

    platformio run -e rf3_custom_pcb -t upload --upload-port COMx
    platformio run -e rf3_custom_pcb -t uploadfs --upload-port COMx
    platformio device monitor --baud 115200 --port COMx
