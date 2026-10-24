# RF3 ESP32 / nRF24 file transfer

The current implementation is **R2**, promoted after a user-operated PDF round
trip produced an exact 411,014-byte match to its original. The Windows GUI and
firmware support the existing custom PCB and the ESP32 devboard with separate
pin profiles. Both use Classic Bluetooth SPP or USB for PC control.

- [Download the Windows GUI and matching firmware](https://github.com/Oddrubberband/RF3esp32-Code-/releases/latest)
- [Every implementation change, what it fixed, and how it was verified](CHANGELOG.md)
- [Build and software checks](docs/build_and_test.md)
- [Verification evidence and limits](docs/verification/current_verification.md)
- [RF protocol and file-extension compatibility](docs/protocol_v2.md)

The release workflow attaches GUI/firmware ZIPs only after validation succeeds.
If a release build is still running, see the repository Actions tab or build
from this checkout using the commands below. Keep the complete GUI distribution,
including `_internal`; copying only the EXE will not work.

## Current behavior

- Reliable file transfer with size checks, CRC32 and peer-confirmed completion.
- SPIFFS browser: import with complete byte-for-byte readback, select/transmit
  an existing stored file, CRC-checked atomic export, and confirmed deletion.
- Live receiver state, byte count, percent, observed payload KiB/s and elapsed time.
- Safe repeated Receive, active-transfer guards and explicit Cancel Receive.
- Channel/power drafts survive refresh; confirmed settings remain separate.
- Bounded handshake/status/command deadlines, proper error reporting and separate
  rotating logs for simultaneous GUI instances.
- Buffered PC reads and negotiated 64/256-byte host chunks, preserving CRC/retries.
- File-type suffix preservation, e.g. PDF to `rx_<id>.pdf`, without changing bytes.

## Boards and Bluetooth

| Signal | Existing custom PCB | ESP32 devboard |
|---|---:|---:|
| CE | 17 | 27 |
| CSN | 27 | 5 |
| IRQ | 16 | 26 |
| SCK | 18 | 18 |
| MOSI | 23 | 23 |
| MISO | 19 | 19 |
| Flash | 16 MB | 4 MB |

Bluetooth names: **RF3-rf3-pcb** and **RF3-rf3-devboard**; Classic SPP service
`RF3-SPP`, legacy pairing PIN `1234`. Test/devboard mode is devboard-only and
uses the host connection without RF transmission. USB power and Bluetooth
control can coexist, but each board may have only one GUI client at a time.
Radio power and wiring must be present independently of the PC control link.

## Windows quick start

From a source checkout, the current GUI launcher is:

```powershell
.\Launch_GUI.cmd
```

For a GUI-only release ZIP, extract everything and use its included Launch_GUI.cmd.
Open it twice to control separate boards. Disconnect the old GUI from a board
before connecting another instance to that same board.

Use the firmware release ZIP or locally built images with the hash-checking
manual helper. Enter each board's **USB programming** COM port, not its Bluetooth
COM port:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Flash_Manual.ps1 -Board custom
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Flash_Manual.ps1 -Board devboard
```

Hold **BOOT**, press Enter at the prompt, tap **EN** at Connecting, release BOOT
when Writing begins, and press **EN manually** after verification. Both resets
remain `no_reset`; the proven standard stub/compression, 115200 baud, DIO40m and
ESP-IDF offsets 0x1000/0x8000/0x10000 are retained. No full erase, Arduino boot_app0
image, or SPIFFS image upload is part of this application update.

Build from source if needed:

```powershell
py -3.11 -m venv .venv-host
.\.venv-host\Scripts\python.exe -m pip install -r host_gui\requirements.txt
.\.venv-host\Scripts\python.exe -m PyInstaller --noconfirm RF3Desktop.spec
py -m pip install -r requirements.txt
py -m platformio run -e rf3_custom_pcb -e rf3_esp32_devboard
```

The Windows manual helper requires the VS Code PlatformIO extension (its default
Python/esptool installation under your user profile). Installing PlatformIO only
in a source virtual environment does not install that extension runtime.

Fresh source builds need generated image hashes before the manual helper will
accept them; use tools/package_current_release.py as described in the build guide.

## Receiving and export

Match channels, press Receive on the peer, then transmit. The receiver's Incoming
RF panel shows progress/rate. Payload rate averages observed byte deltas over
about ten seconds; elapsed time starts at first GUI observation. Neither is the
configured radio bitrate. Use Cancel Receive only to interrupt the file.

**Both boards need matching R2 firmware for file-type metadata.** Updated receivers
accept legacy flag-zero START; older receivers reject extension-bearing START.
RF metadata preserves the final suffix, including case, up to 10 ASCII characters
including the dot, using letters/digits/underscore/hyphen. Unsupported RF suffixes
fail explicitly. `.tar.gz` carries `.gz`; extensionless/legacy transfers use `.bin`.
Host-only imports can retain longer safe suffixes if the SPIFFS name fits. Existing
received files keep their names. Full original filenames and MIME detection are
not part of this feature. File-content CRC remains distinct from filename metadata.

## Verification scope

The tested R2 implementation passed 208 GUI tests, 177 native tests, both firmware
builds, actual-method native fault/compatibility tests, packaged mock startup and
independent review. The physical observation is one user-operated file round trip:
original and exported files are 411,014 bytes, byte-identical, with matching SHA-256.
The PDFs and their document fingerprints remain private.
Direction, board identities and flashed-image identities were not independently
checked in that comparison. Range, sustained throughput, every driver condition,
and long-term reliability remain unqualified. See the verification note for detail.
