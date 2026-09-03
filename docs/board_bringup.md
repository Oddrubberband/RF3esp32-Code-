# RF3 physical-board handoff

**Ready for controlled bring-up preparation, not yet hardware-qualified.** The
software qualification passed, but no physical RF link has been demonstrated.
There is no PCB schematic/layout or module-specific PA+LNA datasheet in this
repository, and no identifiable USB ESP32 board was found on this host during
the 2026-08-30 review. Do not infer a board port from the generic COM1 entry.

The first hardware release gates are the actual PCB revision/pinout, exact ESP32
module/flash size, radio module model, regulator/current budget, and antenna
connections. Reconcile these with the owner before applying power. If any differ
from the profiles below, stop and review the configuration; do not guess pins.

## 1. Prepare the checked images without touching hardware

From the repository root, using the environment in [build_and_test.md](build_and_test.md):

```powershell
.\.venv\Scripts\python.exe tools\prepare_board_handoff.py
```

This builds both canonical firmware and SPIFFS targets, then creates a unique
ignored `.pio/board_handoff/run-*` package. To prepare only the custom PCB, append
`--environment rf3_custom_pcb`. An explicit `--core-dir` or `PLATFORMIO_CORE_DIR`
can select the package cache; otherwise an existing `.pio/core` cache is used
before the normal PlatformIO default.

The script **never opens a serial port or executes an upload, erase, or RF
command**. It fails nonzero if any required check fails. It checks:

- Selected PlatformIO profile, pins, Wi-Fi-disabled flag, and 40 us CE setting.
- Generated 16 MB/4 MB flash size, DIO/40 MHz mode, and partition-table offset.
- The decoded binary partition table against the selected source CSV, with
  non-overlap, flash bounds, and application/bootloader fit checks.
- Bootloader and application checksums and appended hashes using pinned
  esptool's offline `image_info` command.
- SPIFFS tool settings against generated firmware settings: 256-byte pages,
  32-byte object names, four metadata bytes, and both magic options enabled.
- Actual pack/unpack of each role image, with all file lengths, CRC32 values,
  and SHA-256 hashes matching its source directory.

Each profile contains identical firmware for either node, separate
`spiffs-sender.bin` / `spiffs-receiver.bin`, logs, inventories, and
`FLASH_INSTRUCTIONS.md`. The top-level `manifest.json` records image hashes,
flash offsets, configuration, and `hardware_validated: false`. Flash commands
are printed for operator review, not executed. The package uses PlatformIO's
actual `firmware.bin`, `bootloader.bin`, and `partitions.bin`; it does not assume
the differently named files in ESP-IDF's generated `flasher_args.json` exist.

The sender image contains the current staged files plus `one.bin`, a single
byte `0x5A` (CRC32 **59BC5767**). The receiver image contains only `one.bin` to
leave space for reception. Neither `data/` nor earlier packages are modified.
If an existing `data/one.bin` differs, preparation stops rather than replacing it.

## 2. Match the physical board to exactly one profile

| Item | Custom PCB (default) | ESP32 devboard |
|---|---|---|
| Environment | `rf3_custom_pcb` | `rf3_esp32_devboard` |
| Expected module/flash | ESP32-WROOM-32UE-N16, **16 MB** | ESP32-WROOM-32D, **4 MB** |
| CE | GPIO17 | GPIO27 |
| CSN | GPIO5 | GPIO5 |
| IRQ (active low) | GPIO27 | GPIO26 |
| SCK / MOSI / MISO | GPIO18 / GPIO23 / GPIO19 | GPIO18 / GPIO23 / GPIO19 |
| Bus | SPI3, mode 0, 1 MHz | Same |
| Download/reset policy | **Manual**, `no_reset` before upload | Automatic reset when bridge circuitry supports it |
| SPIFFS offset / size | `0x190000` / `0xE70000` | `0x190000` / `0x270000` |

Firmware compile-time checks prevent mixing the two pin profiles. They cannot
check PCB traces or identify an assembled module. Check the markings and, after
safe power-up, use the package's `flash_id` command to confirm detected capacity.
A successful build or user-specified `--flash_size` does not identify real flash.

## 3. Electrical and programming gates — before any RF command

1. **Power disconnected:** inspect orientation and soldering; continuity-check
   each signal in the table against the actual schematic and module pin legend.
   Confirm common ground, no rail short, and that GPIO5's strap bias is preserved.
2. **Power design:** use a regulated 3.3 V rail at the ESP32/radio circuitry,
   following the exact board and radio-module ratings. Espressif recommends a
   3.3 V source capable of at least 500 mA for the ESP32 supply design; add the
   actual PA+LNA module demand and margin to the shared supply budget. This is
   supply capability guidance, not a universal bench current-limit setting.
   Do not assume a USB-UART adapter's 3.3 V pin can power both devices.
3. **Radio supply:** the PA+LNA module is not just the bare nRF24 IC. Verify its
   input rating, current demand, regulator arrangement and local decoupling
   against its vendor datasheet. The old blanket “nRF24 3.0–3.6 V” statement is
   not a valid module specification. Do not apply 5 V to an assumed 3.3 V rail.
4. **UART/programmer:** if the PCB lacks a USB-UART bridge, use a programmer with
   3.3 V logic. Adapter TX goes to ESP32 GPIO3/RXD0, adapter RX to GPIO1/TXD0,
   with common ground. Do not connect competing supply outputs or back-power an
   unpowered board through UART. The WROOM-32UE's connector is not the nRF24's
   antenna connection; confirm both module identities.
5. **First power:** use the board's intended input and appropriate current
   limiting. Measure at the ESP32 and radio pins through reset; stop for heating,
   rail collapse, oscillation, brownout, shorts, or unexpected current. Keep RF
   commands off. Recheck with the radio disconnected only after powering down.
6. **Antennas:** have the specified nRF24 antenna or suitable RF load connected
   before any TX/CW operation. Use authorized test conditions. Start packet
   tests at `POWER 0`; that command selects the chip's power setting and does
   not establish the module's radiated power.

Power and UART details above follow the
[Espressif schematic checklist](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32/schematic-checklist.html)
and [WROOM-32E/32UE datasheet](https://documentation.espressif.com/esp32-wroom-32e_esp32-wroom-32ue_datasheet_en.html).
The actual PCB and PA+LNA module still need their own review.

## 4. Custom-board manual boot and upload

The custom profile intentionally uses `board_upload.before_reset = no_reset`.
It does not put the ESP32 into the ROM downloader for you.

1. Close the serial monitor. Identify the correct board port by its USB-UART
   identity and unplug/replug behavior; never choose COM1 merely because listed.
2. Hold **BOOT/GPIO0 low**, pulse **EN/reset low then release EN**, and keep BOOT
   asserted through that reset. GPIO2 must not be driven high for download mode.
3. Run the package's `flash_id` command and confirm capacity. If the board has
   no DTR/RTS reset circuit, a `hard_reset` message alone does not reset it.
4. Enter download mode again before the selected sender/receiver flash command.
   Use the correct profile directory and role. The write replaces application,
   partition table and filesystem regions: **back up wanted board files first**.
5. Release BOOT so GPIO0 can return high, then pulse EN to boot the application.
   Open a **115200 baud** monitor, with DTR/RTS deasserted. The PlatformIO profile
   sets `monitor_dtr = 0` and `monitor_rts = 0` for this reason.

GPIO0 low at reset selects the downloader; high selects normal execution.
GPIO12 held high can select an inappropriate flash supply voltage on a 3.3 V
flash design. GPIO5 is also a strap pin, but is not the BOOT button signal.
See [Espressif boot-mode selection](https://docs.espressif.com/projects/esptool/en/latest/esp32/advanced-topics/boot-mode-selection.html).
If the PCB does not expose EN/GPIO0/UART, resolve programming access first.

Do not use chip erase as a routine response to an RF failure. Do not use the
normal `uploadfs` command for the lean receiver: that command packs `data/`,
which contains both large fixtures. Use the generated receiver image command.
These are development images, not a secure-boot/flash-encryption provisioning
procedure. If a board is already security-provisioned, review its provisioning
requirements before using these commands.

## 5. First boot and first physical transfer

Capture both serial logs. Boot must report the expected `nRF24 pinset=...`,
`Build profile=... filesystem=ready HTTP-control=disabled`, and `Radio boot OK`.
Run `HELP`, `STATUS`, and `FILES`. Expect an idle Standby radio, no fault, and
the right file inventory. No autonomous transmit is enabled in the tracked
profiles. A failed radio register probe or SPIFFS mount keeps the console
available; a fatal HAL/allocation error can still abort and needs its full log.

Before testing RF, use a logic analyzer or scope to validate SPI/CE/IRQ. The
driver's successful RF_CH probe is evidence of one register exchange, not proof
of every register, radio identity, PA operation, or an RF link. Expected writes:

```text
CONFIG: 0x0C at setup, 0x0E in powered standby, 0x0F in RX
EN_AA=0x00  EN_RXADDR=0x01  SETUP_AW=0x03  SETUP_RETR=0x00
RF_CH=76   RF_SETUP=0x26   RX_PW_P0=32    DYNPD=0  FEATURE=0
RX_ADDR_P0 and TX_ADDR: 52 46 33 24 01
```

These are the repository's intended settings, to be **measured** on both nodes.
Protocol v2 supplies application ACKs/retries; nRF hardware auto-ACK is disabled.
Observe TX-to-RX turnaround, CE pulse, IRQ clearing and rail droop on the real
system if READY/ACK replies are missed; host tests do not measure this timing.

At the bench, under permitted test conditions:

Use `CHANNEL PREVIEW 76` before selecting a channel; it prints the nominal
2476 MHz frequency and leaves the radio unchanged. To verify the preview on
hardware, start RX, run `CHANNEL PREVIEW 42`, and check that `STATUS` remains
`RxListening`, `channel=76`, `frequency_mhz=2476`. The preview should report
2442 MHz without stopping RX or discarding an in-progress transfer. Only
`CHANNEL 42` applies that selection; restore both nodes to channel 76 before
the following link test. This preview calculates a frequency, not RF occupancy.

| Receiver B | Sender A |
|---|---|
| `CHANNEL 76` then `POWER 0` | `CHANNEL 76` then `POWER 0` |
| `RX` | `TX one.bin` |
| `STATUS`, then `FILES` | `STATUS` after completion |

Pass only when both ends identify the same transfer, one byte / one DATA packet,
CRC32 **59BC5767**, receiver publication, and sender Completed /
`peer_complete=true`. `tx_ok=true` alone only describes local radio transmission,
not reception or publication. Repeat in reverse using the one-byte fixture on
the other node, then a bounded 20-transfer one-byte loop. Only then attempt
`speech_test.u8` and `song.u8`, using the expected counts in the qualification
report. Follow [hardware_validation.md](hardware_validation.md) for the full
ordered test campaign, and record observations in [board_bench_log.csv](board_bench_log.csv).

## 6. Receiver storage is a test constraint

The devboard SPIFFS partition is **2,555,904 bytes**. Staging both existing audio
files and receiving another 1,112,701-byte song exceeds even that raw capacity.
Use the lean receiver image for both profiles, and test one large transfer at a
time on the devboard. Valid received files accumulate; there is no console file
delete command. Preserve evidence before deliberately refreshing the receiver
filesystem. Do not run indefinite loops with large files.

Espressif notes that SPIFFS reliably uses roughly 75% of its partition, writes
can pause for garbage collection, and power interruption can corrupt it.
The handoff's 75% budget is only planning headroom; measure actual free space
from boot output, storage latency, and cleanup behavior on hardware. A host
pack/unpack success does not prove live mount/write behavior.
[ESP-IDF SPIFFS notes](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32/api-reference/storage/spiffs.html)

For byte-level evidence from an actual board, stop transfers, save both logs,
then read the SPIFFS partition back with the pinned esptool `read_flash` command
using this profile's offset and size. Unpack that captured image with the same
ESP-IDF mkspiffs tool and compare the actual `rx_XXXXXXXX.bin` to the source.
The operation must use the real flash readback, not the original upload image.
Keep the readback with its SHA-256 and transfer ID. Re-enter manual download mode
for each command when needed; do not overwrite earlier captures.

## Remaining release gates

- Actual schematic, PCB revision, module orientation and pin mapping confirmed.
- Power/ground/boot/UART checks passed on both boards.
- Detected flash capacities match; intended images uploaded and verified.
- Both actual SPIFFS mounts and radio register/CE/IRQ behavior passed.
- Bidirectional one-byte link, repeated sessions and large-file physical transfers
  passed with matching IDs/counts/CRCs and read-back file comparison.
- Stack/heap margins and scheduling observed under real ESP-IDF workload; no
  unsupported claim that the host qualification measured these.
- RF output, antenna performance/range, coexistence and fabricated PCB operation
  validated as required by the project. CW is not a prerequisite for the first
  packet test and must stay off until an appropriate controlled test is ready.

Until these gates have measured results, the defensible status is **software
and host images prepared; physical implementation awaiting bench validation**.

## Recorded host preparation — 2026-08-30

The latest local package is `.pio/board_handoff/run-082dqd2z`, with
`HOST_ARTIFACT_CHECKS_PASS` and `hardware_validated: false` in its manifest.
Both profiles passed the image/partition/configuration checks above; all four
role-specific SPIFFS images packed and unpacked with matching content hashes.
Each sender image has four files; each lean receiver has the one-byte fixture.
This package includes channel preview and supersedes the earlier readiness
package `run-z2p03mj1`, which does not contain that feature.

Validation after adding channel preview: **185/185 native tests**, **16/16 Python
tests**, **264/264 software qualification cases** (`run-hm3_7eip`), **2/2 canonical
firmware environments**, and **2/2 canonical SPIFFS builds** passed. Both firmware
builds completed without compiler warnings or errors. Repository hygiene and
whitespace checks also passed. The three compatibility aliases passed before
this feature was added; they were not rebuilt in this refresh.

Firmware changes are limited to command parsing, a nominal frequency helper,
serial/HTTP preview, and frequency in status/selection output. Radio drivers,
protocol, hardware profiles, pin configuration, and tracked payload fixtures
are unchanged. The actual console/HTTP behavior still needs board validation.
No upload, erase, or physical radio test was performed.
