# RF3 transfer optimization and laptop handoff

Last updated: 2026-09-03

This document is the self-contained continuation record for the RF3 ESP32
repository. It exists so development can move to another computer without the
original Codex conversation.

## Repository identity and recovery points

- Remote: `https://github.com/Oddrubberband/RF3esp32-Code-.git`
- Working branch: `agent/rf3-pre-hardware-readiness`
- Implementation commit: `5300d40` (`Complete RF3 transfer optimization and qualification tooling`)
- Pre-change commit: `7c8c4bae676d61e965581aea4a8d892bc1ff55c6`
- Rollback tag: `rf3-before-transfer-optimization-20260903`
- Main branch observed during this work: `5fe9899`

The rollback tag points directly to the parent of the implementation commit.
The safest ways to inspect or undo the update are:

```powershell
# Inspect/build the earlier version without changing branch history.
git switch --detach rf3-before-transfer-optimization-20260903

# Return to the development branch.
git switch agent/rf3-pre-hardware-readiness

# Undo the implementation while preserving history, if that is ever needed.
git revert 5300d40
```

Do not use `git reset --hard` for ordinary rollback. A separate worktree is a
convenient way to compare the old and new versions side by side:

```powershell
git worktree add ..\rf3-before-optimization rf3-before-transfer-optimization-20260903
```

## Original objective

The source brief was `RF3 File Transfer Optimization.pdf`, originally located
at `C:\Users\leals\Downloads\RF3 File Transfer Optimization.pdf`. The PDF is
not committed to this repository.

The requested work was to audit the entire RF3 file-transfer path for delays,
drain limits, retry behavior, timeouts, and hot-path logging; improve healthy
throughput without weakening reliability; preserve empty-FIFO safety, IRQ
handling, sequencing, duplicate/out-of-order behavior, CRC verification,
bounded loops, size limits, STOP/cancellation, storage publication rules, and
mode exclusion; then validate the result with builds, tests, loss/corruption
campaigns, and a clear hardware follow-up plan.

## Implemented behavior

### Scheduler and receive servicing

- `CONFIG_FREERTOS_HZ` is now 1000 in `sdkconfig.defaults`. The intended 2 ms
  polling delay was previously rounded to the 100 Hz scheduler's 10 ms tick.
- The RX task uses a named 2 ms poll period and compile-time checks that the
  period is representable.
- The bounded receive-drain cap increased from 8 to 32 packets per service
  pass. STOP/failure exits are not misreported as cap exhaustion.
- Drain-cap hits are counted as `rx_drain_hits`; warnings occur on the first hit
  and every 64th hit instead of flooding the transfer hot path.

### nRF24 FIFO and IRQ safety

- `FIFO_STATUS.RX_EMPTY` is authoritative before `R_RX_PAYLOAD`.
- A stale `RX_DR` latch with an empty FIFO is cleared and returns no packet.
  The driver never reads an empty FIFO merely because the interrupt latch is
  still set.
- A nonempty FIFO is serviced even when `RX_DR` has already been cleared.
- Existing TX/RX IRQ cleanup, TX failure recovery, bounded status polling, and
  radio state normalization remain intact.

### Protocol reliability and lifecycle

- Protocol v2 stays wire compatible: version, 32-byte frames, 20-byte DATA
  payload, CRC32, sequence fields, and maximum 1,310,720-byte file size did not
  change.
- The retry budget remains five retransmissions for each outstanding START,
  DATA, END, or CANCEL exchange. It resets when that exchange advances.
  `totalRetries()` is cumulative telemetry only and is not a whole-file limit.
- Control timeout remains 500 ms, DATA ACK timeout remains 250 ms, and sender
  and receiver inactivity timeouts remain 10 seconds.
- STOP/CANCEL wait increased from 2 to 4 seconds so the initial CANCEL plus five
  bounded 500 ms retries can terminate normally.
- Receiver inactivity now measures forward progress: the first accepted START
  and each accepted in-order DATA packet refresh the deadline. Duplicate START
  or DATA, out-of-order DATA, and unexpected same-transfer traffic are answered
  as before but cannot preserve a partial file indefinitely.
- Transfer-ID generation now makes at most two random calls and then uses a
  nonzero deterministic fallback, eliminating a theoretical unbounded loop.
- Duplicate DATA remains idempotent: it is re-ACKed without another write or CRC
  update. A duplicate counter replaces per-packet warning spam.

### Measurement and diagnostics

- Successful and failed TX completion logs include `elapsed_ms` and
  `throughput_bps`.
- `STATUS` includes `tx_elapsed_ms`, `tx_bps`, `rx_duplicates`, and
  `rx_drain_hits`, in addition to protocol state, byte/packet progress, CRC,
  retries, IRQ state, FIFO state, and fault information.
- `docs/board_bench_log.csv` has matching columns for physical measurements.

### Qualification and board handoff support

- `tools/run_firmware_qualification.py` builds and runs the deterministic host
  qualification harness in `test/qualification/main.cpp`.
- `tools/prepare_board_handoff.py` prepares separate sender and lean receiver
  firmware/filesystem handoff artifacts without modifying `data/`.
- Radio-channel preview/selection parsing, hardware profiles, CI commands,
  staging tests, and the ordered board procedures are covered by the same
  committed readiness update.

## Guardrail disposition

Tuned:

- FreeRTOS tick rate and effective RX poll interval.
- Bounded RX drain limit and telemetry.
- STOP wait duration.
- Duplicate/drain hot logging.
- Receiver timeout semantics so only progress refreshes the deadline.

Kept unchanged:

- Empty-FIFO prohibition and bounded radio loops.
- IRQ clearing and TX/RX recovery.
- Sequence, duplicate, gap, wrong-transfer, and out-of-order handling.
- CRC32 validation and verified publication before COMPLETE.
- Source/packet/file-size limits and partial-file cleanup.
- Five retries per exchange and all wire timeouts.
- Mode mutexes, radio ownership, STOP, and cancellation state transitions.
- nRF24 power-up, RX-settle, CE-pulse, SPI-poll, and fallback timing delays.

Removed:

- Only unconditional per-duplicate warning output. The event remains counted
  and visible through diagnostics. No safety mechanism was removed.

Not normally active:

- nRF24 `MAX_RT` is not expected in the normal path because hardware auto-ack
  is disabled, but defensive handling remains.
- The generic transfer rate limiter remains configured to zero.
- Legacy/audio streaming pacing is separate from Protocol v2 file transfer and
  was not treated as a current file-transfer bottleneck.

## Final software evidence

The final validation performed on Windows/PowerShell was:

- PlatformIO native Unity: 189/189 passed.
- Python unittest discovery: 16/16 passed.
- Software qualification: 264 transfer cases passed.
- Firmware: all five tracked environments built successfully.
- SPIFFS: both canonical filesystem images built successfully.
- Repository hygiene: passed.
- `git diff --check`: passed; Git only reported normal LF-to-CRLF notices for
  the workflow and `sdkconfig.defaults` working copies.

Final canonical firmware sizes:

| Environment | RAM | Application flash |
| --- | ---: | ---: |
| `rf3_custom_pcb` | 14,188 / 327,680 bytes | 256,665 / 1,572,864 bytes |
| `rf3_esp32_devboard` | 14,320 / 327,680 bytes | 256,765 / 1,572,864 bytes |

Qualification details from the last run:

- Evidence directory on the original computer:
  `.pio/qualification/run-1sgnbtwc` (ignored by Git).
- `song.u8`: 1,112,701 TX/ACK/RX bytes, 55,636 DATA packets, source and
  destination CRC32 `5138505B`, zero byte errors, one verified publication.
- Maximum transfer: 1,310,720 bytes, 65,536 DATA packets, final sequence
  65,535, CRC32 `3D4C8B1E`, zero pattern errors.
- Targeted loss: START, READY, DATA, ACK, END, and COMPLETE loss each recovered
  with one actual drop and one retry.
- Deterministic loss campaign: 100/100 completed, 3,006 drops and retries,
  3,363,919 verified bytes, zero final integrity failures, worst retry burst 4
  against the allowed 5.
- Corruption campaign: 28/28 detected, zero invalid publications, all partials
  removed, and all 28 subsequent clean probes passed.
- Back-to-back campaign: 100/100 completed, 14,462,040 verified bytes, 196
  stale frames ignored, and zero earlier-output byte errors.

The `.pio` evidence is deliberately not versioned. Re-run the qualification on
the laptop to generate new machine-specific `report.txt`, `report.json`, and
`transfers.jsonl` files.

## Reproducing the environment on a laptop

For a new clone:

```powershell
git clone https://github.com/Oddrubberband/RF3esp32-Code-.git
cd RF3esp32-Code-
git fetch origin --tags
git switch agent/rf3-pre-hardware-readiness
git pull --ff-only
git show --stat 5300d40
git show rf3-before-transfer-optimization-20260903 --no-patch

python -m venv .venv
.venv\Scripts\python -m pip install -r requirements.txt
.venv\Scripts\Activate.ps1
```

For an existing clone:

```powershell
git fetch origin --prune --tags
git switch agent/rf3-pre-hardware-readiness
git pull --ff-only
```

Run the complete software checks from the repository root:

```powershell
platformio test -e native
python -m unittest discover -s test -p "test_*.py"
python tools\run_firmware_qualification.py
platformio run -e rf3_custom_pcb -e rf3_esp32_devboard -e esp32wroom32d -e esp32wroom32d_manual_boot -e esp32wroom32d_devboard
platformio run -e rf3_custom_pcb -e rf3_esp32_devboard -t buildfs
python tools\check_repository_hygiene.py
git diff --check
git status --short
```

PlatformIO versions and toolchain packages are pinned in `platformio.ini`.
Build output, the virtual environment, and qualification evidence are ignored
and must not be committed.

## Hardware profiles

Canonical custom PCB (`rf3_custom_pcb`):

- ESP32-WROOM-32UE-N16 / 16 MB.
- CE 17, CSN 5, IRQ 27, SCK 18, MOSI 23, MISO 19.
- Partition file: `partitions.csv`.

Canonical development board (`rf3_esp32_devboard`):

- ESP32-WROOM-32D / 4 MB.
- CE 27, CSN 5, IRQ 26, SCK 18, MOSI 23, MISO 19.
- Partition file: `partitions_4mb.csv`.

Serial upload and monitor ports are intentionally not committed. Substitute the
actual laptop port:

```powershell
platformio run -e rf3_custom_pcb -t upload --upload-port COMx
platformio run -e rf3_custom_pcb -t uploadfs --upload-port COMx
platformio device monitor --baud 115200 --port COMx
```

## Remaining physical validation

Software qualification does not establish RF or electrical behavior. The next
work item is the physical two-board campaign in this order:

1. Follow `docs/board_bringup.md` to identify boards, verify power, flash the
   correct profile, and confirm SPIFFS and radio register health.
2. Follow `docs/hardware_validation.md` from the one-byte transfer through
   repeated loss/retry, STOP with a missing peer, exact-full audio transfer,
   maximum transfer, corruption/no-publication checks, and long soak testing.
3. Record both consoles and enter results in `docs/board_bench_log.csv`.
4. For every transfer record source/destination CRC, bytes, packets, retries,
   `tx_elapsed_ms`, `tx_bps`, `rx_duplicates`, `rx_drain_hits`, publication
   path, resets/watchdogs, supply behavior, distance, antennas, and power level.
5. Treat any CRC mismatch, corrupted publication, unbounded wait, watchdog
   reset, partial-file leak, or inability to STOP as a hard failure.

Use `python tools\prepare_board_handoff.py` for reproducible sender/receiver
images. The lean receiver filesystem is important on the 4 MB profile because
it avoids consuming receiver capacity with sender fixtures.

## Known deferred bottlenecks and risks

These were intentionally not changed without physical measurements:

- Protocol v2 is stop-and-wait with only 20 DATA bytes per 32-byte RF frame.
  This is the dominant architectural throughput ceiling.
- The nRF24 PHY remains 250 kbps and SPI remains 1 MHz.
- Hardware timing delays, including the pre-CE delay, remain conservative.
- Source inspection reads the file before transmission to determine size and
  CRC, so the complete user operation includes a pre-scan.
- The firmware does not yet perform an explicit SPIFFS free-space reservation
  for the entire incoming file before accepting START. Mid-transfer writes are
  still checked and failures clean up without publication.
- Some live radio snapshot/status paths make redundant SPI register reads, but
  they are outside the normal DATA hot path.

Do not raise PHY/SPI rates, remove radio settle delays, introduce a transfer
window, or change frame formats until the current build has a recorded physical
baseline and fault-injection comparison.

## Committed file map

Core implementation and configuration:

- `sdkconfig.defaults`
- `src/main.cpp`
- `src/nrf24.cpp`
- `src/radio_manager.cpp`
- `include/reliable_transfer_v2.hpp`
- `include/rx_drain.hpp`
- `include/radio_channel.hpp`
- `include/command_parser.hpp`

Tests and qualification:

- `test/test_nrf24/test_main.cpp`
- `test/test_nrf24/test_protocol_v2.cpp`
- `test/include/protocol_v2_fake_transport.hpp`
- `test/qualification/main.cpp`
- `test/test_board_handoff.py`
- `tools/run_firmware_qualification.py`
- `tools/prepare_board_handoff.py`
- `.github/workflows/validate.yml`

Documentation and handoff:

- `README.md`
- `docs/build_and_test.md`
- `docs/protocol_v2.md`
- `docs/pre_hardware_readiness.md`
- `docs/firmware_qualification.md`
- `docs/firmware_qualification_results.md`
- `docs/board_bringup.md`
- `docs/hardware_validation.md`
- `docs/board_bench_log.csv`
- `test/README.md`

## Ready-to-paste prompt for a new Codex task

Paste the following into a new task opened on the laptop:

```text
Continue the RF3 ESP32 file-transfer optimization and hardware-readiness work.
The repository is https://github.com/Oddrubberband/RF3esp32-Code-.git and the
working branch is agent/rf3-pre-hardware-readiness. First read
docs/rf3_transfer_optimization_handoff.md completely, then inspect git status,
commit 5300d40, and tag rf3-before-transfer-optimization-20260903. Preserve all
current guardrails and do not change the Protocol v2 wire format, retry count,
timeouts, PHY rate, SPI rate, or radio settle delays without physical evidence.

Software validation was green: 189/189 native tests, 16/16 Python tests, all
264 qualification cases, all five firmware environments, both SPIFFS images,
repository hygiene, and whitespace checks. The remaining task is physical
two-board validation using docs/board_bringup.md, docs/hardware_validation.md,
and docs/board_bench_log.csv. Re-run software checks on this laptop before
flashing. Record elapsed_ms/throughput_bps, CRCs, retries, duplicates, drain
hits, resets, power, range, and exact artifacts. If hardware is not connected,
prepare the commands and artifacts but do not invent hardware results.
```

## Bottom line

The software update is implemented, committed, rollback-addressable, and fully
qualified in simulation. The sole acceptance gap is physical ESP32+nRF24
validation. The repository documents exactly how to perform and record it.
