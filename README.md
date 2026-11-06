# RF3 ESP32 / nRF24 file transfer

Current source build: **RF3-BAT-R1-20261105**, release **v2026.11.05-battery-r1**.
This update combines devboard battery monitoring with Reliable timeout resume,
optional No ACK transfers and RX diagnostics. Matching firmware is provided for
the existing custom PCB and ESP32 devboard, with separate pin profiles.

- [Download GUI and firmware releases](https://github.com/Oddrubberband/RF3esp32-Code-/releases)
- [Changes and verification](CHANGELOG.md)
- [Build and test](docs/build_and_test.md)
- [Battery calibration and protection](docs/calibration/README.md)
- [RF protocol](docs/protocol_v2.md) and [No ACK mode](docs/no_ack_mode.md)

The release workflow publishes the version above only after both the Linux
firmware checks and Windows GUI checks pass. Until that workflow succeeds,
older release downloads remain older implementations. Source ZIPs contain code;
the separate release assets contain the complete GUI and matching firmware.

## Windows use

Download the GUI ZIP and firmware ZIP from the same release. Extract each fully.
Keep RF3Desktop.exe with its `_internal` directory and use the GUI ZIP's
Launch_GUI.cmd. Disconnect and close an older GUI before opening the updated
one. Use one GUI connection per board; open two windows to control two boards.

Connect over USB or Classic Bluetooth SPP. Bluetooth names are RF3-rf3-pcb and
RF3-rf3-devboard, service RF3-SPP, legacy pairing PIN1234. Select the intended
board's outgoing SPP port. USB power and Bluetooth control can coexist. Test
mode without an nRF is devboard-only and still supports its battery telemetry.

For an RF file transfer, match channel and mode on both boards and start Receive
on the destination first. Reliable mode is the default. The receiver verifies
size and CRC32 before publishing a completed file. Its timeout-resume feature
checks the saved file prefix before continuing a repeated send of the same file.
No ACK has no retransmission or peer delivery confirmation; incomplete files are
rejected. The SPIFFS tab imports with byte readback and exports with CRC/size
verification before atomic publication. A source rebuild does not update an
already packaged executable or firmware ZIP.

## Battery monitoring on the devboard

GPIO34 senses the battery input, GPIO15 requests battery enable, and GPIO21
requests the nRF rail. GPIO22/25 remain reserved. The supplied 41 bench points
map sense voltage to pack voltage. The blank24.25V row is omitted. Conversion
interpolates between neighbors and extrapolates with the nearest end segment.

After a10s startup exclusion and2s of RF quiet,250ms batches form a4s mean.
Five consecutive complete means below **20.4V sensed** latch cutoff. Extended
file TX/listening RX gets scheduled measurement pauses about every5min, capped
at7.5s each; low confirmation checks allow2s of RF progress between attempts.
CW is excluded from quiet measurements. No ACK can lose data during RX pauses.

The GUI displays the sensed voltage minus0.10V and uses the firmware's generic
6S/3300mAh charge estimate. Protection uses the unadjusted sensed voltage.
An asynchronous warning announces shutdown before enables are removed. Its
nonmodal GUI notice remains visible after a disconnect; STATUS confirms actual
output levels. Message delivery is not guaranteed when power disappears.

Cutoff persists across reboot. Explicit reset requires a fresh quiet reading
of at least **21.0V sensed**. Reset enables GPIO15 and leaves the nRF rail OFF
until requested. The custom-PCB and future FEM controller power contracts are
not replaced by the devboard controls.

## Board profiles and manual flashing

| Profile | CE / CSN / IRQ | SCK / MOSI / MISO | Flash |
| --- | --- | --- | --- |
| Existing custom PCB |17 /27 /16|18 /23 /19|16MB|
| ESP32 devboard |27 /5 /26|18 /23 /19|4MB|

Use the matching firmware release ZIP and its Flash_Manual.ps1. In PowerShell:

```powershell
.\Flash_Manual.ps1 -Board custom -Port COMn
# Or select -Board devboard. Replace COMn with the board's USB programming port.
```

Close the GUI connection/serial monitor first. BOOT and EN remain manual,
with no_reset before and after upload. Follow the helper's prompts; release
BOOT and press EN afterward. The helper verifies the package's image hashes.
A normal update does not replace SPIFFS or perform a full erase. The source
checkout deliberately does not pin CI image hashes: use the release manifest
or package an exact verified build as documented in the build guide.

## Verification and limits

Local verification passed204 native tests,263 GUI tests,26 tooling tests
(with one platform symlink skip),114 resume cases and12 NoACK file boundaries,
plus production host/RX/cutoff/file-type/diagnostics checks. Both ESP profiles
compiled; partitions, image integrity and application identity were checked.
The compiled GUI matched18 source modules and passed hardware-free mock startup.
An independent review checked calibration at89,314inputs and cutoff/recovery
behavior. See [current verification](docs/verification/current_verification.md).
GitHub CI independently rebuilds this source for the published release.

These are software results. The sensing data was measured with the analog output
unconnected. Loaded GPIO34/ADC accuracy, physical rail shutdown, warning delivery
under power loss and RF performance remain bench checks. The charge estimate is
not a measured Soikon discharge curve. No hardware operation is performed by CI.
