# RF3 ESP32 nRF24 File Transfer Demo

This repository contains an ESP-IDF firmware project for an ESP32 that stages
files in SPIFFS and moves them over an nRF24L01-style radio as packetized data.

## What It Does

- Mounts a SPIFFS partition containing staged files
- Exposes a serial console for selecting files and controlling the radio
- Packs file bytes into fixed-size nRF24 payloads
- Saves completed RX streams back into SPIFFS as `rx_####.bin`
- Supports RX mode, TX mode, standby, sleep, power-down, and CW test mode
- Optionally exposes a small Wi-Fi HTTP control plane on the devboard build

## Project Layout

- `src/`: firmware sources and ESP-IDF component registration
- `include/`: shared headers for the firmware and native tests
- `tools/`: host-side helpers for staging files and PlatformIO integration
- `data/`: files packed into the SPIFFS partition
- `test/`: native-side tests for packet and radio helper logic

## Hardware Defaults

The default devboard nRF24 wiring in the firmware is:

- `SCK = GPIO18`
- `MISO = GPIO19`
- `MOSI = GPIO23`
- `CE = GPIO27`
- `CSN = GPIO5`
- `IRQ = GPIO26` (set `irq_pin` to `kNoIrqPin` if you do not wire it)
- plus `3.3V` and `GND`

The custom PCB environment overrides CE/CSN/IRQ in `platformio.ini`.

## Build And Flash

Firmware build:

```powershell
C:\Users\dman2\.platformio\penv\Scripts\platformio.exe run -e esp32wroom32d
```

Firmware flash:

```powershell
C:\Users\dman2\.platformio\penv\Scripts\platformio.exe run -e esp32wroom32d -t upload
```

SPIFFS upload:

```powershell
C:\Users\dman2\.platformio\penv\Scripts\platformio.exe run -e esp32wroom32d -t uploadfs
```

Serial monitor:

```powershell
C:\Users\dman2\.platformio\penv\Scripts\platformio.exe device monitor -b 115200 -p COM5
```

## File Staging Workflow

Stage any host file into `data/`:

```powershell
python tools\stage_demo_file.py "C:\path\to\payload.bin"
```

Optional destination name:

```powershell
python tools\stage_demo_file.py "C:\path\to\report.pdf" --output-name report.pdf
```

Then upload the SPIFFS image again so the staged file is available on the
board.

The helper:

- accepts arbitrary input files
- copies them into `data/`
- verifies the resulting SPIFFS image still fits
- keeps the over-the-air payload as raw bytes rather than audio semantics

## Serial Console Commands

- `HELP`
- `STATUS`
- `STOP`
- `FILES`
- `SELECT <file>`
- `TX [file]`
- `TX LOOP [count|INF] [file]`
- `MORSE <text>`
- `RX`
- `STANDBY`
- `SLEEP`
- `WAKE`
- `POWERDOWN`
- `CHANNEL <0-125>`
- `CW START [channel] [power0-3]`
- `CW LOOP <on_ms> <off_ms> [channel] [power0-3] [EVERY <loops>]`

## RX Behavior

When the board is in `RX` mode:

- accepted packet streams are written to a temporary SPIFFS file
- a completed stream is renamed to `rx_####.bin`
- partial or corrupt streams are discarded
- `STATUS` and the HTTP `/status` endpoint report stream counters and saved-byte totals

## Wi-Fi Control

The devboard build enables Wi-Fi HTTP control by default. Once connected to the
configured network, it serves:

- `GET /status`
- `POST /rx/start`
- `POST /tx/start`
- `POST /stop`
- `POST /channel?value=<0-125>`

The PCB build disables Wi-Fi control unless you turn it back on with build
flags.

## Notes

- If SPIFFS is missing, the app asks you to upload a filesystem image.
- If the nRF24 is not connected, the app still boots so SPIFFS and serial/Wi-Fi
  controls remain testable.
- Some source files still use names like `AudioPacket` for historical reasons,
  but the transfer path now treats payloads as generic bytes.
