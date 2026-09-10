# RF3 ESP32 nRF24 File Transfer

RF3 is an ESP-IDF firmware project for transferring arbitrary binary files
over an nRF24L01+ radio. Audio files, including .u8 samples, remain supported
as ordinary binary payloads.

## Continuing development on another computer

Start with [the project handoff](docs/project_handoff.md). It records the current
branch, completed work, validation, and next steps. Read the
[detailed phase guide](docs/firmware_roadmap.md) for the development plan, or its
[PDF copy](docs/reports/RF3_Detailed_Phase_Guide.pdf). Repository instructions in
`AGENTS.md` require these records to stay current as work proceeds.

## Capabilities

- Protocol v2 START/READY, stop-and-wait DATA/ACK, and END/COMPLETE flow
- Transfer IDs, bounded retries, cancellation, inactivity timeouts, and CRC32
- Streaming sources and verified, collision-safe SPIFFS publication
- Serial commands for file transfer, RX, Morse, CW, and radio diagnostics
- Optional development-only Wi-Fi HTTP controls, disabled by default

## Supported hardware profiles

The rf3_custom_pcb environment selects the custom ESP32-WROOM-32UE-N16 PCB:

- CE 17, CSN 27, IRQ 16
- SCK 18, MOSI 23, MISO 19

The rf3_esp32_devboard environment selects the ESP32 development-board setup:

- CE 27, CSN 5, IRQ 26
- SCK 18, MOSI 23, MISO 19

Each firmware environment selects and compile-time checks exactly one profile.
Radio channel, data rate, address, power, payload width, and SPI behavior remain
common to both.

## Build

Install the tracked PlatformIO Core version into an isolated environment first:

    python -m venv .venv
    .venv\Scripts\python -m pip install -r requirements.txt
    .venv\Scripts\Activate.ps1

    platformio run -e rf3_custom_pcb
    platformio run -e rf3_esp32_devboard
    platformio test -e native -v

Run the software-only firmware qualification (GCC/Clang C++17 required):

    python tools/run_firmware_qualification.py

This reports measured file integrity, deterministic loss recovery, corruption
rejection, and transfer stress results. It does **not** validate physical RF or
hardware. See [qualification details](docs/firmware_qualification.md).

Upload ports are local choices and are not stored in the repository:

Before physical implementation, follow [the board bring-up guide](docs/board_bringup.md).
Prepare checked firmware plus separate sender/receiver filesystem images without
accessing any hardware:

    python tools/prepare_board_handoff.py

The custom PCB requires manual BOOT/EN entry before upload. The guide documents
the exact pinout, safe-power gates, receiver storage limits, and first-link test.

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

Preview a channel's nominal center frequency before selecting it:

```text
rf24> CHANNEL PREVIEW 76
Channel 76: 2476 MHz (nominal center frequency). Radio unchanged.
Use CHANNEL 76 to select it.
rf24> CHANNEL 76
Radio reinitialized on channel 76 (2476 MHz)
```

`CHANNEL PREVIEW <0-125>` only calculates the frequency: it does not tune,
transmit, stop a loop, or discard a receive in progress. `CHANNEL <0-125>` applies
the change and retains the existing behavior of stopping active work and
reinitializing the radio. Both nodes must select the same channel. `STATUS`
also shows `frequency_mhz` for the configured channel. Invalid values and extra
arguments are rejected without changing the radio.

The conversion is `2400 + channel` MHz, per the
[Nordic nRF24 specification, section 6.3](https://devzone.nordicsemi.com/cfs-file/__key/communityserver-discussions-components-files/4/content.pdf).
This is a nominal frequency preview, not a measurement or channel occupancy
scan. A supported channel number does not establish permission to transmit;
follow the permitted frequencies and power levels for the test site.

## Wi-Fi control security

Wi-Fi and HTTP control are disabled in every tracked environment. The existing
HTTP endpoints are unauthenticated and can start transfer or change device
state. To opt in on an isolated development network, copy
include/wifi_control_config.local.example.hpp to the ignored
include/wifi_control_config.local.hpp, supply local credentials, and explicitly
define RF3_WIFI_CONTROL_ENABLED=1 for that local build.

When HTTP control is enabled, `GET /channel?value=76` previews without accessing
the radio and returns `{"preview":true,"channel":76,"frequency_mhz":2476}`.
`POST /channel?value=76` still applies the selection. `GET /status` now includes
`frequency_mhz` alongside the active `channel`. No web page is served by the
firmware; these are JSON API endpoints. Wi-Fi remains disabled by default.

See docs/build_and_test.md, docs/pre_hardware_readiness.md,
docs/hardware_validation.md, docs/file_transfer_api.md, docs/protocol_v2.md,
and docs/security.md for integration and validation details.
