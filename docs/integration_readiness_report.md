# RF3 Integration Readiness Report

Date: 2026-08-03
Branch: `codex/integration-readiness`
Base merge: `b067afa4f01c29c2e556f20ff28c9277141019e5`

## Outcome

The RF3 firmware is ready for subsystem-level integration testing. The file-transfer path is generic and binary-safe, both supported ESP32/nRF24L01+ hardware profiles build reproducibly, HTTP control is disabled by default, repository credentials and machine-generated artifacts have been removed from the working tree, and automated validation covers native behavior, both firmware targets, SPIFFS images, file staging, diff hygiene, secrets, and generated-file policy.

This report records software validation only. It does not claim over-the-air interoperability or electrical validation on physical custom-PCB and development-board hardware.

## Delivered integration surface

The hardware-independent `FileTransfer::Service` exposes:

- `startTransfer(...)` for streaming and bounded-memory sources;
- `cancelTransfer()` for the active transfer;
- `getTransferStatus()` for role, state, progress, retries, CRC, completion, error, path, and timestamps;
- `registerReceiveHandler(...)` for verified, published receive completions.

Transfer metadata carries a bounded filename, media type, expected length, collision policy, and caller context without dynamic allocation. Receive callbacks run synchronously in the caller that processes the verified protocol event; the ESP32 integration currently invokes them from the radio receive task. Callbacks therefore must remain non-blocking or hand work to another task.

Sources remain streamed with fixed bounded storage. SPIFFS-backed, generated callback-backed, and bounded-memory inputs are supported. Audio `.u8` files remain valid binary inputs, but the transport does not assume audio semantics or impose audio pacing. The optional generic byte-rate limit defaults to disabled.

## Hardware profiles

| Profile | PlatformIO environment | CE | CSN | IRQ | SCK | MOSI | MISO |
|---|---|---:|---:|---:|---:|---:|---:|
| Custom ESP32-WROOM-32UE-N16 PCB | `rf3_custom_pcb` | 17 | 5 | 27 | 18 | 23 | 19 |
| ESP32 development board | `rf3_esp32_devboard` | 27 | 5 | 26 | 18 | 23 | 19 |

Each environment selects exactly one compile-time profile. Static assertions reject incomplete, unsupported, or mismatched pin definitions. The Espressif PlatformIO platform is pinned to `platformio/espressif32@7.0.1`. The development-board SDK defaults were repaired so `kconfgen` and the complete build succeed.

## Security and repository hygiene

- Removed the real Wi-Fi SSID and password from the working tree. Because historical commits remain unchanged, the credentials must still be rotated.
- HTTP control is disabled by default for both canonical environments.
- Local Wi-Fi values may be supplied through the ignored `include/wifi_control_config.local.hpp`, using the tracked example as a template.
- Removed four tracked generated SDK configuration files, the machine-specific installation log, and generated SCANOSS output, exactly as explicitly approved.
- Removed editor-specific COM-port and ESP-IDF setup values.
- Generated SDK configs, PlatformIO output, local credentials, partial transfer files, and local reports are ignored as appropriate.
- The console remains available when filesystem or radio initialization fails, and status output reports the selected profile, protocol version, filesystem state, radio state, and HTTP-control state.

## SPIFFS staging

The active staging script accepts generic binary files and preserves their sanitized filename rather than forcing an audio extension. Capacity checks count replacements once, exclude receiver `.part` files, and validate the complete proposed directory before replacing staged content. The staging tests exercise replacement, near-full, over-capacity, partial-file exclusion, binary filenames, and replace-directory behavior.

## Validation environment

- PlatformIO Core: 6.1.19
- Espressif PlatformIO platform: 7.0.1
- ESP-IDF framework package: 4.60001.0 (ESP-IDF 6.0.1)
- Host C/C++ compiler: MinGW-w64 GCC 16.1.0
- Isolated PlatformIO executable: `C:\Users\leals\AppData\Local\Temp\codex-rf3-pio-validation-019fc930\Scripts\platformio.exe`
- Isolated compiler directory: `C:\Users\leals\AppData\Local\Temp\codex-rf3-host-gcc-019fc930\mingw64\bin`
- Short isolated PlatformIO package cache used to avoid Windows path-length failures: `C:\Users\leals\AppData\Local\Temp\p3`

No toolchain, cache, credential, build output, or machine-specific path was added to firmware or PlatformIO configuration.

## Validation results

| Check | Result |
|---|---|
| Native Unity suite | 174 passed, 0 failed |
| File-staging Python suite | 6 passed, 0 failed |
| Custom-PCB clean | Passed |
| Custom-PCB full firmware | Passed; RAM 14,188 / 327,680 bytes; flash 255,541 / 1,572,864 bytes |
| Devboard clean | Passed |
| Devboard full firmware | Passed; RAM 14,320 / 327,680 bytes; flash 255,641 / 1,572,864 bytes |
| Custom-PCB SPIFFS image | Passed |
| Devboard SPIFFS image | Passed |
| `git diff --check` | Passed; line-ending conversion warnings only |
| HTTP default | Disabled in both canonical environments |
| Source credentials | Removed from current tree |
| Hardware profile pin checks | Passed natively and at compile time |

Commands used:

```powershell
platformio test -e native
python -m unittest -v test\test_stage_demo_file.py
platformio run -e rf3_custom_pcb -t clean
platformio run -e rf3_custom_pcb
platformio run -e rf3_esp32_devboard -t clean
platformio run -e rf3_esp32_devboard
platformio run -e rf3_custom_pcb -t buildfs
platformio run -e rf3_esp32_devboard -t buildfs
python tools\check_repository_hygiene.py
git diff --check
```

## Backup and recovery

Two pre-change backups were created outside the repository:

| Backup | SHA-256 |
|---|---|
| `C:\Users\leals\Documents\rf3-backups\rf3-integration-base-20260803-225138.bundle` | `4348EED3347173254FB632AD950DB1F9D593037AE688E38DFC18D753927E034D` |
| `C:\Users\leals\Documents\rf3-backups\rf3-integration-base-source-20260803-225138.zip` | `081190EF5B2FE1F6E04D5F4307FBA05C4E0F34A8F7B3A90EC338C37B09158823` |

The Git bundle provides commit/branch recovery; the ZIP preserves the exact base source tree independently of Git metadata.

## Remaining integration work

- Validate both pinouts and startup/status reporting on physical hardware.
- Exercise transfer start, retry, cancellation, timeout, collision publication, and verified completion over real radios.
- Rotate the removed Wi-Fi credentials because they remain in repository history.
- Review whether HTTP control should ever be enabled in a production environment; it remains unauthenticated and plaintext when explicitly enabled.
- Define application-level scheduling and callback handoff policies for future subsystem consumers.
