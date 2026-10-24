# Changelog

## Current R2 — promotion dated 2026-10-24

This release promotes the tested R2 implementation developed on 2026-10-22–23. It includes the connection, storage, performance and GUI repairs below, followed by receive protection, incoming progress/rate and file-type preservation. The selected source lineage starts at upstream `main7b14485`; earlier standalone packet-power experiments are not the current firmware.

This note covers 71 accepted material changes in 12 groups and maps the 15 audit findings to their corrections. Verification details and limits are in [current verification](docs/verification/current_verification.md).

### Promotion and distribution changes

- Use the canonical **RF3 Desktop** GUI/application name instead of the intermediate test-build title.
- Replace machine-specific paths in current instructions and launch/build/flash workflows with portable project-relative discovery.
- Include a portable legacy protocol fixture for compatibility tests rather than depending on a sibling historical checkout.
- Pin the supported dependency/build environment and provide reproducible source, GUI and firmware release tooling with CI checks.
- Keep firmware behavior, board profiles and partition layouts unchanged during promotion; remove two inherited trailing-whitespace lines in a driver declaration and native test only. Record final GUI/distribution hashes after any branding or packaging rebuild.

**Promotion verification:** the canonical candidate passed all 208 GUI tests (4.806 s), packaged EXE five-second mock startup, all three production-method harnesses for both profiles using the bundled legacy fixture, and 27 tooling tests (one Windows symlink-permission skip). Five strict C++ helper fixtures passed. Manual helper syntax, image hash selection and invalid-port/absent-rollback-root refusal passed without opening a device. Source hygiene and staged whitespace checks passed. The six local firmware images were rehashed against the verified R2 baseline and copied unchanged. The canonical local EXE SHA-256 is `59e5e466cc82a93b7759b0faa25290e2f96ccadb0fe48d4605b8073e123edbc2`. Independent public-source and compressed EXE privacy checks excluded personal material. GitHub CI independently rebuilds from the published commit; its artifacts have their own recorded hashes. Local checks do not claim that a future remote CI run has already passed.

### Changes and fixes

#### B0 — Selected source and unchanged hardware contract

- Adopt the corrected source archive matching upstream main7b14485; retain the wrong initial archive and older standalone project as superseded history.
- Retain compile-time-checked custom CE17/CSN27/IRQ16 with 16 MB flash and devboard CE27/CSN5/IRQ26 with 4 MB flash; both SPI SCK18/MOSI23/MISO19. Existing partitions remain unchanged.
- Retain baseline core radio/ProtocolV2 reliable sender/receiver, CRC, audio/text/Morse/CW features. These inherited functions are not all new work in this update.
- Enable Classic Bluetooth SPP on custom PCB, alongside the already-enabled devboard: RF3-rf3-pcb / RF3-rf3-devboard, RF3-SPP service, pairing required. No BLE transport added.

Implementation: `platformio.ini`, `sdkconfig.bluetooth.defaults`, `include/hardware_profile.hpp`, `src/main.cpp`. Verification: B0, BT/FLASH, R2.

Scope: Existing assembled-PCB firmware pins are separate from proposed redesign wiring; no circuit/component/PCB change is included.

#### FLASH — Manual-only programming and recoverability

- Set before/after reset to no_reset on both profiles; retain monitor DTR/RTS0.
- Use explicit board and USB-port selection and verify selected image hashes before upload. Operator holds BOOT, taps EN at Connecting, releases BOOT when Writing begins and presses EN manually after verification.
- Retain the proven standard RAM stub/compressed write_flash -z,115200 baud,DIO40m,flash-size detect and ESP-IDF offsets0x1000/0x8000/0x10000. Do not add Arduino boot_app0 at 0xe000.
- Normal helper writes firmware images, not a SPIFFS image/full erase.
- Provide board wrappers, verified image manifests, Previous-image selection and restore into a new absent directory with archive/extracted hash validation.

Implementation: `platformio.ini`, `Flash_Manual.ps1`, `Flash_Custom_PCB.cmd`, `Flash_Devboard.cmd`, `Firmware_Images.json`. Local historical rollback helpers (not public release files): `Restore_Previous_Copy.ps1`, `Launch_Previous_GUI.cmd`. Public rollback uses a separately extracted previous release and explicit `-PreviousRoot`. Verification: BT/FLASH, R1, R2.

Scope: Local rollback saves sources/GUI/build images, not physical flash/NVS/SPIFFS or later board-file mutations. Publication portability/naming changes remain coordinator-owned.

#### H2 — Host identity independent of radio health

- Add immutable HELLO node/profile/host-protocol/RF-protocol identity without radio/filesystem/command locks.
- Validate host identity separately from radio readiness; retain legacy STATUS fallback and gate RF controls until usable telemetry.
- Separate Bluetooth120 s/USB30 s opening setup from 25 s handshake after port opening; expose setup stages.
- Remove unbounded Windows write flush; retain bounded writes/length checks and one outstanding STATUS.
- Clear previous-peer capabilities/readings on transitions; label unavailable/last-reported state honestly.
- Add bounded rotating connection diagnostics, excluding file payloads and live-message text from persisted debug records.

Implementation: `include/host_connection_protocol.hpp`, `src/main.cpp`, `host_gui/rf3_gui/device/serial_device.py`, `host_gui/rf3_gui/app/state_model.py`, `host_gui/rf3_gui/ui/main_window.py`, `host_gui/rf3_gui/logging_config.py`. Verification: H2, R1, R2.

Scope: HELLO success does not establish nRF24 power, wiring or RF delivery.

#### H3 — Windows Classic SPP activation correction

- Reuse exactly one outgoing SPP mapping for the selected device address before redundant service activation.
- Handle error87 through a bounded selected-address mapping wait; reject incoming/other-peer/ambiguous ports.
- Replace the incorrect BLE-only diagnosis with an accurate Windows connection error and persist setup failures.

Implementation: `host_gui/rf3_gui/device/windows_bluetooth.py`, `host_gui/rf3_gui/device/serial_device.py`. Verification: H3, R2.

Scope: The individual Windows API/driver root cause of error87 was not conclusively identified.

#### S1 — SPIFFS browse, import, export and delete

- Add visible-file names/sizes/usage, four-entry structured pagination and selection-preserving refresh.
- Add store-only Import with CRC verification, complete byte-for-byte SPIFFS readback and a SHA256 fingerprint; Import sends no RF.
- Add selected-file Export with declared size/CRC checks, fsync and atomic PC publication.
- Use unique export temporary files; preserve unrelated .rf3.part sidecars and existing destinations on failure.
- Add confirmed Delete with host/radio/loop/receiver/recovery ownership checks; update ESP selection/last-received references after deletion.
- Add UTF8 hex storage/delete/download commands for exact names containing spaces/Unicode, with protected/internal/path/dot-dot/control/invalid-UTF8/.part checks and30-byte limits.
- Tie host storage/upload/download capabilities to mounted SPIFFS; invalidate listings on refresh failure and reject late/cross-session responses.

Implementation: `include/host_storage_protocol.hpp`, `src/main.cpp`, `host_gui/rf3_gui/device/serial_device.py`, `host_gui/rf3_gui/protocol/console_protocol.py`, `host_gui/rf3_gui/ui/main_window.py`, `host_gui/rf3_gui/app/controller.py`, `host_gui/rf3_gui/device/base.py`, `host_gui/rf3_gui/device/mock_device.py`. Verification: S1, P1, R1, R2.

Scope: Exact extra readback applies to Import and devboard verify-only mode. Ordinary RF-send uploads are CRC-verified and RF success requires peer completion; do not claim every ordinary send always adds a full host readback pass.

#### P1 — Buffered host reads and negotiated chunks

- Replace readline-per-byte adapter behavior with bounded buffered reads; preserve fragmented UTF8/line buffering and corrupt-input resynchronization.
- Negotiate64..256-byte host chunks via host_chunk_max and optional BEGIN argument; absent support remains64 and legacy command bytes stay unchanged.
- Latch each session's chunk size, validate READY agreement before payload, and reject invalid/oversized requests.
- Expand host framing for 512 hex characters while keeping hard bounds, duplicate replay idempotence, tail sizing, retries, CRC and atomic publication.
- Configure main stack8192 and Bluetooth command/BTC stacks6144 as appropriate, with stack-canary/SPP settings retained.
- Build separate GUI/both firmware images so running GUI processes need not be closed during preparation.
- Rebuild the Windows EXE from the current PySide6 GUI source with PyInstaller, retaining editable GUI/test sources and matching firmware for both profiles; the local build uses Python3.11 rather than the untouched supplied Python3.12 distribution.

Implementation: `include/host_transfer_protocol.hpp`, `src/main.cpp`, `sdkconfig.defaults`, `host_gui/rf3_gui/device/serial_device.py`, `host_gui/rf3_gui/app/state_model.py`, `host_gui/rf3_gui/protocol/console_protocol.py`. Verification: P1, R1, R2.

Scope: This changes PC/ESP host traffic, not nRF over-air DATA capacity. For 1 MiB, chunks per pass drop16384 to 4096; actual speedup/stack high-water were not measured.

#### R1-STATUS — Complete and truthful transfer telemetry

- Publish full USB/SPP TX/RX ID/file/state/bytes/sequence/retries/error/CRC/peer result; keep consistent quoted human USB output.
- Publish coherent nonallocating progress/radio records under a separate short critical section while the sender owns the hardware lock.
- Allocate nonzero ID at accepted enqueue and retain it through actual sender startup and early file/source/radio/transport failure; clear old completion before worker execution.
- Capture current power/channel safely and include active loop state during CW OFF intervals.
- Send ACK and full status as separate bounded SPP messages; reject oversized status instead of truncating JSON.
- Distinguish command acceptance, host upload, RF sending and peer-confirmed file delivery. Require complete bytes/no error/true peer confirmation; handle valid empty files.
- Correlate file/transfer identity, isolate ordinary duplicate/prior/different-file terminals and impose bounded RF-progress deadlines.

Implementation: `include/host_status_protocol.hpp`, `src/main.cpp`, `host_gui/rf3_gui/device/serial_device.py`, `host_gui/rf3_gui/app/state_model.py`. Verification: R1, R2.

Scope: RX <- OK STATUS is a host acknowledgement, not RF delivery. The documented ordered-response assumption still applies.

#### R1-GUI — Settings drafts, file sources and two-window diagnostics

- Separate dirty channel/power/CW drafts from confirmed telemetry; manual/periodic refresh preserves edits and Apply does not invent success.
- Display Unknown instead of -1, show fault details and correct fault4 to RX payload-read failure; distinguish requested/current/last-reported state.
- Separate PC source, normalized SPIFFS name, ESP selected file and current/last RF source.
- Add stored-row Select/TXHEX with no re-upload; treat LOOP/STOP names literally on matching firmware and fail safely on incompatible legacy paths.
- Advertise/gate devboard-only test mode and pending transitions; preserve verify-only purpose latches against accidental RF.
- Clear stale modes/capabilities/readings on failures; detach generation/port/timers/sessions/pending state before callbacks so reentry cannot create orphan work.
- Use per-process/UUID rotating files and idempotent logger setup, with instance/device identity and log location for two GUI windows.

Implementation: `host_gui/rf3_gui/ui/main_window.py`, `host_gui/rf3_gui/app/controller.py`, `host_gui/rf3_gui/app/state_model.py`, `host_gui/rf3_gui/device/serial_device.py`, `host_gui/rf3_gui/device/mock_device.py`, `host_gui/rf3_gui/logging_config.py`, `host_gui/rf3_gui/protocol/console_protocol.py`, `src/main.cpp`. Verification: R1, R2.

Scope: Truthful power/fault display does not repair every electrical cause of transmit timeout.

#### R1-LIFECYCLE — Polling, typed responses and operation ownership

- Let usable telemetry complete pending STATUS after a lost text ACK; ACK-only/malformed traffic cannot postpone the original telemetry deadline.
- Match replies to the right command; bound ordinary command deadlines and surface unscoped queue/framing errors.
- Retain storage/host/loop Busy across late status; enforce backend conflict guards in addition to disabled widgets.
- Distinguish missing versus empty selected names and keep filename/ID/state fields textual so false/0 are not coerced.
- Recheck live connection after dialogs and before temp/session creation; cancellation/disconnect leave no orphan Busy download or unintended destination replacement.
- Accept matching explicit TESTMODE telemetry after lost ACK and retain safe session purpose/framing latches.

Implementation: `host_gui/rf3_gui/device/serial_device.py`, `host_gui/rf3_gui/protocol/status_parser.py`, `host_gui/rf3_gui/ui/main_window.py`. Verification: R1, R2.

#### R2-GUARD — Idempotent Receive and protected active reception

- Repeated RX in RxListening succeeds under lock without stop/reset/leave or closing a partial sink; counters/ID/bytes/CRC remain.
- Refuse destructive commands before handler side effects while active receive/partial ownership exists; keep safe observation/listing available and explicit STOP/CW STOP cancellation.
- Acquire receive mutation reservation under radio lock and test it under that same lock before packet draining/ticking, closing the idle-check/use race.
- Retain atomic host-session and queued/active-loop ownership across chunks/startup; release on completion/reset/cancel/disconnect/epoch/timeout or loop termination.
- STOP aborts a live receiver even with coarse radio Fault; retain Cancelled ID/bytes/error and report cleanup failure instead of resetting to Idle.
- Enforce active-receive guards in GUI/backend, including direct/stale calls; repeated Receive is a local no-op while already listening.

Implementation: `include/host_rx_command_guard.hpp`, `src/main.cpp`, `host_gui/rf3_gui/device/serial_device.py`, `host_gui/rf3_gui/ui/main_window.py`. Verification: R2.

Scope: The prior reset hazard was reproduced, but a particular earlier Receive click was not logged or proven causal.

#### R2-PROGRESS — Incoming progress, cancellation and observed rate

- Model Receiving/WaitingForEnd/Verifying/Publishing as incoming Busy with separate ID/bytes/total/phase/error, without overwriting outgoing/host progress.
- Retain terminal metadata across omitted/Idle samples; prevent late duplicates from regressing/resurrecting a transfer; start fresh on new IDs.
- Validate raw completion size/error/session identity before monotonic merge/cancel-state changes; incomplete/error Completed is Failed, and unexpected pre-completion Idle is interrupted.
- Show state/ID/bytes/percentage/bar, observed payload KiB/s and elapsed; retain compact status summary at smaller window sizes.
- Use monotonic byte deltas over approximately 10 s of available samples, exclude pre-observation bytes, freeze terminal estimates and reset on reconnect/new ID.
- Make Cancel Receive explicit with its own15 s confirmation deadline; generic ACK/partial or malformed/unrelated telemetry cannot falsely confirm or postpone cancellation.

Implementation: `host_gui/rf3_gui/app/state_model.py`, `host_gui/rf3_gui/app/receive_rate.py`, `host_gui/rf3_gui/device/serial_device.py`, `host_gui/rf3_gui/app/controller.py`, `host_gui/rf3_gui/ui/main_window.py`. Verification: R2.

Scope: Rate is observed file-payload throughput, not air bitrate/overhead utilization or a calibrated benchmark; time begins at first GUI observation.

#### G5 — Received/exported file type preservation

- Carry canonical optional final-suffix metadata in START flag1, bytes22..31, maximum 10 ASCII bytes including dot; preserve case.
- Keep legacy flag0 byte-identical; validate padding/full-field form and reject unknown flags, unsafe/control/path/non-ASCII metadata, trailing garbage and reserved .part.
- Validate RF suffix before source reads and TX/TX LOOP/TXHEX queueing, with explicit rejection instead of truncation/downgrade.
- Copy metadata before receiver prepare, compare extension on duplicate START, and prevent changed-type replays from retargeting active/terminal IDs.
- Publish collision-safe rx_ID.pdf/rx_ID_nnn.pdf, rechecking at publication; retain internal .part staging, byte count and content CRC.
- Preserve supported full suffix through PC-basename normalization and default export dialogs to the received suffix.
- Apply10-byte limit only to RF. Host-only Import/test verification/export retain longer safe suffixes where the 30-byte SPIFFS name can be formed, with pre-IO failure on impossible/unsafe normalization.

Implementation: `include/file_type_metadata.hpp`, `include/protocol_v2.hpp`, `include/reliable_transfer_v2.hpp`, `include/file_transfer_service.hpp`, `src/main.cpp`, `host_gui/rf3_gui/protocol/console_protocol.py`, `host_gui/rf3_gui/device/serial_device.py`, `host_gui/rf3_gui/app/controller.py`, `host_gui/rf3_gui/ui/main_window.py`. Verification: R2.

Scope: Both matching firmware images required: new receivers accept legacy, old receivers reject flag1. Final suffix only (.tar.gz -> .gz); extensionless/legacy use .bin. No full basename/MIME detection or migration of old .bin files. Metadata is separate from file-content CRC.

### Audit closure

The following findings were reproduced before R1 and repaired there; R2 retains those repairs.

| Finding | Previous behavior | Correction |
|---|---|---|
| F01 | Bluetooth omitted RF progress/result | Complete structured USB/SPP status with separate live transfer publication. |
| F02 | Lost STATUS ACK froze polling | Usable telemetry retires the STATUS request independently of text ACK. |
| F03 | ACK-only polls postponed watchdog indefinitely | Bound usable-telemetry deadline instead of resetting on each ACK. |
| F04 | Status refresh overwrote settings drafts | Separate dirty editable values from confirmed values. |
| F05 | Unknown power displayed as -1 | Normalize/display Unknown, expose fault details and readiness. |
| F06 | No existing-SPIFFS Select/TX workflow | Add exact-name select/transmit with no re-upload. |
| F07 | Empty selected name retained stale name | Distinguish omitted from explicitly empty fields. |
| F08 | Filename false/0 coerced to bool/int | Schema-aware textual fields. |
| F09 | USB reply retired wrong pending command | Typed command correlation and deadlines. |
| F10 | Late status cleared storage Busy | All-operation Busy derivation and backend conflict guards. |
| F11 | Save dialog outlived disconnect | Recheck link before temp/session allocation and modal return. |
| F12 | Unscoped queue errors swallowed | Fail ambiguous pending work and surface/resynchronize error. |
| F13 | Custom PCB offered devboard-only test mode | Advertise/model/gate capability and pending transitions. |
| F14 | Read/write failure retained stale mode/state | Complete teardown/reset before callbacks and honest stale labels. |
| F15 | Two processes collided on log rotation | Per-process/UUID logs and idempotent setup. |

Additional review corrections covered inconsistent completion metadata, early-failure transfer IDs, human/JSON field agreement, current power/channel, loop reporting, reserved filenames, TESTMODE confirmation after lost ACK and callback-safe teardown. R2 additionally validates raw cancellation/completion metadata before monotonic merging and separates host-only filename limits from RF metadata limits.

### Verification progression

These are retained results for the corresponding implementation stage. The 177-case native suite was already present at the selected baseline; it was retained and rerun, not newly created wholesale by this release.

| Stage | GUI tests | Other checks |
|---|---:|---|
| Selected baseline | 77 | 177 native; 6 Python staging; 8 host transport checks; matching-source build and supplied EXE mock startup. |
| H2 identity/connection | 87 | 4 strict HELLO and 8 transport checks; both-profile native replies through the GUI parser; both firmware builds and image checks. |
| H3 SPP activation | 91 | Selected-address mapping/error87 tests and rebuilt GUI startup; firmware unchanged. |
| S1 SPIFFS manager | 104 | Strict filename/UTF8/pagination/guard/capability and native-to-GUI checks; both firmware builds/images. |
| P1 host transport | 114 | 64/256-byte import/readback through 1 MiB; chunk/CRC/retry/framing tests; both builds/images. |
| R1 audited repairs | 157 | Independent 157-test repeat; 177 native; production-method contracts; coherent publication and status-size checks; both firmware builds/images and rollback tests. |
| R2 receive/type update | 208 | Independent 208-test repeat; 177 native; real receiver/codec/production-file-sink native checks; both builds/images, EXE mock startup, UI layouts and rollback tests. |

The final R2 GUI suite passed in 7.587 s and the independent repeat in 9.322 s. The native suite passed 177/177 in 4.087 s. Final custom/devboard firmware builds passed in 203.179 s and 204.734 s, with valid application/bootloader checksums and hashes. These are build/test timings, not transfer performance.

### User-operated round trip

A user-operated original/received PDF round trip produced **411,014 bytes**, exact byte identity and matching SHA256. Local file equality was independently rechecked on 2026-10-24. The actual document digest and private filenames are not published.

This establishes exact preservation of that file, including its bytes and metadata. The agent compared local files only; direction, individual board identities and exact flashed-image versions were not independently established. One successful file is not a range, throughput or sustained reliability qualification.

### Compatibility and operating limits

- Flash the matching firmware on **both boards** for extension-bearing transfers. New receivers accept exact legacy flag-zero START frames; old receivers reject the new extension flag.
- Only the final safe ASCII suffix is carried over RF, up to 10 bytes including the dot. Case is preserved, `.tar.gz` carries `.gz`, and extensionless/legacy files use `.bin`. Full original filenames, MIME detection and migration of previously received `.bin` files are outside this feature.
- File-content CRC remains separate from filename metadata. Frames remain 32 bytes, DATA capacity 20 bytes, and the file-size limit 1,310,720 bytes.
- Import and verify-only modes perform complete byte readback; ordinary RF-send uploads retain CRC verification and peer-confirmed RF completion. Avoid describing all ordinary sends as always performing the additional host readback pass.
- Displayed KiB/s is an observed payload estimate, not the configured RF bitrate. The host chunk optimization does not establish a measured fourfold physical speedup.
- The ordered UART/SPP reply assumption remains: arbitrary reordering of an older same-filename terminal with a different random ID cannot be uniquely resolved without request-to-ID acknowledgement. No causal reordering path was established in the current ordered flow.
- Real driver failure behavior, RF range/sustained reliability/throughput, hardware timing/stack high-water and long-term SPIFFS behavior remain unqualified.
- Local rollback archives save project sources/GUI/build images, not physical flash/NVS/SPIFFS, and cannot undo later board-file mutations.
- No component, PCB, antenna or circuit-simulation changes are part of this software release.

### Superseded work

The wrong initial source archive, earlier standalone TX TEST binaries and inappropriate temporary pin mapping are not the current implementation. A successful ROM-only `flash_id` read did not prove ROM-only firmware writing; the final helper uses the proven standard stub/compressed manual procedure. The initial error87/BLE-only diagnosis and the intermediate RF-length restriction on host-only imports were corrected. Earlier passing test suites and failed/intermediate fixtures are not substituted for the final results above.
