# RF3 ESP32 nRF24 File Transfer

RF3 is an ESP-IDF firmware project for transferring arbitrary binary files
over an nRF24L01+ radio. Audio files, including .u8 samples, remain supported
as ordinary binary payloads.

## Capabilities

- Protocol v2 START/READY, stop-and-wait DATA/ACK, and END/COMPLETE flow
- Transfer IDs, bounded retries, cancellation, inactivity timeouts, and CRC32
- Streaming sources and verified, collision-safe SPIFFS publication
- Serial commands for file transfer, RX, Morse, CW, and radio diagnostics
- Optional development-only Wi-Fi HTTP controls, disabled by default

## Supported hardware profiles

The rf3_custom_pcb environment selects the custom ESP32-WROOM-32UE-N16 PCB:

- CE 17, CSN 5, IRQ 27
- SCK 18, MOSI 23, MISO 19

The rf3_esp32_devboard environment selects the ESP32 development-board setup:

- CE 27, CSN 5, IRQ 26
- SCK 18, MOSI 23, MISO 19

Each firmware environment selects and compile-time checks exactly one profile.
Radio channel, data rate, address, power, payload width, and SPI behavior remain
common to both.

## Build

    platformio run -e rf3_custom_pcb
    platformio run -e rf3_esp32_devboard
    platformio test -e native -v

Upload ports are local choices and are not stored in the repository:

    platformio run -e rf3_custom_pcb -t upload --upload-port COMx
    platformio run -e rf3_custom_pcb -t uploadfs --upload-port COMx
    platformio device monitor --baud 115200 --port COMx

## File staging

Stage any host file into the PlatformIO data directory:

    python tools\stage_demo_file.py C:\path\to\payload.bin

The helper sanitizes the destination name, excludes receiver .part files,
validates replacement and capacity behavior, and checks the SPIFFS image fit.

## Console

The serial console supports HELP, STATUS, STOP, FILES, SELECT, TX, TX LOOP,
MORSE, RX, STANDBY, SLEEP, WAKE, POWERDOWN, CHANNEL, CW START, and CW LOOP.
If SPIFFS or the radio is unavailable, the console remains available and
reports the failed subsystem.

## Wi-Fi control security

Wi-Fi and HTTP control are disabled in every tracked environment. The existing
HTTP endpoints are unauthenticated and can start transfer or change device
state. To opt in on an isolated development network, copy
include/wifi_control_config.local.example.hpp to the ignored
include/wifi_control_config.local.hpp, supply local credentials, and explicitly
define RF3_WIFI_CONTROL_ENABLED=1 for that local build.

See docs/build_and_test.md, docs/file_transfer_api.md, docs/protocol_v2.md, and
docs/security.md for integration and validation details.
