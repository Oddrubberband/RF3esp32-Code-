# Phase 1 reliability fixes

This change started from `aa911c3` on the isolated
`codex/phase-one-reliability` branch and was fast-forwarded into active branch
`agent/rf3-pre-hardware-readiness` on 2026-09-07. Both committed hardware pin
profiles were unchanged in that implementation. No device was connected or
flashed during that validation. The annotated rollback tag
`rf3-before-phase-one-integration-20260907` preserves the pre-integration state.

## Custom-PCB pin correction - 2026-09-10

The laptop's previous local firmware used CE=17, CSN=27, IRQ=16. That local
correction was absent from committed baseline `aa911c3` and the integrated
Phase 1 source, which used CE=17, CSN=5, IRQ=27. After flashing Phase 1, the
user reported custom-PCB startup `fault=1` (SPI register probe failure), while
the devboard booted in Standby with `fault=0`. After reflashing correction
`74654ce`, the user's September 10 custom-PCB status screenshot shows Standby,
`fault=0`, `power=3`, `last_status=0x0E`, `fifo=0x11`, and `irq=high`.
SPIFFS remains mounted with the previously staged files. This confirms radio
initialization recovery, not RF transfer success, throughput, or IRQ timing.

The user authorized restoring the previous local mapping. The firmware build
flags, compile-time hardware profile, native pin assertions, offline handoff
profile, and current pin documentation now agree on CE=17, CSN=27, IRQ=16.
Devboard pins remain CE=27, CSN=5, IRQ=26. Reliability logic, SPI bus pins,
partitions, and Wi-Fi defaults are unchanged. Do not use older custom-PCB
packages that encode CSN=5 and IRQ=27. No Phase 2 work is part of this correction.

The corrected source passed 211 native tests, 16 Python tests (including the
offline handoff configuration checks), repository hygiene, whitespace checks,
and both canonical firmware builds. Custom-board startup after reflashing is
confirmed by the user-supplied status screenshot described above. Qualification,
Wi-Fi validation, and complete offline image packages were not rerun for this
pin-only correction. The broader results below describe September 7.

## Behavior

- TX rejects impossible STATUS or FIFO_STATUS values before interpreting
  success. A floating SPI bus no longer reports a successful local send. The
  driver lowers CE, skips recovery transmission on an invalid snapshot, and
  reports radio fault 10. Ordinary success, MAX_RT, timeout, and the supported
  no-ACK FIFO completion path retain their existing behavior. A valid zero
  STATUS value is not treated as proof of a disconnected radio.
- Receiver cancellation, timeout, and failure responses carry the actual
  post-cleanup error. Repeated CANCEL retries failed cleanup; a successful
  repeated cancellation remains idempotent. Prepare-time cleanup failure is
  also visible through the receiver's cleanup flag.
- The receiver remembers the last four successful transfers in fixed-size
  RAM. A matching delayed START returns COMPLETE without reopening storage,
  replacing another active session, or reporting another publication. Reuse
  of a remembered ID with different metadata is rejected. Explicit successful
  reset, reboot, and FIFO eviction bound this replay protection. The wire
  format is unchanged; new transfers still require fresh IDs.
- Serial and HTTP status read owned cached snapshots through a separate
  mutex. They do not acquire the radio mutex or perform filesystem/SPI reads.
  The radio owner publishes state on release and during sender progress.
  The worker also refreshes idle diagnostics every 250 ms when it can acquire
  the radio lock, so a disconnected idle radio does not remain hidden forever.
  Selection changes share the snapshot lock, and captured filenames remain
  valid after subsequent selections. Lock ordering never requires taking the
  radio or command mutex while holding the snapshot mutex.
- HTTP status preserves its existing fields and adds sender/receiver progress,
  preparation state, publication timestamp, and explicitly named payload
  bytes-per-second reporting. Text is JSON-escaped. Cached radio values reflect
  the most recent owner update; reading status does not force a hardware probe.
- The optimization handoff now references its separately supplied PDF by name,
  allowing the repository hygiene check to pass without weakening the check.

## Verification

Fresh validation on the integrated active checkout passed 211 native Unity
tests, 16 Python tests, and all 264 qualification cases. The native suite adds
fault-injected radio tests,
peer-observed cleanup errors, completed-session replay and eviction tests,
owned snapshot/concurrent publication checks, and JSON escaping coverage.
An independent Python JSON decoder also verifies escaped control bytes,
UTF-8 filenames, and numeric fields from the production serializer.
Six new/strengthened protocol regressions were also run against the original
header and failed as expected before passing with the updated implementation.

Both canonical firmware builds, both SPIFFS images, and the offline board
handoff checks passed. The additional custom-PCB Wi-Fi validation build also
compiled and linked with the HTTP server included. Qualification evidence is
under `.pio/qualification/run-0g43fdix`; the offline handoff reported
`HOST_ARTIFACT_CHECKS_PASS` under `.pio/board_handoff/run-wh6j6pl5`. Logs and
generated reports remain ignored under `.pio/`.

Run the normal checks from this worktree:

```sh
platformio test -e native -v
python -B -m unittest discover -s test -p 'test_*.py'
python tools/run_firmware_qualification.py
platformio run -e rf3_custom_pcb -e rf3_esp32_devboard
platformio run -e rf3_custom_pcb -e rf3_esp32_devboard -t buildfs
python tools/prepare_board_handoff.py
python tools/check_repository_hygiene.py
git diff --check
```

Wi-Fi stays disabled in the tracked environments. Also compile an isolated
Wi-Fi-enabled configuration when changing HTTP integration; local validation
uses an ignored `.pio/phase-one-platformio.ini` that inherits the custom PCB
environment, enables Wi-Fi, and uses a separate build directory and generated
sdkconfig path. Dummy validation-only SSID/password values keep the HTTP code
reachable for the compile/link check; no device is flashed or connected.

Physical RF throughput, ESP32 task scheduling, live HTTP responsiveness, and
hardware fault recovery require board validation. Host tests do not establish
those results.

## Integration boundary

Phase 1 is integrated and software-complete. This does not establish live RF
behavior, power stability, ESP32 scheduling, or HTTP responsiveness on physical
boards; those begin with Phase 2 hardware validation. This work also does not
implement the later filesystem service, browser import/export, or protocol
expansion phases.
