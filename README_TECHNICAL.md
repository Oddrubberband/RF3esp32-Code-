# RF3 technical README

This document is the engineering reference for the current RF3 firmware. For a
guided first-use explanation, read [README_BEGINNER.md](README_BEGINNER.md).
For the live development state, always start with
[docs/project_handoff.md](docs/project_handoff.md).

## Current release state

The current Phase 1 branch is `codex/phase-one-laptop` on
`https://github.com/Oddrubberband/RF3esp32-Code-.git`. Phase 1 combines the
correctness closure, the restored pre-Phase-1 operator features, and the
confirmed board pin correction. Phase 2 has not started.

Software validation currently records:

- 213 native Unity tests passed.
- 16 Python tooling tests passed.
- 264 software qualification cases passed.
- Both canonical firmware and SPIFFS builds passed.
- Repository hygiene and whitespace checks passed.

One physical transfer has been recorded from the custom PCB to the ESP32
devboard. A 1,112,701-byte `song.u8` file completed in 55,636 DATA packets with
CRC32 `5138505B` at 1,908 B/s TX and 1,919 B/s RX, with one retry. The receiver
published `rx_90757F3C.bin`. About 116 seconds later, the idle listening receiver
reported fault 4 after an RX read failure. `STANDBY` followed by `RX` restored
normal operation. That transient fault classification remains open and must be
fixed before Phase 2.

## System purpose and boundary

RF3 transfers arbitrary binary files between ESP32 nodes using fixed-width
nRF24L01+ packets. `.u8` audio remains supported as ordinary binary data. The
firmware also exposes serial radio diagnostics, Morse transmission, continuous
wave bench controls, file selection, transfer loops, and local SPIFFS
maintenance.

The current implementation does not include transfer resume, persistent replay
history, filename transport to the receiver, encryption, authentication,
Bluetooth, a browser file manager, or the planned shared storage layer. Wi-Fi
and HTTP control code exists for isolated development but is disabled in every
tracked build.

## Authoritative hardware profiles

These mappings are confirmed and compile-time checked. Do not substitute the
custom and devboard CSN pins.

| Signal | Custom PCB `rf3_custom_pcb` | Devboard `rf3_esp32_devboard` |
|---|---:|---:|
| CE | GPIO17 | GPIO27 |
| CSN | GPIO27 | GPIO5 |
| IRQ | GPIO16 | GPIO26 |
| SCK | GPIO18 | GPIO18 |
| MOSI | GPIO23 | GPIO23 |
| MISO | GPIO19 | GPIO19 |
| Flash | 16 MB | 4 MB |
| SPIFFS offset | `0x190000` | `0x190000` |
| SPIFFS size | `0xE70000` | `0x270000` |

**Custom PCB: CSN=GPIO27 and IRQ=GPIO16.** The older custom mapping using
CSN=GPIO5 and IRQ=GPIO27 is superseded and caused the radio probe to fail.
GPIO5 remains correct only for the devboard CSN. Any future pin change requires
confirmation against the actual board.

The profiles are defined together in [platformio.ini](platformio.ini),
[include/hardware_profile.hpp](include/hardware_profile.hpp), native tests, and
the offline handoff tool. [AGENTS.md](AGENTS.md) contains the standing pin
warning.

## nRF24 physical configuration

The current packet-mode defaults are:

| Setting | Value |
|---|---|
| Default channel | 76, nominally 2476 MHz |
| RF setup | `0x26`, 250 kbps |
| Static payload width | 32 bytes |
| Address width | 5 bytes |
| RX/TX address | `52 46 33 24 01` |
| Hardware auto-ACK | Disabled, `EN_AA=0x00` |
| Hardware retransmit | Disabled, `SETUP_RETR=0x00` |
| Dynamic payloads | Disabled |
| SPI | SPI3, mode 0, 1 MHz |
| Firmware CE pulse | 40 microseconds in canonical builds |
| Runtime power | `POWER 0` through `POWER 3` |

Protocol v2 supplies application-level ACKs and retries. Both nodes must use the
same channel, RF data rate, address, and packet format. `CHANNEL <0-125>`
reinitializes the radio; `CHANNEL PREVIEW <0-125>` only reports the nominal
`2400 + channel` MHz value.

## Protocol v2 wire flow

Every wire frame is exactly 32 bytes. A DATA frame uses 12 bytes of header and
carries at most 20 unique file bytes. The maximum transfer is 65,536 DATA
packets, or 1,310,720 bytes.

The successful flow is:

```text
Sender                         Receiver
  START(size, packets, CRC)  ->
                          <-  READY
  DATA(sequence, up to 20 B)->
                          <-  ACK(sequence)
  ...one DATA/ACK exchange per chunk...
  END(size, packets, CRC)    ->
                          <-  COMPLETE
```

The sender does not advance its source until the current DATA sequence is
acknowledged. The receiver writes only the next expected sequence. Duplicate
DATA is not written twice; gaps produce a NACK for the expected sequence. END
does not complete until the declared packet count, byte count, metadata, and
calculated CRC all agree and the file has been published successfully.

Current timing and retry defaults are:

| Exchange | Timeout | Retries |
|---|---:|---:|
| START/READY, END/COMPLETE, CANCEL | 500 ms | 5 |
| DATA/ACK | 250 ms | 20 |
| Sender/receiver inactivity | 10,000 ms | n/a |

The DATA retry window is statically constrained below receiver inactivity.
Application ACKs currently occur after every 20-byte DATA chunk. Reducing ACK
frequency requires a coordinated cumulative-ACK/window protocol change on both
boards; suppressing ACKs in the current state machine would cause timeouts.

Packet types are START, READY, DATA, ACK, NACK, END, COMPLETE, ERROR, and CANCEL.
Error values are stable through `InsufficientStorage=23`. See
[docs/protocol_v2.md](docs/protocol_v2.md) for encoding rules and state-machine
details.

## Phase 1 correctness behavior

Phase 1 adds these protections without removing the earlier operator features:

- Impossible nRF STATUS/FIFO snapshots cannot report false TX success. Invalid
  SPI observations lower CE, suppress fallback transmission, and report fault
  10.
- Cleanup failures survive cancellation, timeout, and failure response paths.
  Duplicate CANCEL can retry failed cleanup.
- Four successful completion records are kept in bounded RAM. A delayed
  matching START receives COMPLETE rather than reopening storage. Changed
  metadata under a remembered ID is rejected.
- Serial and HTTP status use an owned snapshot protected by a short-held,
  separate mutex. Status does not wait on a long transfer or force a hardware
  probe.
- Selected filenames are owned rather than borrowed through unsafe `c_str()`
  pointers. JSON text is escaped.
- TX and RX report accepted-byte milestones every 10 percent.
- Control and DATA exchanges use distinct retry budgets.

Replay history is lost on reboot, successful explicit reset, or four-entry
eviction. A status read reports the latest published hardware snapshot rather
than performing SPI synchronously.

## Storage behavior

SPIFFS is mounted at `/spiffs`. Sender files are streamed rather than loaded
wholly into RAM. A receiver writes into an internal `.part` file, closes and
CRC-checks it, then publishes it under a collision-safe name:

```text
rx_<8-hex-digit-transfer-id>.bin
rx_<8-hex-digit-transfer-id>_001.bin
```

Internal partials are hidden from the ordinary `FILES` listing and cleaned at
startup. Existing completed files are not overwritten. Before accepting START,
the receiver reserves 25 percent of total SPIFFS capacity for garbage
collection. A file larger than the remaining safe capacity is rejected with
`InsufficientStorage`.

Local serial filesystem commands are:

| Command | Effect |
|---|---|
| `FILES` | List normal files and show the selected sender file |
| `FS INFO` | Show total, used, free, safe receive capacity, and classifications |
| `FS LIST ALL` | Include visible, hidden, and internal partial files |
| `FS DELETE <file>` | Delete one visible file |
| `FS CLEAN PARTIALS` | Delete stale internal receive fragments |
| `FS FORMAT CONFIRM` | Erase every SPIFFS file |

Mutation commands refuse to run during active loops or receive writes. FS
commands are rejected through the RF remote-command path. Formatting and
`uploadfs` are destructive to stored board files.

## Firmware concurrency and data ownership

`src/main.cpp` integrates four principal execution contexts:

- The serial command loop.
- The `radio_rx` worker, polling at 2 ms and draining no more than 32 packets
  per service pass.
- The `radio_loop` worker for TX, CW, and Morse loops.
- The optional Wi-Fi control task, excluded by default.

Radio access is serialized by `radio_mutex_`. Loop configuration and command
dispatch have separate mutexes. `AppStatus::SnapshotCache` has its own
short-held mutex and owns all strings. Radio owners publish cached state when
releasing ownership and during sender progress. The idle worker refreshes radio
diagnostics every 250 ms when it can acquire the radio lock.

This arrangement keeps STATUS responsive during a long stop-and-wait transfer
and avoids accessing a filename after its originating `std::string` changed.

## Source map

| Path | Responsibility |
|---|---|
| `src/main.cpp` | ESP-IDF startup, console, tasks, SPIFFS adapters, transfer integration |
| `src/nrf24.cpp` | nRF register access, packet TX/RX, CW behavior |
| `src/radio_manager.cpp` | Radio state machine and diagnostic fault codes |
| `include/protocol_v2.hpp` | Wire codec, packet validation, CRC32 |
| `include/reliable_transfer_v2.hpp` | Sender/receiver protocol state machines |
| `include/file_transfer_service.hpp` | Streaming subsystem API and reports |
| `include/app_status.hpp` | Owned cached snapshots and JSON serialization |
| `include/hardware_profile.hpp` | Compile-time board pin validation |
| `test/test_nrf24/` | Native unit and regression tests |
| `tools/run_firmware_qualification.py` | Deterministic software transfer campaign |
| `tools/prepare_board_handoff.py` | Offline firmware/filesystem package validation |

## Build and test

PowerShell setup:

```powershell
py -m venv .venv
& .\.venv\Scripts\python.exe -m pip install -r requirements.txt
```

Canonical checks:

```powershell
& .\.venv\Scripts\platformio.exe test -e native
& .\.venv\Scripts\python.exe -B -m unittest discover -s test -p 'test_*.py'
& .\.venv\Scripts\platformio.exe run -e rf3_custom_pcb -e rf3_esp32_devboard
& .\.venv\Scripts\platformio.exe run -e rf3_custom_pcb -e rf3_esp32_devboard -t buildfs
& .\.venv\Scripts\python.exe tools\run_firmware_qualification.py
& .\.venv\Scripts\python.exe tools\check_repository_hygiene.py
git diff --check
```

The qualification campaign is host simulation, not measured RF throughput.
Generated evidence under `.pio/` is intentionally ignored.

## Flash and monitor

Substitute the actual ports. Do not leave a serial monitor open on a port while
uploading.

```powershell
& .\.venv\Scripts\platformio.exe run -e rf3_custom_pcb -t upload --upload-port COM5
& .\.venv\Scripts\platformio.exe device monitor --port COM5 --baud 115200 --dtr 0 --rts 0
```

```powershell
& .\.venv\Scripts\platformio.exe run -e rf3_esp32_devboard -t upload --upload-port COM3
& .\.venv\Scripts\platformio.exe device monitor --port COM3 --baud 115200 --dtr 0 --rts 0
```

The custom PCB requires manual ROM download entry: hold BOOT/GPIO0 low, pulse
EN/reset, start upload, then release BOOT when transfer begins. Pulse EN again
to run the application. Use 3.3 V UART logic and follow
[docs/board_bringup.md](docs/board_bringup.md) before first power or RF output.

Firmware upload does not automatically replace SPIFFS. Use `uploadfs` only when
you deliberately intend to overwrite that board's filesystem.

## Serial command reference

`HELP` prints the authoritative command list. Major controls are:

- `STATUS`, `FILES`, and the five `FS` operations for inspection.
- `SELECT <file>`, `TX [file]`, and `TX LOOP [count|INF] [file]`.
- `RX`, `STANDBY`, `STOP`, `SLEEP`, `WAKE`, and `POWERDOWN`.
- `CHANNEL <0-125>`, `CHANNEL PREVIEW <0-125>`, and `POWER <0-3>`.
- `MORSE <text>`, `CW START [channel] [power]`, and `CW LOOP`.

STATUS is a human-readable cached snapshot. With locally enabled Wi-Fi, the
unauthenticated development endpoint `GET /status` emits JSON, but tracked
builds keep the entire HTTP control plane disabled.

## Known fault codes and current issue

The most important diagnostics are fault 1 for startup SPI probe/enter-RX
failure, fault 3 for TX failure or timeout, fault 4 for RX read failure, and
fault 10 for an impossible SPI status snapshot during TX. Inspect register
values and the operation log rather than treating the integer alone as a root
cause.

The recorded post-transfer fault 4 recovered immediately with:

```text
STANDBY
RX
STATUS
```

The recovered snapshot was RxListening, fault 0, STATUS `0x0E`, FIFO `0x11`,
and IRQ high. This suggests a transient empty-FIFO/read classification rather
than loss of the radio. It still requires a code fix and regression before the
next phase.

## Next authorized sequence

Phase 2 is Measurement Foundation: audit existing telemetry and add missing
unique-payload throughput, exchange counts, first-attempt success, retry
distribution, completion results, stage timings, byte counts, CRCs, RF
configuration, and structured CSV/JSON output. After its exit gate comes the
physical RF/power baseline. Performance changes such as cumulative ACK windows
or a higher RF data rate should be evaluated against those measurements.

Do not begin the shared storage layer, browser file manager, filename protocol
extension, resume, Bluetooth, or further performance optimization before the
earlier gates are authorized and complete.

