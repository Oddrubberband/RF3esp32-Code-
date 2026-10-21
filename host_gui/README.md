# RF3 Desktop GUI

This is the starting Windows desktop control application for the RF3 ESP32
nRF24 radio firmware.

The application defaults to the serial backend so it can discover USB serial
ports and Windows Bluetooth SPP COM ports. A mock device backend is still
available for GUI development when capstone hardware is not attached.

## Run from source

Run commands directly from the single active project folder:

```powershell
Set-Location 'C:\Users\Pare\Desktop\SCHOOL\fall_2026\404\CodE_project\RF3esp32-Code-clean'
python -m venv .venv-host
.\.venv-host\Scripts\python.exe -m pip install -r host_gui\requirements.txt
.\.venv-host\Scripts\python.exe host_gui\run_gui.py
```

Run the mock backend:

```powershell
.\.venv-host\Scripts\python.exe host_gui\run_gui.py --backend mock
```

Preselect a serial port:

```powershell
.\.venv-host\Scripts\python.exe host_gui\run_gui.py --backend serial --port COM5
```

## Run through Bluetooth SPP

In the maintained EXE, click **Scan Bluetooth**, select the intended ESP by
name and Bluetooth address, and click **Pair & Connect**. For an already paired
device the button reads **Connect**. Windows may display its confirmation/PIN
dialog; the app does not navigate Windows Settings or silently accept pairing.
For the current firmware, a legacy PIN prompt uses `1234`.

The app requests a fresh Classic inquiry and labels cached results explicitly.
It supports any device name exposing Classic SPP; actual RF3 command/file
compatibility is confirmed by STATUS. BLE-only ESP chips require a separate
transport and firmware protocol. **Refresh Ports** only refreshes existing COM
ports; **Scan Bluetooth** performs the radio inquiry. Both are explicit user
actions, never automatic startup operations.

Pairing/service setup and COM-port readiness run in a worker thread. Cancel
rejects stale results and stops local setup; dismiss an outstanding Windows
pairing dialog separately. Pairing already completed in Windows is retained.
The app will not guess another device's COM port if the selected address cannot
be resolved. A missing/ambiguous SPP port produces an actionable error.

The `rf3_esp32_devboard` firmware build now starts a Bluetooth Classic SPP
server named `RF3-rf3-devboard` with service `RF3-SPP`. Windows exposes paired
SPP devices as COM ports, so the GUI still uses the serial backend.

To use an existing paired COM port directly instead of in-app pairing:

1. Flash/power the ESP32 devboard over USB, for example on `COM3`.
2. Use the outgoing SPP port for `RF3-rf3-devboard`.
3. Start the GUI against that Bluetooth COM port:

```powershell
.\dist\RF3DesktopFixed\RF3DesktopFixed.exe --backend serial --port COMx
```

`COM3` can remain the USB power/flashing port. Use the separate Bluetooth COM
port for command/control when testing battery-style operation.

## Update the current package

Rebuild the existing distribution in place; do not create additional
version-named GUI folders. Use `python.exe -m` so the command does not depend
on launcher paths embedded before the project folder was renamed. The single
maintained specification is `RF3Desktop.spec`; it packages to
`dist/RF3DesktopFixed/`. Documentation-only edits do not require packaging.

```powershell
.\.venv-host\Scripts\python.exe -m PyInstaller --noconfirm --distpath dist --workpath build RF3Desktop.spec
```

## Current scope

- Mock discovery and connection.
- Requested state versus confirmed device state.
- Existing firmware controls: `STATUS`, `STOP`, `STANDBY`, `RX`, `SLEEP`,
  `POWERDOWN`, `CHANNEL`, `POWER`, and `TX`.
- File selection with Protocol v2 max-size validation.
- Received-file status: completed RF receives are shown as ESP SPIFFS files
  such as `/spiffs/rx_1234ABCD.bin` when updated firmware reports the path.
  **Save to PC** downloads the file over USB serial or Bluetooth SPP, verifies
  its size and CRC32, and publishes it at the path selected by the user.
- Selected PC files are uploaded to ESP SPIFFS in acknowledged 64-byte chunks,
  verified, and then passed to the existing Protocol v2 nRF24 transmitter.
- Live, bidirectional packet text with a conversation transcript. `TEXT
  <message>` supports up to 154 printable ASCII characters, fragments longer
  messages across nRF24 frames, and returns the sender to receive mode after a
  successful send. Decoded peer messages appear immediately in the GUI.
- Test-mode text echo over USB/Bluetooth with the connected ESP, without
  nRF24 hardware. Exact returned text is labeled ESP ECHO, separately from PEER.
- Separate `Radio & Files` and `Live Text & CW` tabs keep both workflows usable
  at the default 1080 x 720 window size.
- Additional over-the-air controls for `MORSE <text>`, `CW START`, `CW LOOP`,
  and `REMOTE <command...>` firmware commands.
- Transfer progress simulation.
- Structured GUI log area for integration testing.
- USB serial discovery and command/control through the existing firmware
  console parser.
- Bluetooth SPP command/control through the same GUI serial backend once the
  ESP32 is paired and Windows exposes its Bluetooth COM port.

The production serial backend uses the same bounded upload/download protocol
over USB and Windows Bluetooth SPP COM ports. Interrupted downloads remain as
temporary `.rf3.part` files only until the backend cleans them up.

## Test/devboard mode

There is one maintained GUI; the checkbox changes its file and text actions.
Flash the current `rf3_esp32_devboard` firmware for the new text echo capability
(`host_echo=enabled`). Existing file verification needs no protocol change.

With current devboard firmware, connect to the real ESP32 by USB serial or
Bluetooth SPP, then check **Test/devboard mode (no nRF24 TX)**. Select a local
file and press **Upload & Verify**. This sends the actual file contents to ESP
SPIFFS using the existing acknowledged host upload protocol, then reads the
stored file back into bounded PC memory. Success requires size/CRC32 validation,
the final download response, and an exact byte-for-byte comparison. The transfer
detail and log show the byte count, CRC32 and SHA-256 fingerprint. No `TX` is
sent and the firmware blocks nRF24 commands while test mode remains enabled.

The upload stays on ESP storage under its sanitized filename and can replace
that name; the PC source is unchanged and no extra download file is created.
This validates binary transport/storage, not semantic parsing of audio or other
file formats. File size must fit both the Protocol v2 limit and available
SPIFFS space. Disconnect/reconnect requires restarting the check.

The status panel's **Legacy TESTTX counter** belongs to the optional size-only
console command `TESTTX <bytes>`; it is not the file verification result.
Bluetooth SPP still uses Bluetooth radio for host traffic. The mock backend is
only a UI simulator; real transport validation requires a real ESP connection.

In **Live Text & CW**, test mode enables **Send to ESP** when the firmware
advertises `host_echo`. It sends `ECHO <message>` through the selected host
connection and waits up to 10 seconds for an exact `ECHO TEXT:` reply. The
transcript then shows **YOU (TEST)** and **ESP ECHO**. Generic acknowledgments
do not confirm the test. Missing or incorrect returned content closes the
connection; reconnect to retry. There is no flash write or nRF24 transmission.
Morse, remote commands, and CW remain disabled.

With test mode off, **Send to Peer** uses `TEXT <message>`: local laptop →
local ESP → nRF24 → peer ESP → peer laptop. Both ESPs need nRF24 hardware and
matching RF settings; put the peer in Receive first. The sender returns to
receive mode after a successful send. **YOU** means local transmission and
does not prove peer application delivery; **PEER** means received radio text.
Both modes accept up to 154 printable ASCII characters per message.

After GUI source changes, run `host_gui/run_gui.py` or rebuild the packaged
executable in its existing output location using `RF3Desktop.spec`. The
test-mode checkbox is enabled after a successful `STATUS`
connection, including when the nRF24 is absent and the real radio state is
`Fault`.

The only maintained GUI is `dist/RF3DesktopFixed/RF3DesktopFixed.exe`.
Close any running app before starting it, then connect to the USB or outgoing
Bluetooth COM port. The Test/devboard mode checkbox is in the Connection area.
Obsolete generated GUI distributions have been removed to avoid launching
stale versions.

The updated backend preserves partial response lines, isolates reconnects, and
retries missing file DATA responses when firmware advertises `host_retry`.
Interrupted connections require restarting the file transfer. See
`docs/bluetooth_spp_control.md` for the recovery behavior and hardware checks.
