# RF3 software qualification results — 2026-09-03

**Software qualification: PASS. Physical RF remains unvalidated.**

Run from the repository root:

```sh
python tools/run_firmware_qualification.py
```

The recorded final run is `.pio/qualification/run-1sgnbtwc`. Its `report.txt`
is suitable for a presentation screenshot; `report.json` and `transfers.jsonl`
contain the full measurements and input hashes. Artifacts are intentionally
ignored by Git and retained locally. Re-running creates a new evidence directory.

## Quantitative evidence

| Category | Measured result | Verdict |
|---|---|---|
| `song.u8` integrity | TX / ACK / RX each **1,112,701 bytes**; TX / ACK / accepted each **55,636 DATA packets**; CRC32 source and persisted destination **5138505B**; **0 byte errors**; both endpoints Completed; one publication | PASS |
| Individual START, READY, DATA, ACK, END, COMPLETE loss | **6/6** complete; one actual drop and one retry per case; exact data/publication checks pass | PASS |
| Deterministic loss campaign | **100 attempted / 100 completed / 0 failed**; **3,006 actual drops / 3,006 retries**; **3,363,919 verified payload bytes**; **0 final integrity failures** | PASS |
| Retry bounds | Worst **36 retries per transfer**; worst **4 consecutive retries per exchange**, within the configured five-retry limit | PASS |
| Corruption rejection | **28/28** payload corruptions detected; both endpoints Failed / CrcMismatch (16); **0 publish calls / 0 corrupted final files**; **28/28 partial files removed** | PASS |
| Recovery after corruption | **28/28** subsequent clean transfers complete on the same service/receiver/sink | PASS |
| Maximum transfer | TX / ACK / RX each **1,310,720 bytes**; TX / ACK / accepted each **65,536 DATA packets**; CRC32 source/disk **3D4C8B1E**; **0 pattern errors**; final DATA sequence **65,535**, next sequence **65,536**; both endpoints Completed | PASS |
| Back-to-back stress | **100/100** successful transfers using the same sessions; **14,462,040 verified bytes**, including ten maximum transfers; **196 stale prior-transfer frames ignored**; **0 earlier-output byte errors** after re-reading all 100 publications | PASS |

The campaign executes **264 transfer cases**, including 28 expected negative
cases. The 236 valid outputs each publish exactly once; every rejected output is
removed. The host campaign plus independent Python verification took **23.75 s**
on this run, excluding compilation. This is not a throughput benchmark and does
not predict transfer duration on ESP32 or over RF.

## Reproducibility and failure sensitivity

Configuration and exact seed-generation/rule-generation algorithms are in
[firmware_qualification.md](firmware_qualification.md). The base seed is
`0x52463332`, production timeouts/retry limits are unchanged, and simulated time
steps are 10 ms. The final host compiler was MSYS2 GCC **16.1.0**, Python
**3.12.13**, with C++17, optimization level O2, strict warnings, and no compiler
warnings in the qualification build.

Fixture SHA-256 (`data/song.u8`):

```text
9de95cfc106a48e3a429ccf81eecded128116c6146375d583f28f58bd539a7ec
```

The Git base was `7c8c4bae676d61e965581aea4a8d892bc1ff55c6`, with the qualification
changes uncommitted. `report.json` records SHA-256 hashes of the actual production
headers, harness, runner and fixture so the dirty working-tree run is traceable.
Two separate unmodified runs produced **byte-identical measurements for all
264 records**. Their `transfers.jsonl` SHA-256 is:

```text
5d7bbca4957f550977eabdcef6378ed91a60aa00d829abcb4548e139d57fb0d8
```

Failure checks were actually executed:

- An unavailable compiler produced overall FAIL and runner exit code 1.
- A fixture with the wrong size produced executable exit code 2, incomplete
  category failures, overall FAIL, and runner exit code 1.
- In an isolated ignored copy of the production headers, the receiver's CRC
  mismatch rejection branch was disabled. All **28 invalid publications** were
  observed and flagged, the native executable returned **1**, and the reporter
  returned overall **FAIL**. This demonstrates that the host sink oracle does
  not silently compensate for a broken production CRC gate. The repository's
  production header was never changed; the subsequent unmodified run passed.

## Existing regression and build matrix

| Check | Final result |
|---|---|
| Native Unity regression suite | **182/182 PASS** (181 pre-existing tests plus one fault-counter instrumentation regression) |
| Existing Python tests | **8/8 PASS** |
| `rf3_custom_pcb`, clean firmware build | **PASS**, 109.32 s |
| `rf3_esp32_devboard`, clean firmware build | **PASS**, 89.55 s |
| `esp32wroom32d`, clean firmware build | **PASS**, 88.26 s |
| `esp32wroom32d_manual_boot`, clean firmware build | **PASS**, 88.94 s |
| `esp32wroom32d_devboard`, clean firmware build | **PASS**, 88.92 s |
| Canonical custom PCB SPIFFS image | **PASS** |
| Canonical devboard SPIFFS image | **PASS** |
| Existing repository hygiene script | **PASS** |
| Additional hygiene scan including new, untracked source files | **PASS** |
| `git diff --check` plus whitespace checks of new files | **PASS** |
| Generated-file/working-tree review | Only the ten intentional source/documentation changes; generated evidence/build output remains ignored |

No pre-existing assertion was weakened. Clean builds used the unchanged pinned
PlatformIO Core **6.1.19**, Espressif32 **7.0.1**, ESP-IDF **6.0.1**, and Xtensa
compiler **15.2.0** configuration. The five clean firmware builds took 464.99 s
total and produced no compiler warning/error diagnostics. The initial sandboxed
clean attempt was blocked by a Windows access restriction in the Xtensa compiler
launcher. Re-running the unchanged build commands with approved access outside
the sandbox resolved that environment problem; it required no repository or
firmware workaround. Both SPIFFS images were rebuilt after the firmware clean.

Local validation used `.venv` and `PLATFORMIO_CORE_DIR` pointing to the ignored
`.pio/core` cache. Logs are retained under `.pio/qualification/` as
`native-tests.log`, `python-tests.log`, `firmware-clean-builds.log`,
`spiffs-builds.log`, `hygiene-checks.log`, and `replay-check.log`.

## Production changes and scope

**No production firmware defect was found or fixed. No production firmware file
was modified.** The only shared infrastructure change adds measured application
counters to the test fake transport, with a regression checking occurrence and
repeat semantics. A dedicated qualification executable, Python runner,
documentation, and CI invocation were added.

Files added:

- `test/qualification/main.cpp`
- `tools/run_firmware_qualification.py`
- `docs/firmware_qualification.md`
- `docs/firmware_qualification_results.md`

Files updated:

- `test/include/protocol_v2_fake_transport.hpp`
- `test/test_nrf24/test_protocol_v2.cpp`
- `.github/workflows/validate.yml`
- `README.md`
- `test/README.md`
- `docs/build_and_test.md`

This is evidence about the production hardware-independent Protocol v2/service
path using simulated transport and host filesystem adapters. It does not validate
`src/main.cpp` integration at runtime, actual SPIFFS behavior, ESP32 scheduling
or memory use, physical RF transmission, real nRF24 interoperability, SPI signal
integrity/timing, power integrity, packet error rate over the air, radiated RF
output, antenna performance/range, or fabricated PCB functionality. CRC32 is not
a cryptographic guarantee or an exhaustive proof against every corruption.

Presentation conclusion:

> RF3 firmware was software-qualified for end-to-end data integrity,
> deterministic packet-loss recovery, corruption rejection, and maximum-size
> transfer handling; physical RF performance remains pending hardware validation.
