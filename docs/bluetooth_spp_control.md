# RF3 Bluetooth SPP Control

The ESP32 devboard build exposes a Bluetooth Classic Serial Port Profile
(SPP) host channel. It carries commands, live text, and bounded file upload or
download chunks between the PC and its local ESP32. The nRF24 radio still
handles the over-the-air path between RF3 devices.

The current implementation lives in this active project: `src/main.cpp`,
`include/bluetooth_tx_flow.hpp`, `include/host_transfer_guard.hpp`,
`include/host_upload_publish.hpp`, and
`host_gui/rf3_gui/device/serial_device.py`. Its GUI is packaged at
`dist/RF3DesktopFixed/RF3DesktopFixed.exe` using `RF3Desktop.spec`. The archived
downloaded project's older BLE scripts are not runtime or build dependencies.

## Firmware build

Bluetooth is enabled for `rf3_esp32_devboard` through:

- `-DRF3_BLUETOOTH_CONTROL_ENABLED=1` in `platformio.ini`
- `-DRF3_BLUETOOTH_REQUIRE_PAIRING=1` in `platformio.ini`
- Classic Bluetooth/SPP sdkconfig defaults in `sdkconfig.devboard.defaults`

The devboard build requires authenticated pairing. Remove an older Windows
pairing and pair again after flashing if its cached SPP service will not open.
Legacy pairing still uses the development PIN `1234`; SSP confirmation is
automatically accepted on the ESP. This is link authentication, not application
authorization or protection against an unwanted nearby device pairing.

The custom PCB build does not enable Bluetooth by default.

## Windows workflow

The current EXE supports the complete user-initiated Classic SPP setup flow:

1. Power the ESP with SPP-enabled RF3 firmware.
2. Click **Scan Bluetooth** in the GUI (approximately 10 seconds).
3. Select its name and MAC address. Cached records are explicitly marked;
   only recent LastSeen timestamps are labeled as seen in this inquiry.
4. Click **Pair & Connect**, then complete the Windows confirmation or PIN
   prompt. The current firmware uses PIN `1234` for legacy pairing.
5. The app enables the selected address's SPP service and waits up to 30
   seconds for exactly one outgoing COM port. It never substitutes another
   ESP's port. The normal 25-second RF3 STATUS handshake starts after setup.
6. Use the existing file upload/download controls once STATUS succeeds.

**Refresh Ports** lists existing serial ports without issuing an inquiry.
Scanning does not pair devices or enable services. Setup has a 120-second
local deadline; Cancel discards late results. Windows' native pairing call
cannot be forcibly cancelled by this implementation, so dismiss its dialog if
still open; pairing that already completed is retained. Scan/connect controls
are gated during active connections and transfers.

The compatibility boundary is Classic GAP/SSP plus the Bluetooth SIG Serial
Port Profile, with the existing RF3 host file protocol above that stream.
BLE/GATT-only ESP chips, and SPP firmware lacking RF3 STATUS/upload/download,
need additional firmware/transport support. No conversion to BLE is implied.

API basis:

- [Classic inquiry and enumeration](https://learn.microsoft.com/en-us/windows/win32/api/bluetoothapis/nf-bluetoothapis-bluetoothfindfirstdevice).
- [Native Windows pairing/consent](https://learn.microsoft.com/en-us/windows/win32/api/bluetoothapis/nf-bluetoothapis-bluetoothauthenticatedeviceex).
- [SPP service/driver activation](https://learn.microsoft.com/en-us/windows/win32/api/bluetoothapis/nf-bluetoothapis-bluetoothsetservicestate).

Manual pairing remains an alternative when using an existing Bluetooth COM port:

1. Flash the devboard over USB, such as `COM3`.
2. Keep USB connected for power if desired.
3. Pair in Windows Bluetooth settings with `RF3-rf3-devboard`.
4. If Windows asks for a legacy PIN, use `1234`.
5. Find the Bluetooth serial COM port assigned by Windows.
6. Start the GUI with the Bluetooth COM port:

```powershell
.\dist\RF3DesktopFixed\RF3DesktopFixed.exe --backend serial --port COMx
```

Use the Bluetooth COM port for GUI control. `COM3` remains available for power,
flashing, and USB console diagnostics.

Windows often creates two Bluetooth serial ports. The incoming/local listener
is not usable for connecting to the ESP; use the outgoing SPP port for
`RF3-rf3-devboard`. If the outgoing port fails with a semaphore timeout, remove
the paired device, power-cycle the ESP, pair it again, and retry the newly
created outgoing COM port.

## Protocol shape

The Bluetooth SPP server receives newline-terminated ASCII console commands.
Commands are routed through the same firmware `dispatchCommand(...)` path used
by the USB console.

The devboard advertises `host_echo=enabled` in USB and JSON STATUS. While test
mode is on, the host can send `ECHO <message>` (1-154 printable ASCII characters)
and receive `ECHO TEXT: <message>` on that same USB/SPP channel. This command
does not use flash or the nRF24; it is rejected outside devboard test mode or
from HTTP/remote RF origins. The distinct command prevents queued test text
from becoming an RF transmission if mode changes before execution. Responses
use the existing RF3 newline-delimited host protocol and Bluetooth SIG SPP.

The GUI labels successful content verification YOU (TEST) / ESP ECHO and never
labels the local ESP as PEER. It gates the action on `host_echo`, allows one
pending echo, and requires exact returned content within 10 seconds; generic
OK/STATUS acceptance is insufficient. Timeout, disconnect, or wrong content
rejects the attempt, and timeout/wrong content closes the link to isolate late
replies. Normal `TEXT <message>` still uses nRF24 to a listening peer.

For ordinary Bluetooth commands, firmware sends:

```text
OK <command>
{"node_name":"...","state":"...","channel":76,...}
```

or:

```text
ERR <command>
{"node_name":"...","state":"...","channel":76,...}
```

The GUI serial backend accepts both the old human-readable `STATUS` line and
the Bluetooth JSON status line.

ECHO and host file commands use their structured responses instead of the
general OK plus JSON pair. Host file
commands use one structured response per request instead of the
general `OK` plus JSON response. Each line remains below the console limit:

```text
UPLOAD BEGIN <name> <size> <crc32>
UPLOAD DATA <sequence> <hex-payload>
UPLOAD END

DOWNLOAD BEGIN <name>
DOWNLOAD DATA <sequence>
DOWNLOAD END
```

The GUI uses 64-byte chunks and waits for each response before sending the
next request. Uploads are written to `host_upload.part` and published only
after size and CRC32 verification. Downloads are written to a `.rf3.part` PC
file and atomically moved to the user-selected destination after verification.

The RF protocol currently publishes received files under generated names such
as `rx_1234ABCD.bin`; it does not preserve the sender's original filename.

## Connection and transfer recovery

- The GUI buffers incomplete serial lines across timeouts and rejects empty,
  unknown, or invalid status documents. Worker events carry a connection
  generation so cancelled opens and late responses cannot affect a reconnect.
  An unanswered connected STATUS poll closes the connection after 15 seconds;
  polling is paused during file transfers, which use their own response timer.
- ESP Bluetooth output uses one bounded message queue and one writer. The
  writer waits for `ESP_SPP_WRITE_EVT` and pauses for congestion to clear.
  Queue overflow or an eight-second write timeout closes the link rather than
  continuing a truncated response. Overlong input commands are discarded in
  full through the next line terminator.
- Firmware advertises `host_retry=enabled`. Only when this capability is present
  does the GUI retry a missing DATA response, up to three times, at 30-second
  intervals. Repeated chunks replay the previous response without writing or
  reading those bytes again. BEGIN/END are not automatically retried; a dropped
  END response can mean the ESP completed the upload but the GUI did not receive
  confirmation. Connection loss requires restarting the transfer, not resuming.
- Upload/download sessions belong to one transport and Bluetooth connection.
  A maintenance task releases disconnected sessions and sessions idle for 60
  seconds. Commands queued by an older Bluetooth connection are ignored.
- Before upload, firmware checks available SPIFFS space with a metadata reserve.
  Flash fragmentation/garbage collection can still cause a later write error.
  Replacing an existing file needs room for both old and new contents.
- Verified replacement uses `host_replace.part` (a checksummed destination
  journal) and `host_backup.part`. Failed publication restores the old file;
  failed rollback retains the backup for recovery at boot or the next upload.
  Startup cleanup never deletes those recovery files. This cannot guarantee
  recovery from physical flash corruption during sudden power loss.
- Names that require sanitizing or truncating get a digest of the original
  filename to prevent common filename aliases. Received RF files still use
  their generated names.

## Validation

Run the GUI tests from the repository root:

```powershell
$env:PYTHONPATH = 'host_gui'
$env:QT_QPA_PLATFORM = 'offscreen'
.\.venv-host\Scripts\python.exe -m unittest discover -s host_gui\tests -v
```

`tools/tests/host_transport_safety.cpp` is a standalone C++11 regression executable
covering flow control, session ownership/expiry, and fault-injected publication
and recovery. Compile it with a desktop C++ compiler and `-Iinclude`.

`host_gui/tests/test_windows_bluetooth.py` injects fake native APIs, port lists,
and registry mappings. It covers pairing consent/cancellation outcomes, fresh
versus cached results, strict address matching, delayed/ambiguous COM ports,
unsupported SPP services, reconnect generations, and GUI state gating. These
tests never invoke a real adapter, pairing dialog, or COM port. Live pairing
and transfers must be verified by the user; the agent must not navigate the
desktop or exercise live device operations without explicit authorization.
The GUI integration regression also clicks Scan Bluetooth, Pair & Connect,
and Disconnect through the actual controller/backend worker flow with injected
native and serial APIs. It verifies that opening the selected outgoing port
and receiving an OK/banner or invalid status cannot establish a connection;
valid fragmented RF3 STATUS can, including when the nRF24 reports Fault.
It also enables test mode through the GUI and uploads/reads back a multi-chunk
binary payload through the actual worker threads, using a simulated ESP store.
It sends maximum-length test text through those workers and verifies the ESP
echo transcript. Native tests cover echo mode/origin/length/ASCII restrictions;
GUI tests also cover missing/mismatched echoes, generic ACKs, old firmware,
overlapping operations, prompt-like message content, and late-response rejection.

On hardware, test USB first, then the paired outgoing SPP port. Repeat
connect/disconnect, transfer binary files spanning multiple chunks, receive live
text while downloading, and interrupt a transfer by disconnecting or resetting
the ESP. Compare the original PC file and saved PC file by SHA256.

For a host-only test without nRF24 hardware, enable devboard test mode and
choose **Upload & Verify**. The GUI uses the same UPLOAD/DOWNLOAD commands
above, with IEEE 802.3 CRC32 and a final byte-for-byte readback comparison.
The result includes a SHA-256 fingerprint. Both directions use the selected
connection; choose the outgoing Bluetooth SPP connection to validate Bluetooth.
This transfers real file contents and writes ESP SPIFFS while firmware blocks
nRF24 commands. It does not validate a file format or RF peer delivery. The
uploaded file remains in SPIFFS under its sanitized name and may replace that
name. Available storage still limits file size. The older `TESTTX <bytes>`
console command remains a size-only diagnostic and is not used by this GUI check.
