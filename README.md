# RF3 ESP32 nRF24 Audio Demo

This repository contains an ESP-IDF firmware project for an ESP32 that stores
raw audio in SPIFFS and sends or receives it over an nRF24L01-style radio.

## What It Does

- Mounts a SPIFFS partition containing `.u8` audio tracks
- Exposes a serial console for selecting tracks and controlling the radio
- Packs audio into fixed-size nRF24 payloads
- Supports RX mode, TX mode, standby, sleep, power-down, and CW test mode
- Includes a host-side helper that converts MP3 files into the expected PCM
  format before uploading them to SPIFFS

## Project Layout

- `src/`: firmware sources and ESP-IDF component registration
- `include/`: shared headers for the firmware and native tests
- `tools/`: host-side helpers for audio preparation and PlatformIO integration
- `data/`: files packed into the SPIFFS partition
- `test/`: native-side tests for packet and radio helper logic

## Hardware Defaults

The default nRF24 wiring in the firmware is:

- `SCK = GPIO18`
- `MISO = GPIO19`
- `MOSI = GPIO23`
- `CE = GPIO27`
- `CSN = GPIO5`
- plus `3.3V` and `GND`

## Build and Flash

### PlatformIO

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

### ESP-IDF

If you are using the ESP-IDF extension or terminal:

```powershell
idf.py build
idf.py -p COM3 flash
idf.py -p COM3 monitor
```

## Audio Workflow

Convert an MP3 into the raw audio format the firmware expects:

```powershell
python tools\prepare_demo_audio.py "C:\path\to\song.mp3" --output-name song_name
```

Then upload the SPIFFS image again so the new `.u8` file is available on the
board.

The firmware expects:

- `8000 Hz`
- `8-bit`
- `mono`
- unsigned PCM stored as `.u8`

## Serial Console Commands

Once the board boots successfully and the nRF24 is connected, the serial
monitor exposes commands such as:

- `HELP`
- `STATUS`
- `FILES`
- `SELECT <file>`
- `TX [file]`
- `RX`
- `STANDBY`
- `SLEEP`
- `WAKE`
- `POWERDOWN`
- `CHANNEL <0-125>`
- `CW START [channel] [power0-3]`
- `CW STOP`

## Notes

- If SPIFFS is missing, the app will stop at startup and ask you to upload a
  filesystem image.
- If the nRF24 is not connected, the app will boot, mount SPIFFS, and then
  fail the radio probe as expected.
- The project includes native tests for parts of the radio and packet logic
  that do not require real ESP32 hardware.
