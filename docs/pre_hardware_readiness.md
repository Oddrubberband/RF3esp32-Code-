# RF3 pre-hardware readiness

Date: 2026-08-18

2026-08-30 follow-up: the separate
[firmware qualification](firmware_qualification_results.md) passed all 264 cases.
Use [board_bringup.md](board_bringup.md) for current image preparation and manual
programming steps. The historical results below are retained; they do not mean
the physical PCB/RF gates have been passed.

## Recommendation

**Conditional Go** for controlled bench validation. The software is buildable
from pinned inputs, all host tests pass, and no known Critical or High software
defect remains. The condition is intentional: neither an ESP32 nor an nRF24 was
available during this audit, so power integrity, SPI electrical behavior, radio
identity, over-the-air interoperability, and real task stack margins are not yet
proven.

Use `rf3_custom_pcb` on the ESP32-WROOM-32UE-N16 PCB. The
`rf3_esp32_devboard` profile has a deliberately different CE/IRQ pinout and a
4 MB partition table.

## Reproducible toolchain

The repository selects this stack explicitly:

| Component | Pinned/resolved version |
| --- | --- |
| PlatformIO Core | 6.1.19 (`requirements.txt`) |
| Espressif32 platform | 7.0.1 |
| ESP-IDF framework package | 4.60001.0 (ESP-IDF 6.0.1) |
| Xtensa compiler | `toolchain-xtensa-esp-elf` 15.2.0+20251204 (GCC 15.2.0) |
| ESP32 ULP toolchain | 1.23800.240113 |
| CMake | 3.30.2 |
| Ninja | 1.9.0 |
| SCons | 4.40801.0 |
| ESP-IDF helper | 1.0.1 |
| menuconfig tool | 1.4060000.20190628 |
| ESP ROM ELFs | 0.0.1+20241011 |
| esptool package | 2.41100.0 (esptool 4.11.0) |
| mkspiffs | 2.230.0 (2.30) |
| Xtensa GDB | 11.2.0+20230208 |
| RISC-V GDB (platform dependency) | 11.2.0+20220823 |
| Native platform | 1.2.1 |
| Unity | 2.6.1 |

The validation machine used Python 3.12.13 on Windows. Python itself is not a
firmware input; PlatformIO's tracked requirement and platform packages are.
A separately installed ESP-IDF extension, including an ESP-IDF 5.5.3 install,
is not read by these PlatformIO environments.

ESP-IDF 6.0.1 was retained because Espressif32 7.0.1 is the stable PlatformIO
release already compatible with the current source and repaired sdkconfig
defaults. Inspection found no code requirement for 5.5.3 and no justification
for another framework migration. Pinning the complete resolved package set
removes the previous transitive-version drift while preserving application
behavior.

## Build results

Every tracked firmware environment was built after the changes. The two named
profiles are canonical; the other three are compatibility aliases and were
also built rather than assumed equivalent.

| Environment | Profile | Result |
| --- | --- | --- |
| `rf3_custom_pcb` | 16 MB custom PCB | PASS; RAM 14,188/327,680 B, app 256,665/1,572,864 B |
| `rf3_esp32_devboard` | 4 MB devboard | PASS; RAM 14,320/327,680 B, app 256,765/1,572,864 B |
| `esp32wroom32d` | custom-PCB alias | PASS |
| `esp32wroom32d_manual_boot` | custom-PCB alias | PASS |
| `esp32wroom32d_devboard` | devboard alias | PASS |

Both canonical SPIFFS images also build. The custom partition is 0xE70000
bytes; the devboard partition is 0x270000 bytes.

## Automated validation

Final host validation:

| Check | Result |
| --- | --- |
| PlatformIO native Unity suite | 189/189 PASS |
| Python staging and handoff tests | 16/16 PASS |
| Repository hygiene check | PASS |
| `git diff --check` | PASS |
| Strict host warning pass | PASS for RF3 production/test sources; third-party Unity emits its own conversion warnings |

The native suite exercises realistic packet construction and parsing,
fixed-width padding, binary and boundary files, sequence rules, duplicate/gap
handling, CRC32, sender and receiver transitions, retry exhaustion,
cancellation, timeouts, maximum-size streaming, storage failures, nRF24
register behavior through a fake HAL, FIFO backlog, CW/Morse helpers, hardware
profiles, command numeric parsing, and the RF3 transfer API. The Python tests
exercise transactional file staging, SPIFFS capacity behavior, reserved partial
files, filename rules, and the Protocol v2 transfer-size boundary.

Host tests cannot establish supply quality, signal voltage/timing, a real
nRF24 register response, genuine/clone radio behavior, packet error rate,
radiated output, antenna performance, RF coexistence, or stack high-water marks
under live ESP-IDF scheduling.

## Audited firmware configuration

- Custom PCB pins: CE 17, CSN 5, IRQ 27, SCK 18, MOSI 23, MISO 19.
- Devboard pins: CE 27, CSN 5, IRQ 26, SCK 18, MOSI 23, MISO 19.
- SPI3, mode 0, 1 MHz.
- nRF24 channel 76, five-byte address `52 46 33 24 01`, 250 kbit/s,
  power level 3, fixed 32-byte payloads, two-byte nRF hardware CRC.
- nRF hardware auto-ACK and auto-retransmit are disabled. Protocol v2 supplies
  START/READY, stop-and-wait DATA/ACK, END/COMPLETE, CRC32, and idempotent retry
  handling at the application layer.
- Protocol v2 DATA payload is 20 bytes. Maximum transfer size is 1,310,720
  bytes (65,536 DATA packets). Control timeout is 500 ms, DATA ACK timeout is
  250 ms, maximum retry count is 5, and session inactivity timeout is 10 s.
- SPIFFS never auto-formats after a mount failure. Incomplete `.part` files are
  hidden/cleaned, and a final file is published only after size and CRC32 verify.
- Wi-Fi/HTTP and RF remote-command control remain disabled in every tracked
  environment.

## Defects and risks found

No Critical or High confirmed software defect was found.

| Severity | Classification | Location | Finding and realistic failure mode | Disposition |
| --- | --- | --- | --- | --- |
| Medium | Confirmed defect | `include/command_parser.hpp`; formerly local parsing in `src/main.cpp` | `strtoul` could accept a signed token such as `-1` as a valid 32-bit unsigned value on targets where `unsigned long` is 32-bit. A loop/timing command could run for an unexpectedly enormous duration. | Fixed with sign rejection, `strtoull`, `errno`, full-token, range, and `uint32_t` checks; host-tested. |
| Medium | Confirmed defect | `tools/stage_demo_file.py::main` | A file larger than Protocol v2's 1,310,720-byte maximum could fit the 16 MB SPIFFS partition and be staged, then fail only at TX. That could look like a link problem at the bench. | Fixed with a staging-time protocol-size preflight and boundary tests. |
| Medium | Confirmed defect | `src/main.cpp::commandCw` | `CW LOOP` read radio status without the radio mutex while the RX/loop tasks can mutate the radio state. It could select a stale/inconsistent channel. | Fixed by taking the existing radio mutex around the status read. |
| Low | Confirmed defect | `tools/stage_demo_file.py::sanitize_output_name` | A staged name could exceed ESP-IDF's generated 31-byte SPIFFS object-name limit and fail later during image creation. | Fixed with ASCII byte bounding while preserving a valid suffix; host-tested. |
| Low | Confirmed defect | `src/main.cpp::DemoConsoleApp::run` | Input beyond 159 characters was silently truncated and the prefix executed. An operator could believe a different command was submitted. | Fixed: the entire overlong line is rejected explicitly and never dispatched. |
| Low | Confirmed defect | `src/radio_manager.cpp::enterRx` | `rx_packets` reset on each internal return to RX, including every protocol response. STATUS therefore under-reported received payloads despite documenting a boot/probe lifetime. | Fixed by resetting only at boot/probe; a re-entry regression test was added. |
| Low | Confirmed defect | `data/README.md` | The active data documentation still described the retired v1 4-byte/28-byte packet layout and old receive filename width. | Updated to Protocol v2's 12-byte/20-byte DATA layout, maximum size, and eight-hex-digit receive names. |
| Low | Strongly supported risk | `platformio.ini` and CI | Framework/tool versions below the platform layer were transitive, so a clean host could resolve a different compiler or image tool. | Fixed by pinning the resolved package set and tracking PlatformIO Core. |
| Medium | Plausible concern requiring validation | ESP-IDF tasks in `src/main.cpp` | Main task is configured for 3,584 bytes and radio RX/loop tasks use 4,096 bytes. Current code fits and builds, but live stack margin is not measured. | Do not change speculatively. Record stack high-water marks during extended bench runs if resets/watchdogs occur. |
| Medium | Plausible concern requiring validation | PCB power and GPIO5 CSN | PA+LNA modules have burst-current sensitivity; GPIO5 is also an ESP32 strapping pin. A radio module, PCB fault, or poor decoupling could disturb boot or RF operation. | Hardware inspection and rail/boot tests are first in the bench procedure. |
| Medium | Plausible concern requiring validation | `Nrf24::startContinuousCarrier` and real modules | Genuine and clone radios differ in continuous-carrier support; the driver includes a fallback but only an RF instrument can prove output. | Validate only after the packet link passes. |

## Confirmed working in software

- Both pin/flash profiles and all aliases compile against one pinned framework.
- Compile-time assertions prevent mixing the custom and devboard pin profiles.
- Protocol v2 framing, bounds, CRC, sequence and retry state machines pass host
  fault-injection tests, including maximum-size incremental transfer.
- SPIFFS staging rejects known untransmittable input before build/upload.
- The current `speech_test.u8` is 360,000 bytes: 18,000 full DATA packets.
- The current `song.u8` is 1,112,701 bytes: 55,635 full DATA packets plus one
  final one-byte DATA packet. Together they give useful exact-full and partial
  final-packet bench fixtures.
- Error paths keep the console alive after radio or SPIFFS initialization
  failure, which makes physical diagnosis possible.

These statements do not claim that physical RF transfer works; they describe
what builds, static inspection, and host simulation establish.

## Remaining software uncertainty

- ESP-IDF task stack headroom and long-run heap behavior need live telemetry.
- Real interrupt latency and the 2 ms RX polling cadence need observation under
  sustained traffic.
- A real module may expose timing/clone quirks not modeled by the fake HAL.
- Transfer confidentiality/authentication is not provided. This is acceptable
  for the requested isolated bench test; Wi-Fi and remote RF commands are off.
- The stop-and-wait protocol favors correctness and diagnostics over throughput;
  full audio fixtures may take substantial time at 250 kbit/s.

## Hardware-dependent unknowns

- PCB assembly correctness, pin continuity, radio orientation, and common ground.
- 3.3 V rail droop/noise during PA transmit bursts and adequate local bypassing.
- GPIO5 boot-strap level with the actual CSN circuit/module attached.
- SPI waveforms and register readback from each physical nRF24.
- Whether both modules are genuine/compatible at the configured 250 kbit/s.
- IRQ electrical behavior, antenna/connector health, radiated power, packet
  error rate, useful range, interference, and regulatory constraints.
- Whether the `.u8` assets represent unsigned 8-bit, 8 kHz, mono audio. RF3
  intentionally transfers them as raw bytes and carries no audio metadata.

The ordered procedure and minimum path are in
[`hardware_validation.md`](hardware_validation.md).
