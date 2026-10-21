# RF3 ESP32 nRF24 File Transfer

RF3 is an ESP-IDF firmware project for transferring arbitrary binary files
over an nRF24L01+ radio. Audio files, including .u8 samples, remain supported
as ordinary binary payloads.

## Index

- [Operations](#operations)
- [Update policy](#update-policy)
- [Capabilities](#capabilities)
- [Engineering standards and documentation rule](#engineering-standards-and-documentation-rule)
- [Supported hardware profiles](#supported-hardware-profiles)
- [Build](#build)
- [Flash and monitor from PowerShell](#flash-and-monitor-from-powershell)
- [File staging](#file-staging)
- [Communications](#communications)
- [Console](#console)
- [Radio probe failure](#radio-probe-failure)
- [Bluetooth SPP control](#bluetooth-spp-control)
- [Live text conversation](#live-text-conversation)
- [Test/devboard mode](#testdevboard-mode)
- [Wi-Fi control security](#wi-fi-control-security)

## Operations

The only active source workspace is:

`C:\Users\Pare\Desktop\SCHOOL\fall_2026\404\CodE_project\RF3esp32-Code-clean`

Open this folder directly in VS Code and run project commands from it. The
path has no whitespace; no directory junction or alternate project path is
needed. Keep firmware source in `src/` and `include/`, GUI source in
`host_gui/`, generated firmware in `.pio/`, and the current packaged GUI at
`dist/RF3DesktopFixed/RF3DesktopFixed.exe` within this same workspace.
Generated build and distribution folders are outputs, not separate source
workspaces. Update the existing application output instead of creating new
version-named distribution folders.

The retired downloaded project is preserved for historical recovery in
`..\downloaded_code_project_archive.zip`, including its Git history and
legacy BLE scripts. The ZIP is not an active workspace. Keep ongoing edits
and builds in the single source workspace above.

Launch the current GUI over USB:

```powershell
Set-Location 'C:\Users\Pare\Desktop\SCHOOL\fall_2026\404\CodE_project\RF3esp32-Code-clean'
& '.\dist\RF3DesktopFixed\RF3DesktopFixed.exe' --backend serial --port COM3
```

Use the USB COM port actually assigned to the ESP32. The project instructions
are maintained in `..\AI_Instructions.txt`, including the required absolute
file paths. The repository's `AGENTS.md` directs future sessions to that file.

### Update policy

- Edit existing documents and source in place. Documentation-only changes
  must not compile firmware, package an application, flash hardware, or create
  another document/build version.
- Repository hygiene permits the registered absolute paths in the project
  runbooks, agent entry point, and saved VS Code ESP-IDF setup. It still checks
  every tracked text file for credentials and rejects machine-specific paths
  in implementation code.
- The only maintained GUI distribution is `dist/RF3DesktopFixed/`.
  `RF3Desktop.spec` is the single packaging specification; it replaces that
  existing application output when packaging is needed. Obsolete generated
  GUI distributions and the duplicate specification have been retired.
- When GUI source changes need a packaged executable, use the existing
  specification from this project root:

```powershell
.\.venv-host\Scripts\python.exe -m PyInstaller --noconfirm --distpath dist --workpath build RF3Desktop.spec
```

Firmware outputs remain in `.pio/build/<environment>/` for the selected
hardware. The devboard and custom PCB are different hardware profiles, not
duplicate project versions. The current GUI and Classic Bluetooth SPP
implementation are in `host_gui/`, `src/main.cpp`, and `include/`; the historical
ZIP's BLE prototype is not required to build or run them.

### Capabilities

- Protocol v2 START/READY, stop-and-wait DATA/ACK, and END/COMPLETE flow
- Transfer IDs, bounded retries, cancellation, inactivity timeouts, and CRC32
- Streaming sources and verified, collision-safe SPIFFS publication
- Serial commands for file transfer, RX, Morse, CW, and radio diagnostics
- Windows GUI Bluetooth Classic discovery, user-initiated pairing, and outgoing
  SPP connection validated by the RF3 STATUS handshake
- Bluetooth SPP command/control, live text, CRC-verified PC/ESP file upload and
  download on the ESP32 devboard build
- Devboard test mode for real file upload/readback and ESP text echo without nRF24 TX
- Optional development-only Wi-Fi HTTP controls, disabled by default

## Engineering standards and documentation rule

All future firmware, host GUI, connection, Bluetooth SPP, and transmit-protocol
changes must follow the applicable IEEE standards and document the chosen
standard or protocol basis when a change affects interfaces, safety, timing,
encoding, reliability, or verification. Bluetooth Classic SPP work must also
stay aligned with the Bluetooth Serial Port Profile behavior documented for the
project, and RF transmit behavior must stay aligned with Protocol v2 unless a
new protocol version is explicitly specified.

Every code or connection change must update this README when the project
behavior, supported hardware, build process, security posture, Bluetooth SPP
flow, transmit protocol, or user workflow changes. Detailed design notes may
live under docs/, but the top-level README must remain the current entry point
for those changes.

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

Run builds directly from the active workspace above. Its path contains no
whitespace and does not require a junction. Select the environment explicitly:
the default is the custom PCB, while the devboard has a different pin map and
enables Bluetooth SPP.

    platformio run -e rf3_custom_pcb
    platformio run -e rf3_esp32_devboard
    platformio test -e native -v

### Flash and monitor from PowerShell

Run these commands from the active repository directory. Find the ESP32's USB
serial port and replace `COM3` with that port. Close any serial monitor or GUI
using the port before uploading. Flash only the environment matching your board.

```powershell
Set-Location 'C:\Users\Pare\Desktop\SCHOOL\fall_2026\404\CodE_project\RF3esp32-Code-clean'
platformio device list
$usbPort = 'COM3'
```

For the ESP32 development board (4 MB):

```powershell
platformio run -e rf3_esp32_devboard -t upload --upload-port $usbPort
platformio run -e rf3_esp32_devboard -t uploadfs --upload-port $usbPort
```

For the custom PCB (16 MB):

```powershell
platformio run -e rf3_custom_pcb -t upload --upload-port $usbPort
platformio run -e rf3_custom_pcb -t uploadfs --upload-port $usbPort
```

Then open the USB serial terminal:

```powershell
platformio device monitor --baud 115200 --port $usbPort
```

`uploadfs` writes the SPIFFS image from `data/`; run it when those files change.
If `platformio` is not recognized in a VS Code PowerShell terminal, use the
installed PlatformIO Core executable directly. From this repository directory:

```powershell
$pio = 'C:\Users\Pare\.platformio\penv\Scripts\platformio.exe'
& $pio device list
& $pio run -e rf3_esp32_devboard -t upload --upload-port COM3
& $pio run -e rf3_esp32_devboard -t uploadfs --upload-port COM3
& $pio device monitor --baud 115200 --port COM3
```

Replace `COM3` with the devboard USB port reported by `device list`.
For Bluetooth SPP control, select Windows' separate outgoing Bluetooth COM port
in the GUI. The Bluetooth COM port is not the USB flashing port.

## File staging

Stage any host file into the PlatformIO data directory:

    python tools\stage_demo_file.py C:\path\to\payload.bin

The helper sanitizes the destination name, excludes receiver .part files,
validates replacement and capacity behavior, and checks the SPIFFS image fit.

## Communications

The USB UART console and optional Bluetooth Classic SPP channel share the
firmware command parser. The nRF24 data path uses Protocol v2 between RF3
devices. Host control and file ingress/export remain distinct from RF delivery.

### Console

The serial console supports HELP, STATUS, STOP, FILES, SELECT, TX, TX LOOP,
UPLOAD, DOWNLOAD, MORSE, TEXT, RX, STANDBY, SLEEP, WAKE, POWERDOWN, CHANNEL,
CW START, CW LOOP, and devboard test-mode ECHO.
If SPIFFS or the radio is unavailable, the console remains available and
reports the failed subsystem.

### Radio probe failure

If `STATUS` shows `State=Fault fault=1 last_status=0x00`, the nRF24 SPI register
probe failed. The USB serial connection and SPIFFS may still work. With power
off, verify the selected board's pin map above, the radio module's 3.3 V supply
and common ground, and each SCK/MOSI/MISO/CSN connection. Verify the radio
module's own pin labels before applying power; do not connect its VCC to 5 V.
Then power-cycle the board and check `STATUS` again. A persistent `0x00` can
also indicate an unpowered radio or a held-low MISO line. Avoid starting TX
until the radio reports a non-Fault state.

### Bluetooth SPP control

The EXE now handles discovery and initiates pairing from its Connection area:
click **Scan Bluetooth**, select the ESP by name/address, then click
**Pair & Connect** (or **Connect** if paired). Complete any Windows pairing
confirmation/PIN dialog; the development legacy PIN is `1234`. The app enables
the selected device's SPP service, waits for its outgoing COM port, and runs
the existing RF3 STATUS handshake. **Refresh Ports** separately lists existing
USB and Bluetooth serial ports. No pairing or service changes occur on a scan.

Scan results distinguish **seen in this scan** from **cached, presence
unconfirmed**; a remembered name alone is not proof that the ESP is nearby.
**Cancel** stops local setup and rejects late worker results. If a native
Windows pairing dialog is open, dismiss it as well; completed Windows pairing
is not automatically undone. Scanning is disabled during connections/transfers.

This is Windows Bluetooth Classic SPP, not BLE/GATT. ESP devices must expose
SPP and compatible RF3 console/upload/download firmware to use the GUI's file
operations. BLE-only ESP variants need a separate transport and matching
firmware. Discovery is not restricted to a hardcoded RF3 name or address.
Long device labels are available in tooltips; scrollable work tabs keep the
connection controls and file workflows usable at the default window size.

The ESP32 devboard build can expose a Bluetooth Classic Serial Port Profile
host channel for commands, live text, and PC/ESP file ingress or export. SPP
uses the same command path as the USB console; the nRF24L01+ remains the RF
data path between boards. See docs/bluetooth_spp_control.md for pairing,
COM-port selection, and response framing.

The implementation uses Microsoft's Bluetooth GAP/SSP APIs and the Bluetooth
SIG Serial Port Profile: `BluetoothFindFirstDevice`,
`BluetoothAuthenticateDeviceEx`, and `BluetoothSetServiceState`. Pairing and
SPP driver readiness are background work; a successful pairing is not reported
as an RF3 application connection until STATUS is validated. Firmware/RF
Protocol v2 and CRC-verified host file transfers are unchanged.

Connection requirements: a Windows laptop with an enabled Classic Bluetooth
adapter and a Classic-capable ESP32 running the `rf3_esp32_devboard` RF3
firmware. The custom PCB profile leaves Bluetooth disabled by default; select
firmware by the actual board wiring. An ESP merely advertising Bluetooth is
insufficient: it must provide Classic SPP and the RF3 command protocol.

| Bluetooth function | Current behavior |
| --- | --- |
| Discover and pair | Scan from the GUI, select the ESP address, and complete Windows consent/PIN prompts. |
| Establish application connection | Enable SPP, open only the selected ESP's outgoing COM port, and require valid RF3 STATUS. |
| Control and exchange data | Use status, radio controls, live text, and verified host file ingress/export through the same console protocol as USB. RF delivery still requires the nRF24 hardware and a peer. |
| Check without an nRF24 | A radio Fault does not prevent a host connection. In devboard test mode, Upload & Verify transfers real bytes to SPIFFS and reads them back for exact comparison. |
| Recover | Cancel setup, disconnect/reconnect, reject late worker results, and close silent or failed links. Interrupted file transfers restart from the beginning. |

Software verification on 2026-10-20: all **77 hardware-free GUI regression
tests passed**, including GUI-button-driven scan, pairing, selected-port opening,
STATUS validation, real host-protocol upload/readback, and disconnect with injected
Bluetooth/serial APIs. Exact text echo, mode/capability gating, late responses,
corruption, mismatched metadata, missing responses, injected CRC collisions,
duplicates, binary/empty payloads, and disconnect cleanup are covered. All
**8 native host transport safety tests** and both supported firmware builds
passed. All 16 packaged GUI application
modules matched current source, and the maintained EXE passed a four-second
offscreen startup smoke check with its mock backend. No live Bluetooth inquiry,
pairing, serial connection, flashing, or peer RF transfer was performed. These
checks establish software readiness; physical laptop/ESP operation still needs
a user-run or explicitly authorized hardware test.

### Live text conversation

The same GUI has two text workflows, selected by the devboard test-mode checkbox:

| Mode | Message route | What a transcript entry proves |
| --- | --- | --- |
| Normal: Send to Peer | Laptop → USB/Bluetooth → local ESP → nRF24 → peer ESP → USB/Bluetooth → peer laptop | YOU reports local radio transmission; PEER displays decoded incoming radio text. Local TX alone does not confirm peer application delivery. |
| Test mode: Send to ESP | Laptop → USB/Bluetooth → connected ESP → the same laptop | YOU (TEST) and ESP ECHO appear only after the connected ESP returns the exact text. No nRF24 or second ESP is needed. |

For a conversation between two users, connect each laptop GUI to its own ESP,
attach an nRF24 to each ESP, and use matching channel, address, and data-rate
settings. Turn test mode off on both. In **Radio & Files**, put the receiving
peer into **Receive**, then use **Live Text & CW → Send to Peer**. The sender
returns to receive mode after a successful send so the other user can reply.
Text is limited to 154 printable ASCII characters per message. Text controls
use the existing RF text framing; file transfer continues to use Protocol v2.

To check Bluetooth text with one ESP and no nRF24, flash the current devboard
firmware, connect the GUI over Bluetooth, enable test mode, then type in
**Live Text & CW** and click **Send to ESP**. The ESP must advertise
`host_echo=enabled`; earlier firmware needs reflashing before this control is
enabled. The GUI sends `ECHO <message>` and requires a matching `ECHO TEXT:`
reply within 10 seconds. A generic OK is insufficient. A timeout or mismatched
echo closes the connection so late replies cannot validate a later attempt.
Echo does not write SPIFFS, generate RF frames, or count as a peer response.
Morse, CW, remote commands, and RF receive stay blocked in test mode.

## Test/devboard mode

Flash the current `rf3_esp32_devboard` firmware, connect the GUI over USB serial
or the outgoing Bluetooth SPP COM port, then enable **Test/devboard mode (no nRF24 TX)**
in the Connection area. The ESP confirms the mode in `STATUS` as `test_mode=on`.
Launch the GUI from `host_gui/run_gui.py` after source changes or update the
existing package using the canonical specification above. The current
packaged copy is available at
`dist/RF3DesktopFixed/RF3DesktopFixed.exe`; close any running GUI before opening it
so the selected COM port is free.
The checkbox becomes available after a successful `STATUS` connection, even
when the real radio reports `State=Fault` because no nRF24 is attached.
Choose a local file and press **Upload & Verify** (the file action changes its
label while test mode is enabled). The GUI reads the actual binary contents,
uploads acknowledged 64-byte chunks over the selected USB or Bluetooth SPP
connection, and waits for the ESP to verify size and CRC32 and publish the file
in SPIFFS. It then downloads that same stored file into bounded PC memory and
compares every byte against the uploaded contents. Completion requires the
download END response, matching size/CRC32, and an exact byte-for-byte match.
The transfer detail and log show the verified byte count, CRC32, SHA-256, and
**no nRF24 TX**. Both upload and download firmware capabilities are required.

This checks binary transfer and storage, including embedded zeros and line
breaks; it does not parse WAV/audio or other file formats. The selected file
must fit the Protocol v2 size limit and available ESP SPIFFS space. The verified
upload stays on the ESP under its sanitized filename and can replace an existing
file of that name. No extra PC copy is saved, and the source file is unchanged.
Disconnect cancels the check; restart after reconnecting if it fails.

This mode is devboard-only and starts off after each reboot. It blocks normal
RF commands while enabled. Upload & Verify never sends `TX` or starts the
Protocol v2 radio handshake; it verifies only the PC/ESP host transfer and
storage, not peer RF delivery. The legacy console command `TESTTX <bytes>`
still provides a size-only simulated acknowledgment. Its status counter is
separate from the GUI's real file verification and does not increase for an
Upload & Verify operation.
The real radio `State` and `fault` remain visible, and `tx_ok` continues to
describe real RF activity. Disable the mode before testing the actual radio.
The mode remains on if the GUI disconnects; reconnect to see its current state
or reboot the ESP to return to the normal startup state.
Bluetooth SPP still uses the Bluetooth radio for control traffic, and the
devboard build may advertise Bluetooth even when USB serial is used. Test mode
guarantees only that it does not initiate nRF24 transmission.

## Wi-Fi control security

Wi-Fi and HTTP control are disabled in every tracked environment. The existing
HTTP endpoints are unauthenticated and can start transfer or change device
state. To opt in on an isolated development network, copy
include/wifi_control_config.local.example.hpp to the ignored
include/wifi_control_config.local.hpp, supply local credentials, and explicitly
define RF3_WIFI_CONTROL_ENABLED=1 for that local build.

See docs/build_and_test.md, docs/bluetooth_spp_control.md,
docs/file_transfer_api.md, docs/protocol_v2.md, and docs/security.md for
integration and validation details.
