# Current implementation verification

Implementation: R2, developed 2026-10-22–23. Promotion date: 2026-10-24.

This record separates the tested R2 baseline from promotion-only packaging changes. [CHANGELOG](../../CHANGELOG.md) records the accepted changes, including every audit finding. The user operates hardware flashing and physical acceptance.

## Verification status

| Scope | Status |
|---|---|
| Tested R2 GUI/backend/firmware implementation | Software suites, builds, integrity checks and independent review passed as recorded below. |
| User-operated file comparison | One 411,014-byte PDF round trip has exact byte identity and matching SHA256. |
| Canonical GUI branding/name | Local candidate checks passed; see Promotion checks below. |
| Portable instructions, paths and launch/build/flash workflow | Local candidate checks passed; see Promotion checks below. |
| Checked-in legacy compatibility fixture | Both-profile metadata/legacy candidate checks passed. |
| Pinned dependencies, CI and release tooling | Local tooling tests passed; remote CI/build result is separate. |
| Final public GUI/distribution hashes | Canonical local EXE hash recorded below; remote asset hashes are recorded by CI. |

Historical R2 checks and final local promotion checks are listed separately. Firmware behavior remains unchanged during promotion. Remote CI is not claimed successful in advance.

## R2 software checks actually performed

| Check | Observed result | Scope |
|---|---|---|
| Full GUI suite | 208/208 PASS, 7.587 s | Fake ports and offscreen Qt. |
| Independent full GUI repeat | 208/208 PASS, 9.322 s | Final R2 source, separate review. |
| Existing native radio/protocol suite | 177/177 PASS, 4.087 s | Existing suite retained from baseline; not 177 newly added cases. |
| Frozen R2 product/config/test files | 98/98 hashes match | Final pre-promotion implementation. |
| Custom firmware build | PASS, 203.179 s | Custom 16 MB profile. |
| Devboard firmware build | PASS, 204.734 s | Devboard 4 MB profile. |
| Application/bootloader integrity | All four checksum/hash checks VALID | Six selected images hash-matched, including unchanged partition images. |
| Packaged GUI | PyInstaller build PASS; 5 s mock startup PASS | Own test process deliberately terminated after the smoke interval. |
| UI layout | Mock views inspected at 1080×720 and 1150×900 | Incoming state/bytes/bar/rate/elapsed and compact status presentation. |
| Preservation/rollback | Prior 365 R1, 327 S1 and 342 P1 recorded files unchanged | Local archives passed CRC/all-file hashes; restore exercised into a new folder; selected image hashes/invalid-port refusal checked without opening a port. |
| Independent review | No remaining software release blocker identified | Not a claim of physical qualification or absence of every possible defect. |

## Fix-to-check coverage

| Area | Regression/verification evidence |
|---|---|
| H2 handshake and H3 Windows SPP | Immutable identity independent of RF ownership; both actual C++ profile replies through fragmented parsing; separate setup/handshake deadlines; bounded writes; exact selected-address outgoing-port rules and error87 cases. |
| F01 transfer telemetry | Production-method status capture while radio ownership is held; unique enqueue IDs through early failures; actual USB human+JSON sequences through the GUI backend; true peer completion/size/error checks. |
| F02/F03 polling | Valid JSON without ACK retires polling; ACK-only/malformed streams cannot move the original usable-telemetry deadline. |
| F04/F05 controls | Actual offscreen manual/periodic refresh preserves dirty channel/power/CW values; unknown power, readiness and fault presentation tested. |
| F06/F07/F08 file sources | Stored Select/TX with no upload; literal reserved names; empty selection clears; textual `false`/`0`, spaces and UTF8 remain filenames. |
| F09/F10/F12 command/ownership | Typed acknowledgement correlation and deadlines; late status cannot clear storage/host Busy; unscoped queue errors are surfaced; backend conflict guards exercised. |
| F11/F14 connection teardown | Save-dialog/disconnect race, generation rejection, temporary cleanup and callback reentry cannot create orphan operations or retain current-looking stale state. |
| F13 test mode | Explicit devboard capability and pending transition; matching telemetry resolves lost ACK; verification-purpose latches prevent accidental RF. |
| F15 multiple instances | Independent processes exercise their own rotating logs; no common-file rollover collision; repeated logger setup does not duplicate handlers. |
| S1 import/export/delete | CRC/size, full Import/test-mode byte readback, atomic PC publication, unique temporary files, protected names, pagination, mounted/unmounted capability and activity/recovery guards. |
| P1 host transport | 64/256-byte sessions, correct READY agreement, legacy 64-byte commands, session latching, lost/duplicate chunk replies, tail lengths, CRC failures and bounded framing. Sizes include 0,1,255,256,257,513,1041 and1,048,576 bytes. |
| R1 firmware concurrency/framing | 49,999-write coherent report publication stress with native adapters; conservative status fixture1470 bytes within1534+CRLF/1536-byte SPP message budget; loop-only CW OFF Busy isolation. |
| R2 repeated Receive | Real ReceiverSession receives encoded START plus20 bytes; production RX repeated twice retains ID/open sink/counters, then finishes exact43-byte content and CRC. Prior reset hazard was also reproduced in the older implementation. |
| R2 disruptive-command race | Guarded destructive commands reject before handler side effects; receive reservation acquired/tested under radio lock; host timeout/disconnect/epoch and queued-worker ownership release checked. |
| R2 cancellation | Actual STOP retains Cancelled ID/bytes/error, works with coarse radio Fault and reports cleanup failure. GUI cancellation has an independent15 s deadline; malformed/unrelated/partial telemetry cannot retire or postpone it. |
| R2 incoming progress | Four active RX phases, monotonic updates, duplicate/late/Idle/terminal validation and separate outgoing/host progress; actual native receiver status fed through backend for both profiles. |
| Observed rate | Monotonic payload-byte delta math, approximately10 s available-sample window, first-sample exclusion, stall behavior, reconnect/new-ID reset and terminal freezing. |
| G5 file type | Real Service→codec→ReceiverSession→production storage methods; final suffix/case and10-byte boundary, zero-length file, legacy exact bytes/original decoder, lost READY/retry, type-changing replay, malformed metadata, collisions at prepare/publish and corrupt-CRC cleanup. |
| G5 preflight scope | Invalid RF suffix fails before source read/session/queue; host-only longer safe suffix upload plus CRC/full readback/no RF; unpreservable SPIFFS normalization fails before IO. |

Native production-method tests replace task/radio adapters while executing real protocol and storage logic with controlled temporary files. Synthetic `.pdf`-labelled payloads prove naming/byte preservation, not PDF rendering. They are distinct from the later real PDF file comparison.

## Physical acceptance record

The user reported a successful original/received PDF round trip. Both files are **411,014 bytes**, exactly byte-equal and have matching SHA256. Local comparison was independently rechecked on 2026-10-24 without publishing either file, its private basename or its actual document digest.

The agent did not operate the boards. Direction, individual board identities and exact flashed-image versions were not independently verified for that comparison. It supports exact preservation of that one file, including its bytes/metadata; it does not establish general RF range, speed or reliability.

The previous logs showed receiver progress resetting before sender retry exhaustion. The repeated-Receive code hazard was reproduced and repaired, but the exact user click was not logged, so that specific physical cause is not asserted.

## Stable implementation identity and board contract

The tested firmware source `src/main.cpp` SHA256 is:

`3c7de48fed6c84ba79991a3d1945a99440c87e36f36720639366aee2e63f9c11`

The tested application image hashes are:

| Target | Application SHA256 |
|---|---|
| `rf3_custom_pcb` | `e966e9630fb07f22e5d594ab176f97f8b71f2401854398f4185e55a8156b274e` |
| `rf3_esp32_devboard` | `72122db8d256766116778a85c4762063fcfcc4fc573a956e3e4b2401e3e0090a` |

These are ordinary public code/image fingerprints. The document digest is intentionally omitted. Final image manifests remain authoritative for helper selection; promotion rebuilds must record any new GUI/distribution hashes separately.

| Contract | Custom PCB | Devboard |
|---|---:|---:|
| Flash | 16 MB | 4 MB |
| CE | GPIO17 | GPIO27 |
| CSN | GPIO27 | GPIO5 |
| IRQ | GPIO16 | GPIO26 |
| SCK / MOSI / MISO | GPIO18 / GPIO23 / GPIO19 | GPIO18 / GPIO23 / GPIO19 |

Manual BOOT/EN and `--before no_reset --after no_reset` remain required. Use the USB programming port and explicit board selection. The verified procedure uses standard stub/compression,115200,DIO40m and ESP-IDF offsets0x1000/0x8000/0x10000; normal flashing does not upload SPIFFS or perform a full erase. See the current README for portable commands.

## Compatibility and limits

- Both updated firmware images are required for extension-bearing START. The new decoder accepts legacy flag0, while the old decoder rejects flag1.
- RF metadata supports only the final safe ASCII suffix, up to10 bytes including its dot, using letters/digits/underscore/hyphen and excluding reserved `.part`. Case is preserved; `.tar.gz` carries `.gz`; extensionless/legacy files use `.bin`. Host-only storage can retain longer safe suffixes within SPIFFS naming limits. No full-name/MIME detection or old-file migration is implied.
- Frames remain32 bytes, DATA capacity20 bytes, max file1,310,720 bytes. Content CRC remains distinct from filename metadata.
- The GUI rate is observed payload KiB/s. Elapsed starts at first observation; neither is a physical air-bitrate benchmark. Chunk-count reduction is not proof of a proportional measured speedup.
- The ordered UART/SPP reply assumption remains. Arbitrarily reordered older terminals for the same filename with a different random ID cannot be uniquely resolved without request-to-ID acknowledgement; no causal reordering path was established.
- Real driver behavior during failures, RF range/sustained reliability/throughput, hardware timing/stack high-water and long-term SPIFFS behavior remain unqualified.
- Local rollback saves sources/GUI/build images, not physical flash/NVS/SPIFFS or later board-file mutations.
- No circuit, component, antenna, PCB or simulation qualification is part of this software record.

## Promotion checks performed on 2026-10-24

- Canonical title/name: 208 GUI tests passed in 4.806 seconds. The packaged RF3Desktop.exe remained running for five seconds with the mock backend and was then deliberately terminated; no hardware was opened.
- Portable production harnesses: actual runtime/status, receive guards and metadata/legacy fixture checks passed for both profiles without a sibling checkout dependency.
- Release tooling: 27 tests OK with one Windows symlink-permission skip; five strict C++ fixtures passed. Tests cover explicit source identity, archive/member CRC and SHA-256, no overwrite, default dry-run and mocked draft publication. No real publication is inferred from mocked API tests.
- Manual helper parsed successfully, checked the current images, and refused invalid COM and an omitted explicit PreviousRoot before serial access. Both no_reset options, manual BOOT/EN, board profiles and partitions remain intact.
- Six local firmware images rehashed and copied unchanged from the verified R2 baseline. Firmware logic is unchanged; two inherited trailing-whitespace lines were cleaned in a driver declaration/native test. Backend/protocol sources match the tested implementation.
- Source hygiene and staged whitespace checks passed. Independent source/embedded Python privacy checks excluded private documents, fingerprints, personal paths and connection logs. A Qt vendor build path is an upstream dependency constant, not user data.
- Canonical local EXE SHA-256: `59e5e466cc82a93b7759b0faa25290e2f96ccadb0fe48d4605b8073e123edbc2`.

GitHub validation rebuilds from the exact published source commit and attaches its own source/asset checksum manifests. A remote build is only successful when its workflow reports success; the local checks above do not pre-claim that result. Independently built Windows binaries can differ from this local EXE.
