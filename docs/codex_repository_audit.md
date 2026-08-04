# RF3 Repository Audit

Audit date: 2026-08-03  
Repository: https://github.com/Oddrubberband/RF3esp32-Code-  
Audited revision: main at aae7af5f4a53ae0ff82247dc1f3aae6ccb3b2f10, committed 2026-06-11 14:09:21 UTC  
Audit target: generic, binary-safe, packetized file transfer over ESP32 and nRF24L01+PA+LNA; audio is a supported payload, not the transport architecture  
Audit method: complete source/configuration/documentation inventory, relevant branch/history comparison, static control-flow and protocol reconstruction, isolated clean builds, filesystem-image build, and inspection of the host-test suite  

No production firmware, header, build configuration, test, partition, script, binary asset, or pre-existing documentation file was modified. This report is the only authored repository deliverable.

## 1. Executive summary

The active system is an ESP-IDF application whose primary path is:

source file in SPIFFS → 28-byte chunks → fixed 32-byte nRF24 frames → receiver polling → temporary SPIFFS file → renamed received file.

That path is substantially more generic than the historical names suggest. It opens source and destination files in binary mode, uses explicit byte counts, and does not interpret payload bytes as text or samples. Nulls, 0xFF, embedded newlines, and arbitrary byte patterns are safe inside an individual nonempty data chunk. A .u8 file follows the same byte-copy path as another extension.

The project is nevertheless a prototype, not a reliable generic file-transfer subsystem. It has a packet format and a weak start/stop gate, but not a complete reliable transfer session. There is no receiver-ready handshake, end-to-end acknowledgment, retransmission based on receiver state, declared file size, total-packet count, data-packet transfer identifier, timeout, cancellation handshake, completion confirmation, or file hash. Hardware auto-ack is also disabled. The sender therefore reports local radio FIFO completion, not remote delivery.

The default 16 MB PCB/manual-boot firmware builds successfully, and its SPIFFS image builds successfully. The declared 4 MB devboard/Wi-Fi environment does not configure under the currently resolved ESP-IDF because its committed full sdkconfig is stale. Native tests were discovered but could not compile because this audit machine has no host GCC, Clang, or MSVC toolchain. The test source itself was still inspected completely.

Generic binary file transfer is not reliable in the current implementation:

- Files whose size is an exact multiple of 28 bytes never receive a LAST flag and are discarded by the receiver when STOP arrives.
- A zero-byte file produces START and STOP but no destination file.
- Loss of up to eight consecutive packets is explicitly tolerated, and the shortened file is renamed and counted as successfully saved.
- Files larger than 1,835,007 bytes roll the 16-bit sequence number; no size guard prevents this.
- An interrupted sender emits no cancel/stop frame, while the receiver has no transfer timeout.
- Stream identifiers restart at one after a transmitter reset, and the receiver removes an existing rx_####.bin before renaming the new file.
- Data packets carry no transfer identifier, so start/stop session identity does not bind the data frames.

The system is not ready for subsystem integration. Its upstream contract is a manually staged SPIFFS file plus serial or optional HTTP commands. Its downstream contract is another SPIFFS file with a generated name. There is no callback, queue, stream API, data-valid signal, or trustworthy completion result for another subsystem.

Largest immediate risks:

1. Silent file corruption is accepted and reported as success.
2. Common file-size boundaries fail deterministically.
3. Local TX completion is presented as if it represented remote delivery.
4. Interrupted and repeated sessions can leave stale state or overwrite valid files.
5. Active radio pins conflict with the stated intended wiring.
6. The devboard environment is not reproducible and enables an unauthenticated HTTP control plane using committed credentials.

No Critical finding was assigned because the primary default firmware and filesystem build are reproducible in the audited environment. There are multiple High findings capable of corrupting data, losing transfers, or breaking integration.

Software-only limitations: no ESP32 was flashed, no two-radio transfer was observed, no PCB netlist or schematic was available, and RF output power, PA/LNA supply integrity, antenna behavior, FIFO overrun rate, boot-button wiring, range, coexistence, and reset behavior could not be verified. Compilation is not treated as RF-reliability evidence.

The single best first implementation task is B1-01: establish one version-pinned, passing build and board/pin contract for both declared hardware profiles. It is the prerequisite for safely adding tests and changing the wire protocol.

## 2. Repository map

### 2.1 Active implementation decision

The active entry point is app_main in src/main.cpp. The evidence chain is:

- platformio.ini defaults to esp32wroom32d_manual_boot.
- That environment extends esp32wroom32d, selects the custom esp32wroom32d_n16 board, and uses ESP-IDF.
- Root CMakeLists.txt registers src as the component directory.
- src/CMakeLists.txt compiles main.cpp and the driver/support modules.
- app_main constructs Esp32Nrf24Hal, Nrf24, RadioManager, and DemoConsoleApp.
- The clean default build links that path successfully.

src/audio_reassembler.cpp is compiled into firmware but has no caller in the active application. Active RX persistence is implemented separately inside DemoConsoleApp in src/main.cpp. src/frame_io.cpp is excluded from ESP-IDF firmware and included only by the native test environment.

### 2.2 Complete file classification

| File path | Classification | Role | Evidence | Notes |
|---|---|---|---|---|
| .gitattributes | Active support | Text/binary attributes | Applies repository-wide; marks .u8 binary | Comment still calls all .u8 files PCM |
| .gitignore | Active support | Excludes PlatformIO/build/generated local files | Matches current toolchain outputs | Several generated sdkconfig files are nevertheless tracked |
| .scanoss/scanoss-raw.json | Generated | SCANOSS result | Contains scanner version and a developer-local COMMIT_EDITMSG path | Accidental/no runtime role |
| .vscode/extensions.json | Local configuration | Recommends PlatformIO | Editor-only | Harmless |
| .vscode/settings.json | Local configuration | ESP-IDF editor setup | Hard-codes C:\esp\v5.5.3\esp-idf and COM3 | Conflicts with resolved ESP-IDF 6.0.1 and default COM5 |
| boards/esp32wroom32d_n16.json | Active | Custom 16 MB PCB board target | Selected by default base environment | Names WROOM-32UE-N16; 16 MB flash |
| boards/esp32wroom32d_4mb.json | Probably active / conflicting | 4 MB devboard profile | Selected by declared esp32wroom32d_devboard environment | Its sdkconfig path makes that environment fail under current dependencies |
| boards/featheresp32_n16.json | Legacy | Earlier Feather target | No active PlatformIO environment references it | Leftover board configuration |
| CMakeLists.txt | Active | ESP-IDF project entry | Registers src and project nrf24_app | Disables IDF component manager |
| data/README.md | Documentation / active filesystem content | Staging workflow | buildfs includes it in SPIFFS | Its claim that .part files are hidden is false |
| data/song.u8 | Active demo asset | 8-bit audio payload | Included in built SPIFFS image | 1,112,701 bytes; remainder 9 modulo 28; lacks 0x00 and 0xFF |
| data/speech_test.u8 | Active demo asset | 8-bit audio payload | Included in built SPIFFS image | 360,000 bytes; remainder 4 modulo 28; lacks 0x00 and 0xFF |
| include/audio_packet.hpp | Active | Data-frame constants and API | Included by main, driver tests, StreamSync | Generic bytes under historical audio names |
| include/audio_reassembler.hpp | Duplicate / test-oriented | In-memory reassembler API | Only implementation/tests refer to class | Not used by active RX file path |
| include/esp32_nrf24_hal.hpp | Active | ESP32 pin/SPI HAL configuration | Instantiated by app_main | Defaults match platform flags, not stated intended CSN/IRQ wiring |
| include/frame_io.hpp | Test-only | Text record serialization | Only native environment includes implementation | Not an over-air or firmware interface |
| include/morse.hpp | Active optional application | Morse event generation/rendering | Used by DemoConsoleApp | Application-specific, not file transport |
| include/nrf24.hpp | Active | nRF24 driver API/state | Used by app and tests | Fixed-payload, no-ack driver |
| include/nrf24_hal.hpp | Active | Hardware abstraction interface | Implemented by ESP32 HAL and FakeHal | Good test seam |
| include/radio_manager.hpp | Active | App-facing radio state wrapper | Used by DemoConsoleApp | Status is local radio state, not peer delivery |
| include/rx_drain.hpp | Active | Bounded RX FIFO drain helper | Called by RX task and tested | Header-only |
| include/stream_sync.hpp | Active | START/STOP/remote control and ReceiverGate | Called by TX and RX paths | Incomplete transfer-session layer |
| include/tx_helpers.hpp | Active | Retry count and 8 kHz pacing | Called by sendDataFile | Mixes audio rate policy into generic transport |
| include/validation.hpp | Active support | Payload/channel/timing validation | Compiled and used by optional functions/tests | Small helper |
| include/wifi_control_config.hpp | Active only in devboard build | Wi-Fi credentials and node name | RF3_WIFI_CONTROL_ENABLED=1 in devboard env | Contains committed real-looking credentials |
| InstallationLog.txt | Generated / accidental | MSYS2 installer log | Developer paths and failed installer attempts | No build/runtime role |
| partitions.csv | Active | 16 MB PCB partition table | Default environment selects it | 1.5 MiB app, 14.4375 MiB SPIFFS |
| partitions_4mb.csv | Probably active | 4 MB devboard partition table | Devboard environment selects it | 1.5 MiB app, 2.4375 MiB SPIFFS |
| platformio.ini | Active | Build environments, pins, flags, filesystem | Controls all reproduced builds | Platform/dependencies unpinned; hard-coded ports |
| README.md | Documentation / conflicting | Project, wiring, commands, workflow | Describes current concept | Wiring and command list differ from code; examples contain developer path |
| sdkconfig.defaults | Active | Minimal default 16 MB IDF settings | Used by default board | Appropriate style |
| sdkconfig.devboard.defaults | Probably active | Minimal 4 MB defaults | Loaded by devboard board JSON | Combined with stale full sdkconfig |
| sdkconfig.devboard | Generated / conflicting | Full 4 MB ESP-IDF config | esp32wroom32d_4mb points to it | Causes current devboard configuration failure |
| sdkconfig.esp32wroom32d_devboard | Generated / legacy | Full 16 MB config for old environment name | No current environment of that exact behavior uses it | Duplicate hash with manual_boot variant |
| sdkconfig.esp32wroom32d_devboard_manual_boot | Generated / legacy duplicate | Full 16 MB config | Old environment naming | Byte-identical to sdkconfig.esp32wroom32d_devboard |
| sdkconfig.featheresp32 | Generated / legacy | Full Feather ESP-IDF config | No active environment references it | Leftover target |
| src/CMakeLists.txt | Active | ESP-IDF component source list/dependencies | Used by default build | Compiles unused audio_reassembler.cpp; excludes frame_io.cpp |
| src/audio_packet.cpp | Active | 32-byte data-frame serializer/deserializer | Called by main and StreamSync gate | Raw bytes and explicit lengths; zero-length encode rejected |
| src/audio_reassembler.cpp | Duplicate / test-oriented | In-memory byte reconstruction | No active application caller | Duplicates active file RX policy, including tolerated gaps |
| src/esp32_nrf24_hal.cpp | Active | GPIO/SPI implementation | Constructed by app_main | Polls IRQ GPIO; no ISR |
| src/frame_io.cpp | Test-only | Text line record parser/writer | Native source filter only | Not firmware |
| src/main.cpp | Active | Boot, serial/HTTP control, TX/RX, SPIFFS persistence, Morse/CW | Contains app_main and actual transfer state | Monolithic and owns duplicate RX logic |
| src/morse.cpp | Active optional application | Morse encoding/rendering | Used by command handlers | Separate enough from payload transport |
| src/nrf24.cpp | Active | nRF24 register/FIFO/mode/CW implementation | Used by RadioManager | No auto-ack/retry; clone timing fallback |
| src/radio_manager.cpp | Active | Radio state/status facade | Used by app | receivePayload marks Fault on a failed read |
| src/validation.cpp | Active support | Input validators | Linked in firmware/native tests | No transfer-session validation |
| test/README.md | Documentation | Native test command and scope | Matches platformio native environment | Overstates executability when no compiler is provisioned |
| test/include/fake_hal.hpp | Test-only | Simulated nRF24 HAL/register/FIFO behavior | Used by sole test suite | Useful unit seam |
| test/test_nrf24/test_main.cpp | Test-only | Unity unit suite | 77 test functions, 76 registered | Does not exercise active main.cpp file TX/RX; one defined test omitted |
| tools/platformio_audio_targets.py | Active host tooling / legacy naming | Adds staging custom targets | Loaded as pre-script by every firmware env | Generic target plus legacy audio alias |
| tools/prepare_demo_audio.py | Legacy compatibility wrapper | Runs stage_demo_file.py | Not required by active workflow | Harmless alias |
| tools/stage_demo_file.py | Active host tooling | Sanitizes/copies arbitrary file and estimates SPIFFS fit | README and PlatformIO target call it | Replacement fit check can approve a directory it does not actually create |

No examples directory, archived firmware directory, second active entry point, or active external high-power-PA implementation was found. No file remained Unclear after inspection.

### 2.3 Relevant history and branches

The public API returned 39 commits from 2026-03-17 through 2026-06-11. Most recent messages are non-descriptive, including jn, g, and random-looking strings, so history alone is weak design evidence.

Three non-main branches and one tag were accessible:

- backup/before-ai-edits
- restore-ts
- work/test-changes
- before-ai-edits-2026-04-14

All point to 9ef4cfb1aff11dfe9cfc75bc2e16127db6de0694, “Backup before changes,” dated 2026-04-14. Relative to that snapshot, current main adds or substantially changes session sync, Wi-Fi control, RX-drain/TX helpers, generic staging, main.cpp, radio behavior, and tests: 4,594 insertions and 666 deletions over 46 paths. Those branches are historical snapshots, not competing active implementations. They confirm that the current generic/session behavior was layered onto an earlier audio demo.

## 3. Active build configuration

### 3.1 Intended primary environment

| Item | Active value | Evidence |
|---|---|---|
| Default environment | esp32wroom32d_manual_boot | platformio.ini:11-14 |
| Base environment | esp32wroom32d | platformio.ini:16-47 |
| Board | esp32wroom32d_n16 | platformio.ini:19; boards/esp32wroom32d_n16.json |
| MCU | ESP32 / ESP32-WROOM-32UE-N16 | Board JSON and project context |
| Framework | ESP-IDF | platformio.ini:20 |
| Platform | espressif32, unpinned | platformio.ini:18 |
| Filesystem | SPIFFS | platformio.ini:38 |
| Partition table | partitions.csv | platformio.ini:39 |
| Upload/monitor | 115200 baud, COM5, DTR/RTS disabled | platformio.ini:22-33 |
| Manual boot behavior | no pre-upload reset; hard reset after | platformio.ini:49-55 |
| Wi-Fi control | Disabled | build flag at platformio.ini:42 |
| Radio pin flags | CE17, CSN27, IRQ16 | platformio.ini:44-46 |

The devboard environment is declared but called “Legacy” in its own comments. It changes to the 4 MB board/partition table, COM3, node name rf3-devboard, and enables Wi-Fi. It retains the PCB CE17/CSN27/IRQ16 pin flags.

### 3.2 Resolved dependency versions

The repository declares no lib_deps and disables the ESP-IDF managed-component mechanism. PlatformIO resolved:

| Dependency | Resolved version |
|---|---|
| PlatformIO Core | 6.1.19 |
| platform-espressif32 | 7.0.1 |
| framework-espidf | 4.60001.0, ESP-IDF 6.0.1 |
| xtensa-esp-elf toolchain | 15.2.0+20251204 |
| esptool | 4.11.0 |
| CMake | 3.30.2 |
| Ninja | 1.9.0 |
| mkspiffs | 2.30 |
| Native platform | 1.2.1 |
| Unity for native tests | 2.6.1 |

These versions are observations, not repository guarantees, because platform and native platform versions are not pinned. .vscode/settings.json instead points to ESP-IDF 5.5.3.

### 3.3 Flash, partitions, and filesystem

The 16 MB map is:

| Name | Type | Offset | Size |
|---|---|---:|---:|
| nvs | data/nvs | 0x9000 | 0x5000 |
| phy_init | data/phy | 0xe000 | 0x1000 |
| factory | app/factory | 0x10000 | 0x180000, 1,572,864 bytes |
| spiffs | data/spiffs | 0x190000 | 0xE70000, 15,138,816 bytes |

The 4 MB profile retains the 1.5 MiB app and reduces SPIFFS to 0x270000, 2,555,904 bytes. The active mount uses /spiffs, max_files=8, and format_if_mount_failed=false. The default build generated ESP32, 16 MB, partitions.csv, 160 MHz CPU, 3,584-byte main task stack, 100 Hz FreeRTOS tick, SPIFFS 256-byte pages, and 32-byte object names.

### 3.4 Exact audit commands and results

The local workspace began as an empty initialized Git directory, so the exact public main archive was inspected and built in an isolated temporary source tree. Source hashes matched the main archive before this report was added.

The effective commands were:

    $env:PLATFORMIO_CORE_DIR = "C:\Users\leals\AppData\Local\Temp\p"
    C:\Users\leals\AppData\Local\Temp\codex-rf3-pio-env-019fc930\Scripts\platformio.exe run --project-dir C:\Users\leals\AppData\Local\Temp\codex-rf3-main-src-019fc930\RF3esp32-Code--main -e esp32wroom32d_manual_boot -t clean
    C:\Users\leals\AppData\Local\Temp\codex-rf3-pio-env-019fc930\Scripts\platformio.exe run --project-dir C:\Users\leals\AppData\Local\Temp\codex-rf3-main-src-019fc930\RF3esp32-Code--main -e esp32wroom32d_manual_boot
    C:\Users\leals\AppData\Local\Temp\codex-rf3-pio-env-019fc930\Scripts\platformio.exe run --project-dir C:\Users\leals\AppData\Local\Temp\codex-rf3-main-src-019fc930\RF3esp32-Code--main -e esp32wroom32d_manual_boot -t buildfs
    C:\Users\leals\AppData\Local\Temp\codex-rf3-pio-env-019fc930\Scripts\platformio.exe run --project-dir C:\Users\leals\AppData\Local\Temp\codex-rf3-main-src-019fc930\RF3esp32-Code--main -e esp32wroom32d_devboard
    C:\Users\leals\AppData\Local\Temp\codex-rf3-pio-env-019fc930\Scripts\platformio.exe test --project-dir C:\Users\leals\AppData\Local\Temp\codex-rf3-main-src-019fc930\RF3esp32-Code--main -e native -v

The short PlatformIO core directory was necessary because the first dependency extraction attempt exceeded Windows path handling inside ESP-IDF packages. That was an audit-machine/tool cache issue, not a repository compiler failure.

Primary result: success.

- Clean target: success.
- Firmware build: success.
- RAM: 14,180 of 327,680 bytes, 4.3%.
- Factory-app flash: 242,233 of 1,572,864 bytes, 15.4%.
- firmware.bin: 242,656 bytes; SHA-256 6dbafbc01f396d9468b3824c746d7f814698aafee056786703558e2774fb1051.
- bootloader.bin: 26,128 bytes.
- partitions.bin: 3,072 bytes.
- No meaningful application compiler warning was observed.

Filesystem result: success.

- spiffs.bin: 15,138,816 bytes, exactly the configured partition size.
- SHA-256: 729989c9d01013a45e04bfef3a14213ab6bdd3621032def6cd608b3d8b6202fb.
- Included /README.md, /song.u8, and /speech_test.u8.

Devboard result: failure during CMake/kconfgen before compilation. ESP-IDF 6.0.1 reported renamed symbols in sdkconfig.devboard, then kconfgen raised AttributeError: 'NoneType' object has no attribute 'name'. This is a repository configuration/reproducibility failure for the declared devboard environment.

Native test result: one suite collected, zero tests executed. PlatformIO could not find gcc/g++, clang, or MSVC on the audit host. The native environment assumes an existing compiler and does not provision one. This is primarily an audit-environment limitation, with a secondary reproducibility weakness in the repository.

## 4. Reconstructed architecture

### 4.1 Layer and ownership map

~~~mermaid
flowchart LR
    A["Serial console or optional HTTP"] --> B["DemoConsoleApp in main.cpp"]
    C["SPIFFS source file"] --> B
    B --> D["AudioPacket serializer"]
    B --> E["StreamSync control/gate"]
    D --> F["RadioManager"]
    E --> F
    F --> G["Nrf24 register/FIFO driver"]
    G --> H["Esp32Nrf24Hal: SPI3 + GPIO"]
    H --> I["Local nRF24L01+PA+LNA"]
    I <--> J["2.4 GHz link"]
    J <--> K["Peer nRF24L01+PA+LNA"]
    K --> L["Peer RX polling and ReceiverGate"]
    L --> M["DemoConsoleApp file reassembly"]
    M --> N["SPIFFS rx_####.part / rx_####.bin"]
~~~

The hardware and packet layers are reasonably separable. The reliable-session layer is not complete, and the generic file source/destination plus application control are concentrated in the 3,225-line main.cpp.

### 4.2 Boot flow

1. ESP-IDF invokes app_main.
2. Default pins instantiate Esp32Nrf24Config.
3. Esp32Nrf24Hal configures CE output, optional IRQ input, SPI3 bus, and nRF24 device at 1 MHz, mode 0. GPIO/SPI calls use ESP_ERROR_CHECK.
4. Nrf24, RadioManager, and DemoConsoleApp are constructed.
5. DemoConsoleApp creates radio, loop, and command mutexes; disables stdio buffering.
6. SPIFFS mounts at /spiffs without format-on-failure.
7. RadioManager probes RF_CH read/write, then programs channel 76 and fixed radio defaults.
8. Radio failure at the probe level is logged and the console continues in filesystem-only mode.
9. If payload.bin is missing, the lexicographically first regular SPIFFS file becomes selected. In the built image that is README.md.
10. RX-poll and loop-worker tasks are created.
11. Optional wireless/HTTP controls start according to compile flags.
12. Serial help/status are printed and the blocking character-input loop begins.

Default application/radio state is Standby, not RX. A peer must be commanded into RX before transmission unless a separately enabled wireless auto-RX option is used.

Failure behavior:

- GPIO/SPI HAL errors abort through ESP_ERROR_CHECK before the graceful “radio unavailable” path.
- SPIFFS mount or task/mutex creation failure returns from initialize; app_main then delays forever and exposes no command recovery.
- A failed radio probe is nonfatal.
- Later commands generally call ensureStandbyLocked, which attempts a fresh boot from Boot or Fault state.

### 4.3 Command flow

The main task reads stdin one character at a time. CR or LF dispatches a nonempty line. A maximum of 159 characters is retained in a 160-byte logical buffer; excess characters are silently dropped. Tokenization splits ASCII whitespace and provides no quoting, so filenames cannot contain whitespace. Command keywords are uppercased and are case-insensitive; filename arguments retain case. Dispatch is serialized with command_mutex_. Long operations are placed in the background loop task, so the serial task remains able to issue STOP.

HTTP and optional RF remote control reuse the same dispatcher. The default PCB build disables both Wi-Fi and RF remote-command forwarding. The devboard enables HTTP but not RF remote command forwarding.

### 4.4 TX flow

~~~mermaid
sequenceDiagram
    participant U as "Command/API"
    participant W as "Loop worker"
    participant FS as "SPIFFS"
    participant RM as "RadioManager"
    participant R as "nRF24"
    U->>W: "TX or TX LOOP"
    W->>RM: "normalize to Standby"
    W->>FS: "fopen(path, rb)"
    W->>RM: "START(stream_id) x5"
    loop "fread up to 28 bytes"
        W->>W: "seq16, FIRST/LAST, zero padding"
        W->>RM: "local send retry up to 3"
        RM->>R: "flush TX, write 32 bytes, pulse CE"
        Note over RM,R: "No auto-ack; TX_DS/FIFO-empty is local completion"
        W->>RM: "extra seq0 x2; seq1 x1"
        W->>W: "8 kB/s audio-derived pacing"
    end
    W->>RM: "STOP(stream_id) x3; results ignored"
    W->>FS: "fclose"
    W-->>U: "local pass complete"
~~~

Metadata creation is limited to a 16-bit RAM-only stream ID in START/STOP. Data frames contain sequence, payload length, and flags only. No filename, file size, total count, checksum, or receiver readiness is sent.

The sender uses a local three-attempt retry helper, but all attempts are unacknowledged over the air. Sequence 0 is transmitted three times total and sequence 1 twice to improve startup odds. Remaining data frames are transmitted once. STOP is transmitted three times.

The is_last decision is bytes_read < 28. This works for a short final chunk but not for exact multiples. File handles are closed on the explicit error paths inspected. STOP failures and fclose status are not checked.

### 4.5 RX flow and transfer lifecycle

1. RX command stops other loop activity, normalizes to Standby, starts radio RX, and resets session counters/gate.
2. The RX task polls every 2 ms, takes the radio mutex, and drains at most eight pending fixed-width frames.
3. Remote command frames are intercepted only if RF wireless control is compiled on.
4. ReceiverGate checks START, STOP, then data:
   - START stores stream ID and waits for sequence 0.
   - A legacy FIRST+sequence-0 data frame is also accepted without START and uses stream ID zero.
   - A matching data start opens rx_####.part with wb.
   - Old sequence numbers are ignored.
   - Forward gaps of one through eight are counted but accepted; missing bytes are not represented.
   - A gap over eight discards the partial file and resets the gate.
   - LAST closes and renames the partial file after removing any same-name final file.
   - STOP without LAST discards the active partial file.
5. There is no inactivity timeout. An interrupted transfer can remain active/open until another command, start, large gap, local RX exit, write error, or reset.
6. A power reset can leave a persisted .part file; startup does not scan or clean it.

### 4.6 Radio configuration

| Setting | Active value | Evidence/behavior |
|---|---|---|
| SPI host | SPI3_HOST | Esp32Nrf24Config |
| SPI pins | SCK18, MOSI23, MISO19 | Header defaults; no PlatformIO override |
| SPI mode/frequency | Mode 0, 1 MHz | esp32_nrf24_hal.cpp:50-54 |
| CE/CSN/IRQ | 17/27/16 | platformio.ini:44-46 |
| Address | 52 46 33 24 01 hex | src/nrf24.cpp:6 |
| Address width | 5 bytes | SETUP_AW=0x03 |
| Pipes | RX pipe 0 enabled; RX_ADDR_P0 and TX_ADDR same | EN_RXADDR=0x01; register writes |
| Channel | 76, 2476 MHz center nominally | initialize and RF_CH |
| Data rate | 250 kbps | RF_SETUP=0x26, RF_DR_LOW set |
| Chip RF power bits | Level 3, maximum setting on genuine nRF24 | RF_SETUP bits 2:1 = 11 |
| External PA/LNA result | Cannot determine | Module/hardware dependent |
| Payload | Static, 32 bytes | RX_PW_P0=32; FEATURE/DYNPD=0 |
| Auto-ack | Disabled | EN_AA=0 |
| Hardware retry | Disabled | SETUP_RETR=0 |
| Hardware CRC | Enabled, 2 bytes | CONFIG=0x0C |
| IRQ | GPIO input, interrupt disabled, active-low polled | esp32_nrf24_hal.cpp:28-35, 80-91 |
| Power-up timing | 1.5 ms; RX CE settle 150 us | src/nrf24.cpp:226-262 |
| TX FIFO | Flushed before each send and after timeout/MAX_RT | transmitOnce |
| RX FIFO | Flushed on init/start RX/rearm, then drained by polling | startRx/readOnePacket |

The driver first uses a CE pulse, then on local timeout reprograms the radio and retries while holding CE high. It also treats an empty TX FIFO after 300 us as successful local completion in no-ACK mode. Comments explicitly describe clone modules and long-wire setups; this is timing/hardware dependence, not peer delivery evidence.

### 4.7 Filesystem flow

- Host staging sanitizes a destination basename and copies it into data.
- buildfs packs every regular data file, not only .u8 files.
- Firmware mounts the prebuilt image without formatting.
- FILES lists every regular entry except names beginning with a dot.
- TX opens a named entry under /spiffs in rb.
- RX writes rx_####.part in wb and renames it to rx_####.bin on LAST.
- Existing partial and final paths are removed before replacement.
- A short fwrite aborts and removes the partial.
- There is no preallocation/free-space check, fsync, or checked fclose.
- A .part suffix is not hidden because its basename begins with r, not a dot.

## 5. Reconstructed interfaces

### 5.1 GPIO interface

| Pin | Active signal | Direction | Purpose | Definition | Discrepancy |
|---:|---|---|---|---|---|
| 18 | SCK | ESP32 output | SPI clock | include/esp32_nrf24_hal.hpp:7-9 | Matches stated intended mapping |
| 23 | MOSI | ESP32 output | SPI command/data to nRF24 | Header:11-13 | Matches |
| 19 | MISO | ESP32 input | SPI status/data from nRF24 | Header:15-17 | Matches |
| 17 | CE | ESP32 output | RX/TX mode/launch | platformio.ini:44 | Matches |
| 27 | CSN | ESP32 output via SPI driver | nRF24 chip select | platformio.ini:45 | Stated intended CSN is GPIO5 |
| 16 | IRQ | ESP32 input, polled | Active-low nRF24 status | platformio.ini:46 | Stated intended IRQ is GPIO27 |
| 5 | None in active radio config | — | Stated intended CSN | Project brief; README says CSN5 | Not used by active radio |

README.md instead calls CE27, CSN5, IRQ26 the “default devboard” wiring and then says the custom PCB overrides those pins. The repository therefore contains three pin narratives. Only CE17/CSN27/IRQ16 is compiled in both declared firmware environments.

### 5.2 Serial interface

Common parser contract:

- 115200 baud.
- Command keyword is case-insensitive; arguments are case-preserving.
- Spaces/tabs and other ASCII whitespace delimit tokens; repeated whitespace is collapsed.
- No quoted filenames or escaped whitespace.
- Either CR or LF terminates. CRLF causes one command followed by an ignored empty line.
- 159 retained characters maximum; extra input is silently dropped.
- Commands return immediately after queuing loop work unless noted.
- Local and HTTP/RF command dispatch share a mutex.

| Command | Exact syntax/arguments | Valid states | Success / error behavior | Side effects / blocking | Status |
|---|---|---|---|---|---|
| HELP, ? | No args | Any initialized state | Prints runtime command list | None; immediate | Active; ? undocumented |
| STATUS | No args | Any | Prints local radio, pin, selected file, loop, RX counters, faults | Brief radio snapshot; immediate | Active |
| STOP | No args | Any | Reports standby or failure to stop within 2 s | Requests loop stop, exits RX/CW, discards active partial, normalizes radio | Active |
| FILES, LS | No args | Filesystem mounted | Lists regular SPIFFS files or “No staged files” | Includes .part files; immediate | Active; LS undocumented |
| SELECT | SELECT filename | Any; file must exist | “Selected …” or usage/not-found | Changes default TX file | Active |
| TX | TX optional-filename | Any recoverable radio state | “TX started …” or state/file error | Queues one background pass; leaves selected file changed | Active |
| TX LOOP | TX LOOP optional-count-or-INF optional-file | Any recoverable radio state | Reports finite/infinite loop or usage error | Queues background repeated passes; STOP-capable | Active |
| MORSE | MORSE text | Any recoverable radio state | Queues Morse or rejects missing/unsupported text | Application-specific CW keying; STOP-capable | Active |
| REMOTE | REMOTE command-text | Standby-capable; feature flag required | Default build says disabled; otherwise validates max 26-byte printable command | Sends one RF command, no response/confirmation | Implemented but disabled by current environments and undocumented |
| RX | No args | Any recoverable state | Reports listening or fault | Stops loop, enters RX, resets counters/gate | Active |
| STANDBY | No args | Any recoverable state | Reports standby/failure | Stops loop/CW/RX, discards partial | Active |
| SLEEP | No args | Any recoverable state | Reports sleep/failure | Stops active work and powers radio down into Sleep state | Active |
| WAKE | No args | Sleep/PowerDown or already recoverable | Reports standby/failure | Powers/reboots as needed | Active |
| POWERDOWN | No args | Any recoverable state | Reports power down/failure | Stops active work and powers radio down | Active |
| CHANNEL | CHANNEL 0-125 | Any recoverable state | Reports new channel or validation/fault | Discards partial, reboots radio on channel | Active |
| POWER | POWER 0-3 | Any radio state allowing register access | Reports level or validation/fault | Changes packet RF power bits | Active; omitted from README command list |
| CW | CW START optional-channel optional-power; CW LOOP on_ms off_ms optional-channel optional-power optional-EVERY loops; CW STOP redirects to STOP | Any recoverable state | Reports active/loop or usage/fault | Continuous carrier/payload-reuse test; STOP-capable | Active optional diagnostic |

Responses are console text rather than a stable machine-readable result schema. A queued TX success message means accepted for work, not completed or delivered.

Documentation mismatches:

- Code-only or omitted details: ?, LS, POWER, REMOTE, /power, /command, argument/alias details.
- README lists the main commands but not all active endpoints.
- No documented command exists only in prose and not code, but README examples use a developer-specific PlatformIO path and a non-default base environment.

### 5.3 Optional HTTP interface

Devboard-only compile flag enables an HTTP server on port 80 after Wi-Fi association:

| Method/path | Input | Result |
|---|---|---|
| GET /status | None | JSON local status |
| POST /rx/start | None | Dispatch RX |
| POST /tx/start | None | Dispatch TX |
| POST /stop | None | Dispatch STOP |
| POST /channel?value=0-125 | Query | Dispatch CHANNEL |
| POST /power?value=0-3 | Query | Dispatch POWER |
| POST /command | Query/body command text | Dispatch arbitrary allowed local command |

There is no authentication, authorization, transport security, origin check, or anti-replay mechanism. The “queued” response from /command is actually the result of synchronous dispatch/queue acceptance, not transfer completion.

### 5.4 RF packet interface

All transmitted frames are fixed 32 bytes.

#### Data frame

| Byte(s) | Width | Field | Encoding |
|---:|---:|---|---|
| 0-1 | 16 bits | Sequence | Little-endian unsigned |
| 2 | 8 bits | Payload length | 0-28 accepted by decoder; encoder requires 1-28 |
| 3 | 8 bits | Flags | bit0 FIRST, bit1 LAST; other bits not rejected |
| 4-31 | 28 bytes | Data/padding | First length bytes are payload; remaining bytes zero on local encoder |

No version, packet-type discriminator, transfer ID, file size, filename, total count, application type, checksum, or hash exists in a data frame. No C/C++ struct is transmitted; fields are written byte by byte, so there is no struct-padding or compiler-layout dependence. Sequence endianness is explicit.

#### START/STOP control frame

| Byte(s) | Field | Encoding |
|---:|---|---|
| 0-2 | Reserved/data-shape guard | Encoder writes zero; decoder checks only byte 2 |
| 3 | Type | 0x81 START or 0x82 STOP |
| 4-6 | Magic | ASCII RF3 |
| 7 | Version | 1 |
| 8-9 | Stream ID | Big-endian uint16 |
| 10-31 | Reserved | Encoder writes zero; decoder does not validate |

START is repeated five times at 15 ms spacing followed by 40 ms. STOP is repeated three times at 8 ms spacing. STOP is only a sender indication, not an acknowledgment or completion frame.

#### Remote command/response frame

| Byte(s) | Field | Encoding |
|---:|---|---|
| 0-2 | Magic | ASCII RF3 |
| 3 | Type | 0x83 command or 0x84 response |
| 4 | Version | 1 |
| 5 | Text length | 1-26 |
| 6-31 | Text/zero padding | Printable ASCII; response also permits CR/LF/tab |

Remote response encode/decode exists, but active firmware neither transmits nor consumes a response. The one response round-trip test is defined but not registered.

#### Absent protocol representations

| Concern | Representation |
|---|---|
| Receiver readiness | None |
| Data acknowledgment | None |
| Negative acknowledgment | None |
| Retry request | None |
| Error | None |
| Cancellation | None |
| Completion confirmation | None |
| File integrity | None |
| Sender/receiver reset epoch | None |

The current framing is not a complete file-transfer protocol.

Maximum size:

- Sequence has 65,536 values.
- At 28 payload bytes, the wire field could distinguish at most 1,835,008 payload bytes if a full final frame could be marked LAST.
- The current sender’s exact-multiple defect makes the largest representable file that can complete before rollover 1,835,007 bytes.
- The firmware does not enforce this maximum. The 16 MB SPIFFS partition and even the 4 MB profile can hold files larger than it.
- No file-size or packet-count field exists, so the peer cannot independently enforce or validate a declared maximum.

### 5.5 File interface and binary-safety verdict

| Property | Current contract |
|---|---|
| Host source | Any regular host file passed to tools/stage_demo_file.py |
| ESP32 source | /spiffs/name; selected by FILES/SELECT/TX |
| Required extension | None in firmware; staging sanitizes extension to 1-8 alphanumerics or .bin |
| Default source | payload.bin, then lexicographically first file if absent |
| Destination | /spiffs/rx_####.part, then rx_####.bin |
| Filename carried over RF | No |
| Destination collision | Existing final file is removed |
| Partial visibility | Listed by FILES because basename does not begin with dot |
| Open modes | rb and wb |
| Byte handling | fread/fwrite with explicit counts |
| Content sentinel | None |
| Practical validated sizes | Demo assets only; both avoid exact-28 boundary |
| Protocol maximum | 1,835,007 bytes in current implementation |
| Completion proof | LAST flag and successful local rename only |

Binary-safety verdict:

- Payload content: binary-safe for each accepted nonempty chunk. Payload bytes never pass through strlen, %s, line-based I/O, string constructors without length, or sentinel-based EOF logic.
- File domain: not fully binary-safe/reliable because zero-length and exact-multiple sizes fail, size rollover is unchecked, missing chunks produce shortened “successful” files, and no byte-for-byte integrity check exists.
- The bundled .u8 files are weak generic-binary fixtures: song.u8 has 131 distinct byte values and speech_test.u8 has 161; neither contains 0x00 or 0xFF. They do not establish arbitrary-binary correctness.
- A one-byte, 1-27-byte, 29-byte, or other nonmultiple file should segment correctly by code inspection, subject to RF loss. A 28-byte or 56-byte file fails deterministically.

Expected transfer time has an audio-derived lower bound of size / 8,000 seconds, before RF transaction, repeated startup packets, and scheduling overhead. The bundled files therefore have lower bounds of about 139.1 seconds and 45.0 seconds. Actual time will be longer. No generic throughput/latency contract is documented.

## 6. Confirmed defects

### F-TX-001 — Empty and exact-28-byte-multiple files cannot complete

| Field | Value |
|---|---|
| Severity | High |
| Confidence | Confirmed |
| Path / location | src/main.cpp:480-647, sendDataFile; src/main.cpp:2944-2947, processRxPayload; src/audio_packet.cpp:19 |
| Category | Confirmed firmware defect; packetization/end-of-transfer |
| Dependencies | Protocol specification and segmentation tests should precede correction |

Current behavior and evidence: sendDataFile sets LAST only when fread returns fewer than 28 bytes. A full final read is sent without LAST; the next read returns zero and exits the loop. STOP then causes RX to discard a transfer that never saw LAST. A zero-length file emits no data frame because AudioPacket::encode rejects length zero.

Why it matters / likely failure: common binary sizes 28, 56, and so on consistently lose the transfer; an empty file silently produces no destination. This violates generic file support independently of RF quality.

Recommended correction: determine file size/look ahead before setting LAST, or define an explicit END frame carrying size/hash. Define empty-file representation. Do not infer completion solely from a short read.

Software verification: table-driven segment/reassemble tests for 0, 1, 27, 28, 29, 55, 56, and 57 bytes, asserting flags, frame counts, output size, and SHA-256.

Hardware validation: two-board repeats of the same boundary matrix and byte-for-byte comparison. Hardware is not required to prove the logic fix.

### F-RX-001 — Missing packets are omitted and the corrupt file is reported saved

| Field | Value |
|---|---|
| Severity | High |
| Confidence | Confirmed |
| Path / location | src/main.cpp:1433-1472 finalizeIncomingFileTransfer; 1475-1536 appendIncomingFileChunk; 2982-2987 processRxPayload; src/audio_reassembler.cpp:38-61 |
| Category | Confirmed firmware defect; data integrity |
| Dependencies | Reliable-session policy and end-to-end integrity design |

Current behavior and evidence: forward gaps of up to eight increment missing_packets and continue writing later payloads. LAST then renames the shortened file, increments saved counters, and logs “Saved RX file … missing=N.” The registered unit test test_audioReassembler_accepts_small_forward_gap_and_records_missing asserts that the shortened byte vector is complete.

Why it matters / likely failure: a lost data frame creates a shifted/truncated binary that looks valid to users and other subsystems. For audio it may sound damaged; for arbitrary data it may be unusable or dangerous.

Recommended correction: never publish a file with missing data as successful. Buffer/retry via NACK or fail and remove/quarantine it. Require declared byte count and final integrity verification before atomic publication.

Software verification: inject each single/multiple missing sequence location; assert retransmission or failed state, no final filename, and no success counter until hash matches.

Hardware validation: controlled packet-drop tests on two boards. Dependencies: F-PROTO-001 and F-SESSION-001.

### F-PROTO-001 — TX success is local-only; no delivery acknowledgment or integrity proof exists

| Field | Value |
|---|---|
| Severity | High |
| Confidence | Confirmed |
| Path / location | src/nrf24.cpp:43-83 initDefaults; 280-431 transmitOnce; src/main.cpp:573-647 sendDataFile; include/stream_sync.hpp |
| Category | Confirmed protocol/firmware defect |
| Dependencies | None for specification; session changes must coordinate both peers |

Current behavior and evidence: EN_AA and SETUP_RETR are zero. transmitOnce returns true on TX_DS or, in no-ACK mode, an empty local TX FIFO. The application retry repeats only local launch attempts. There are no ACK/NACK/completion frames. STOP send results are explicitly cast to void, and sendDataFile logs “Finished” and returns true.

Why it matters / likely failure: an absent, powered-down, wrong-channel, overflowing, or reset receiver can receive nothing while the transmitter reports a completed pass. The current UI provides no trustworthy success signal to another subsystem.

Recommended correction: define application-level receiver-ready, ACK/NACK or window/bitmap recovery, END, verified-complete response, and error/cancel frames. Hardware auto-ack may be enabled as a link optimization but must not substitute for file completion and hash verification.

Software verification: paired simulated peers with deterministic drop/duplicate/reorder/fault injection; assert sender success only after peer verified completion.

Hardware validation: receiver-off, wrong-channel, antenna-attenuated, FIFO-overflow, and mid-transfer reset scenarios.

### F-SESSION-001 — Data frames are not bound to the START/STOP session

| Field | Value |
|---|---|
| Severity | High |
| Confidence | Confirmed |
| Path / location | include/audio_packet.hpp:13-27; include/stream_sync.hpp:47-126, 279-427 ReceiverGate |
| Category | Confirmed protocol/session defect |
| Dependencies | Protocol versioning decision |

Current behavior and evidence: the 16-bit stream ID exists only in START/STOP. Data frames contain no stream ID/version. ReceiverGate accepts legacy sequence-0/FIRST without START and assigns stream zero. START always replaces the current stream. pending_stream_id_ is never assigned a nonzero value, making its transition branches unreachable. Control decoding also does not validate bytes 0-1 or reserved padding.

Why it matters / likely failure: delayed data from an old transfer can be accepted under a new control session if its sequence fits. A missed/repeated START can reset state. Peer reset epochs are indistinguishable. The receiver cannot prove which transfer produced a data frame.

Recommended correction: include protocol version and sufficiently large transfer ID in every frame; remove legacy acceptance unless explicit compatibility mode is required; validate reserved/flag fields; define legal transitions.

Software verification: interleave frames from two transfer IDs, repeat/delay START, inject legacy and malformed controls, and assert isolation.

Hardware validation: back-to-back transfers with delayed/repeated packets and independent peer resets.

### F-STATE-001 — Cancellation and interruption do not close the peer session

| Field | Value |
|---|---|
| Severity | High |
| Confidence | Confirmed |
| Path / location | src/main.cpp:507-510, 538-541, 596-602 sendDataFile; 1370-1431 incoming transfer; include/stream_sync.hpp |
| Category | Confirmed state-recovery defect |
| Dependencies | Session protocol and timer source |

Current behavior and evidence: when loop_stop_requested_ becomes true, sendDataFile closes its local file and returns without sending STOP or CANCEL. The receiver has no inactivity timeout. Its partial file can remain open indefinitely until another event or reset.

Why it matters / likely failure: a user STOP, sender fault, power loss, or link loss leaves the receiver in stale streaming state and consumes a file handle. Subsequent frames can be misinterpreted; a reboot leaves an unmanaged .part file.

Recommended correction: define idempotent CANCEL/ABORT, receiver inactivity timeout, cleanup-on-boot, and explicit failed state. Make timeout values part of the interface contract.

Software verification: interrupt before data, mid-packet sequence, before END, and during retry; advance a fake clock and assert both peers reset and partial data is unavailable.

Hardware validation: user STOP, transmitter power-cycle, receiver power-cycle, and RF disconnect at multiple offsets.

### F-PROTO-002 — Sequence rollover is unchecked and the protocol maximum is unenforced

| Field | Value |
|---|---|
| Severity | High |
| Confidence | Confirmed |
| Path / location | include/audio_packet.hpp:24-27 Header; src/main.cpp:534, 621 sendDataFile; 1532 appendIncomingFileChunk |
| Category | Confirmed firmware/protocol defect |
| Dependencies | Protocol field-width decision |

Current behavior and evidence: sequence and next_sequence are uint16_t and increment with wrap. Neither staging nor firmware rejects files above the unambiguous limit. Active SPIFFS partitions can hold larger files.

Why it matters / likely failure: packet 65,536 becomes sequence zero. Active duplicate/start logic can ignore or mis-handle it, and a large file cannot reconstruct correctly.

Recommended correction: use a wider sequence/offset or segmented windows with explicit total size; enforce a documented maximum at staging and firmware boundaries.

Software verification: virtual files at maximum-minus-one, maximum, maximum-plus-one, and wrap; no large memory allocation should be required.

Hardware validation: one near-limit transfer after software tests pass.

### F-FS-001 — Stream-ID reuse destructively overwrites prior received files

| Field | Value |
|---|---|
| Severity | High |
| Confidence | Confirmed |
| Path / location | src/main.cpp:368-377 buildReceivedFileName; 490-495 sendDataFile; 1448-1450 finalizeIncomingFileTransfer |
| Category | Confirmed filesystem/session defect |
| Dependencies | Stable transfer identity and destination policy |

Current behavior and evidence: next_stream_id is RAM-only and starts at one after every sender boot. Destination name is solely rx_%04u.bin. Finalization removes an existing final path before rename.

Why it matters / likely failure: repeating a transfer after transmitter reset can delete a prior valid file without warning. Two transmitters also collide.

Recommended correction: use a wider random/persisted ID plus collision-safe naming, or let the receiving subsystem allocate a destination; never silently remove a successful artifact. Define overwrite policy explicitly.

Software verification: repeated IDs, transmitter restart, two senders, and existing-final fixtures; assert no destructive overwrite.

Hardware validation: repeat transfers across sender power cycles.

### F-CFG-001 — Active CE/CSN/IRQ configuration conflicts with the intended wiring

| Field | Value |
|---|---|
| Severity | High |
| Confidence | Confirmed for configuration discrepancy; hardware effect requires validation |
| Path / location | platformio.ini:40-47 and 65-72; include/esp32_nrf24_hal.hpp:19-32; README.md:23-35 |
| Category | Build/configuration defect |
| Dependencies | PCB revision/netlist confirmation |

Current behavior and evidence: both firmware environments compile CE17/CSN27/IRQ16. The project brief states CE17/CSN5/IRQ27. README states devboard CE27/CSN5/IRQ26. IRQ16 is configured as a polled input; GPIO27 is used as CSN, not IRQ.

Why it matters / likely failure: a board wired to the stated intended map will not select the nRF24 and will poll the wrong IRQ. This can prevent basic TX/RX.

Recommended correction: establish one per-board pin contract from schematic/netlist, centralize it, add compile-time collision/strapping checks, and update runtime/status documentation.

Software verification: compile-time assertions and build-matrix tests printing expected pin manifests.

Hardware validation: continuity test and SPI probe on each board revision. Hardware confirmation is required before changing active pins.

### F-CFG-002 — The declared 4 MB devboard environment fails configuration

| Field | Value |
|---|---|
| Severity | Medium |
| Confidence | Confirmed by clean audit build |
| Path / location | boards/esp32wroom32d_4mb.json build.esp-idf.sdkconfig_path; sdkconfig.devboard; platformio.ini:57-72 |
| Category | Build/configuration defect |
| Dependencies | Version pinning decision |

Current behavior and evidence: PlatformIO with resolved ESP-IDF 6.0.1 loads sdkconfig.devboard, reports multiple renamed options, then kconfgen crashes before compilation. The primary PCB environment succeeds because it generates a current environment-specific config from minimal defaults.

Why it matters / likely failure: the profile that enables Wi-Fi control cannot be reproduced from a clean checkout with the repository’s current unpinned dependencies.

Recommended correction: pin a supported platform/ESP-IDF version and replace committed full generated configs with minimal reviewed defaults or regenerate them under the pinned version.

Software verification: clean CI build and buildfs for all declared environments.

Hardware validation: 4 MB flash/partition and Wi-Fi boot test after build succeeds.

### F-SEC-001 — Devboard credentials are committed and control endpoints are unauthenticated

| Field | Value |
|---|---|
| Severity | High |
| Confidence | Confirmed |
| Path / location | include/wifi_control_config.hpp:3-10; platformio.ini:65-72; src/main.cpp:948-990, 1187-1282 |
| Category | Configuration/security/integration defect |
| Dependencies | Product/integration threat model |

Current behavior and evidence: a real-looking SSID and password are source defaults. The devboard build enables HTTP port 80 with status, arbitrary command, TX, RX, STOP, channel, and power endpoints. No authentication or transport security is implemented.

Why it matters / likely failure: credentials are exposed to every repository reader, and any host on the joined network may control RF behavior, including long TX/CW operations.

Recommended correction: revoke/change the credential, remove it from tracked history where appropriate, inject secrets outside source, disable HTTP by default, and add an authenticated narrow API if it remains necessary.

Software verification: secret scanning, build without credentials, endpoint authorization tests, and command allow-list tests.

Hardware validation: network isolation/authentication test on the devboard.

### F-ARCH-001 — Generic transfer is throttled and constrained by audio policy

| Field | Value |
|---|---|
| Severity | Medium |
| Confidence | Confirmed |
| Path / location | src/main.cpp:69-106 and 627-632; include/tx_helpers.hpp:10-24 |
| Category | Confirmed architecture/integration defect |
| Dependencies | Throughput and coexistence requirements |

Current behavior and evidence: transport constants hard-code 8,000 bytes/s and assert 125 us per “audio byte.” Every file uses calculateAudioPacingDelay regardless of type.

Why it matters / likely failure: arbitrary files inherit an undocumented audio sample-rate assumption, producing long transfers and expanding exposure to loss/reset. The transport cannot negotiate or expose throughput.

Recommended correction: move rate limiting into a configurable policy/application layer. Use radio/session flow control for reliability. Keep optional 8 kHz pacing only for an audio handler if real-time behavior is required.

Software verification: segmentation output invariant across rate policies; timing tests for unthrottled, capped, and audio modes.

Hardware validation: throughput/loss measurements at selected rates.

### F-FS-002 — Partial files are visible, selectable, and persist without startup cleanup

| Field | Value |
|---|---|
| Severity | Medium |
| Confidence | Confirmed |
| Path / location | src/main.cpp:368-377, 403-430 listTracks, 1401-1431; data/README.md |
| Category | Confirmed filesystem/documentation defect |
| Dependencies | Failure-state and destination policy |

Current behavior and evidence: partial names are rx_####.part. listTracks hides only entries whose first character is a dot, so .part files are listed and may be selected/transmitted. Startup does not remove/quarantine stale partials. Documentation says they are hidden.

Why it matters / likely failure: interrupted data can be mistaken for a valid source or manually retransmitted. It also consumes storage across resets.

Recommended correction: store partials in a hidden/internal namespace, exclude by suffix/state, scan and clean/quarantine on boot, and expose explicit failed-transfer diagnostics.

Software verification: boot with stale partials, FILES/SELECT tests, and cleanup policy tests.

Hardware validation: power-cut during RX followed by reboot.

### F-CONC-001 — Cross-task stop state uses volatile rather than synchronization

| Field | Value |
|---|---|
| Severity | Medium |
| Confidence | Confirmed under the C++ memory model |
| Path / location | src/main.cpp:1563-1598, 2719-2754, 3181 loop_stop_requested_ |
| Category | Confirmed firmware concurrency defect |
| Dependencies | None |

Current behavior and evidence: command and loop tasks read/write volatile bool loop_stop_requested_ without consistently holding loop_mutex_ or using an atomic/FreeRTOS primitive. volatile does not establish inter-task synchronization in C++.

Why it matters / likely failure: behavior is formally undefined; optimization or timing can delay/miss cancellation or race with loop state reset.

Recommended correction: use a FreeRTOS event/notification or std::atomic if supported, with a documented ownership protocol.

Software verification: thread sanitizer on a host-extracted loop controller where feasible, plus high-iteration stop/start stress tests.

Hardware validation: rapid STOP/TX/CW/Morse stress over long runs.

### F-TOOL-001 — Replacement staging can approve a filesystem image it does not create

| Field | Value |
|---|---|
| Severity | Medium |
| Confidence | Confirmed by code inspection |
| Path / location | tools/stage_demo_file.py:147-167 stage_data_directory; 225-245 main |
| Category | Confirmed PC-tool defect |
| Dependencies | Desired replace semantics |

Current behavior and evidence: --replace-existing-files omits old files only from the temporary fit-check directory. After validation, the script copies the new file into the real data directory but never removes those old files. A fit check may pass while the subsequent actual buildfs still contains all old assets and can fail.

Why it matters / likely failure: a user receives false assurance that a staged image fits.

Recommended correction: either atomically apply the same replacement set to data after successful validation or rename the option to simulation-only and validate the actual final directory.

Software verification: temporary data directory near capacity; run replace mode; assert real directory contents exactly match validated contents and buildfs succeeds.

Hardware validation: none beyond one upload smoke test.

### F-INIT-001 — Some initialization failures bypass the advertised recoverable console

| Field | Value |
|---|---|
| Severity | Medium |
| Confidence | Confirmed |
| Path / location | src/esp32_nrf24_hal.cpp:24, 35, 46, 56, 72; src/main.cpp:687-714 and 3215-3221 |
| Category | Confirmed firmware robustness defect |
| Dependencies | Error-model decision |

Current behavior and evidence: low-level GPIO/SPI operations use ESP_ERROR_CHECK and abort on failure before RadioManager can report “radio unavailable.” SPIFFS mount failure prevents the console task from running and app_main delays forever. Only a failed nRF24 register probe follows the documented filesystem-only mode.

Why it matters / likely failure: pin/resource/configuration failures can reboot/abort, and a missing/corrupt filesystem removes the very serial interface needed for diagnostics or recovery.

Recommended correction: return structured initialization errors, keep a minimal diagnostics console alive, and expose retry/remount/reprobe commands where safe.

Software verification: mocked GPIO/SPI/mount/task failures with expected degraded states and no abort.

Hardware validation: radio absent, CSN/MISO disconnected, invalid SPI resource, blank/corrupt SPIFFS.

### F-BUILD-001 — Builds are machine-specific and dependency resolution is floating

| Field | Value |
|---|---|
| Severity | Medium |
| Confidence | Confirmed |
| Path / location | platformio.ini:18, 25-35, 57-87; .vscode/settings.json; README.md:38-56; generated sdkconfig files |
| Category | Build/configuration defect |
| Dependencies | Supported toolchain selection |

Current behavior and evidence: espressif32/native versions are unpinned; upload ports are COM5/COM3; README commands embed C:\Users\dman2; editor config points to IDF 5.5.3 while clean PlatformIO resolved 6.0.1. Four large generated full configs and an installer log are tracked.

Why it matters / likely failure: two students can compile different frameworks/configurations; new dependency releases can break builds, as the devboard profile demonstrates.

Recommended correction: pin platform/tool versions, use portable port overrides, retain minimal defaults only, document one setup command, and add clean CI builds.

Software verification: clean Windows and Linux CI/container builds from a fresh checkout with identical resolved versions and configuration manifest.

Hardware validation: upload/monitor port discovery and manual-reset workflow on each development machine.

### F-TEST-001 — Tests omit the active file-transfer path and one defined test is not registered

| Field | Value |
|---|---|
| Severity | Low |
| Confidence | Confirmed |
| Path / location | platformio.ini:74-87; test/test_nrf24/test_main.cpp:1247-1263 and 1492-1571; src/main.cpp |
| Category | Confirmed test defect/coverage gap |
| Dependencies | Extracted transfer/session seam and host compiler |

Current behavior and evidence: 77 test_ functions are defined but only 76 RUN_TEST calls exist; test_streamSync_remote_response_round_trip is omitted. main.cpp is not compiled by native tests, so actual sendDataFile boundary logic, SPIFFS reassembly, naming, timeout, and completion behavior are not tested. The existing gap test enshrines corrupt-success behavior.

Why it matters / likely failure: protocol helpers can pass while the active application fails common file sizes and integrity requirements.

Recommended correction: register all intended tests and extract minimally sized, hardware-free segmentation/session/file-sink logic for host tests. Replace the gap-success expectation.

Software verification: test enumeration check, boundary matrix, session state table, fault injection, and byte-for-byte reconstructed fixtures.

Hardware validation: none for unit coverage; follow with two-board tests.

## 7. Probable and possible risks

### R-RF-001 — RX FIFO may overflow under scheduling, Wi-Fi, or logging load

| Field | Value |
|---|---|
| Severity | High |
| Confidence | Probable |
| Path / location | src/main.cpp:90-93, 3005-3069; include/rx_drain.hpp; nRF24 three-packet FIFO |
| Current behavior | Poll every 2 ms and drain at most eight; no IRQ ISR or explicit overflow indication/recovery |
| Evidence | Active task design plus small hardware FIFO; no measured scheduler worst case |
| Why / likely failure | A delayed RX task can lose frames; current gap policy can publish corruption |
| Recommended correction | Measure service latency, use IRQ/task notification or tighter bounded drain, instrument FIFO/sequence loss, and rely on session retransmission |
| Software test | Fake scheduler/backlog bursts and guard-exhaustion tests |
| Hardware validation | Required with Wi-Fi on/off, logging, maximum throughput, and long runs |
| Dependencies | F-RX-001 and F-PROTO-001 |

### R-RF-002 — Clone-specific CE/rearm fallback depends on undocumented timing

| Field | Value |
|---|---|
| Severity | Medium |
| Confidence | Probable |
| Path / location | src/nrf24.cpp:280-431 transmitOnce |
| Current behavior | 150 us pre-CE delay, configurable 40 us pulse, then full re-prime and CE-held-high fallback |
| Evidence | Comments cite clone modules and long-wire setups; no hardware qualification data |
| Why / likely failure | Different modules may behave inconsistently; rearm flushes RX and may hide root causes |
| Recommended correction | Document supported module variants, base timing on datasheet states, instrument which path succeeds, and isolate workarounds behind a quirk flag |
| Software test | Register/timing trace tests for normal and fallback state restoration |
| Hardware validation | Required across all actual radio modules and supply conditions |
| Dependencies | Stable driver config |

### R-FS-001 — Close/power-loss errors can publish an incompletely persisted file

| Field | Value |
|---|---|
| Severity | Medium |
| Confidence | Probable |
| Path / location | src/main.cpp:1370-1375 and 1433-1472 |
| Current behavior | fclose return is ignored; no fsync; final path is removed before rename |
| Evidence | Direct code inspection; actual SPIFFS failure semantics require runtime validation |
| Why / likely failure | Full flash, media error, or power loss around close/rename may lose the old file or publish incomplete data |
| Recommended correction | Check close/flush results where supported, preserve old final until verified atomic replacement, and add a recovery journal/state marker |
| Software test | Fault-injected sink at write/close/rename boundaries |
| Hardware validation | Required with near-full SPIFFS and controlled power cuts |
| Dependencies | File publication policy and integrity verification |

### R-PCB-001 — Boot/reset/upload behavior may remain hardware-dependent

| Field | Value |
|---|---|
| Severity | Medium |
| Confidence | Possible |
| Path / location | platformio.ini:12-14, 25-33, 49-55; project hardware context |
| Current behavior | Manual-boot default, fixed COM5, DTR/RTS disabled, no-reset before upload |
| Evidence | Repository comments and reported prior EN/GPIO0 wiring issue; no schematic/board present |
| Why / likely failure | Integration/flashing may require button manipulation or a specific USB-UART timing sequence |
| Recommended correction | Validate and document reset/boot truth table; correct schematic/BOM/PCB if necessary; make upload profile explicit per revision |
| Software test | Upload script/profile lint only |
| Hardware validation | Required on every PCB revision with CP210x |
| Dependencies | PCB schematic and board revision |

### R-RF-003 — RF power, PA/LNA supply, antenna, and range are unverified

| Field | Value |
|---|---|
| Severity | High |
| Confidence | Possible |
| Path / location | src/nrf24.cpp RF_SETUP=0x26; project hardware context |
| Current behavior | Genuine-chip power bits are at level 3 and 250 kbps; external module PA/LNA behavior is outside firmware |
| Evidence | No schematic, supply measurements, conducted power data, antenna data, or range logs in repository |
| Why / likely failure | PA current transients, 3.3 V regulation/decoupling, antenna mismatch, interference, or regulatory limits can dominate reliability |
| Recommended correction | Define power/antenna test plan and regulatory operating limits; measure conducted/radiated behavior and packet error rate |
| Software test | Configuration manifest only |
| Hardware validation | Required; include supply droop, current, EIRP/range, orientation, obstruction, and coexistence |
| Dependencies | Confirmed hardware BOM and target jurisdiction |

### R-HW-001 — The actual PCB net mapping cannot be inferred from repository source

| Field | Value |
|---|---|
| Severity | High |
| Confidence | Possible |
| Path / location | platformio.ini, README.md, project stated intended mapping |
| Current behavior | Three conflicting maps exist; firmware logs its active one |
| Evidence | No schematic/netlist/layout available |
| Why / likely failure | Changing firmware to the stated map without checking PCB revision could fix one board and break another |
| Recommended correction | Treat schematic/netlist/continuity as authoritative and version pin manifests by board revision |
| Software test | Per-environment compile-time pin manifest |
| Hardware validation | Required before F-CFG-001 correction |
| Dependencies | User-provided PCB evidence |

### R-OPS-001 — Long-duration and repeated-transfer stability is unknown

| Field | Value |
|---|---|
| Severity | Medium |
| Confidence | Possible |
| Path / location | src/main.cpp task loops, dynamic std::string/vector usage, SPIFFS lifecycle |
| Current behavior | Permanent tasks and repeated command loops exist; no soak test or resource telemetry |
| Evidence | No automated long-duration test/log in repository |
| Why / likely failure | Rare scheduler, heap-fragmentation, file-handle, counter-wrap, or recovery problems may appear only after many cycles |
| Recommended correction | Add heap high-water, task-stack, file-handle, retry/error counters and repeatable soak scripts |
| Software test | Thousands of simulated sessions and command cycles |
| Hardware validation | 8-24 hour two-board soak with periodic hashes and forced faults |
| Dependencies | Reliable protocol and diagnostics |

## 8. Obsolete, duplicate, and conflicting code

### Board/build remnants

- boards/featheresp32_n16.json and sdkconfig.featheresp32 are inactive Feather remnants.
- sdkconfig.esp32wroom32d_devboard and sdkconfig.esp32wroom32d_devboard_manual_boot use old naming and are byte-identical.
- sdkconfig.devboard is active for a declared profile but stale/incompatible.
- InstallationLog.txt and .scanoss/scanoss-raw.json are generated developer-machine artifacts.
- .vscode/settings.json and README contain machine-local paths/ports/version assumptions.

### Audio-only assumptions

- Namespace/class/fields remain AudioPacket, AudioReassembler, audio_len, audio(), and kAudioBytesPerPacket.
- The transport rate is still hard-wired to unsigned 8-bit 8 kHz audio through kAudioByteUs=125 and compile-time assertions.
- platformio_audio_targets.py and prepare_demo_audio.py preserve legacy names, although the active staging implementation is generic.
- Partition and .gitattributes comments still describe an audio workflow.
- The two .u8 files are valid supported payloads but should become only fixtures/application examples.

### Duplicate/inactive implementations

- DemoConsoleApp implements active streaming-to-SPIFFS reconstruction.
- AudioReassembler separately implements in-memory reconstruction and the same tolerated-gap policy, is compiled into firmware, but is not called.
- frame_io is native-test-only despite living in src.
- ReceiverGate.pending_stream_id_ is reset/read but never assigned nonzero; its pending-stream branches are dead.
- Remote response encode/decode is not integrated; its test is not registered.
- RF REMOTE command is present but disabled by all current PlatformIO environment flags.

### Documentation conflicts

- Intended pins, README “devboard” pins, and active compiled pins disagree.
- data/README.md says .part files are hidden, but code lists them.
- README omits POWER, REMOTE, /power, and /command and embeds one developer’s PlatformIO path.
- platformio.ini comments say every .u8 file is packed; buildfs actually packs all regular files, which is desirable for generic transfer.
- The initial default payload.bin is not shipped; README.md becomes the first selected file in the built image.

### Abandoned high-power PA work

No active code, build target, device driver, schematic, or document for a separate approximately 5 W PA was found. Active CW is an nRF24 diagnostic and active RF power control only changes nRF24 RF_SETUP bits. The selected PA+LNA module remains hardware context; a separate high-power PA is not part of the current repository design.

## 9. Build and test results

| Activity | Result | Evidence/notes |
|---|---|---|
| Complete source inventory | Success | 55 non-Git files classified; binary hashes/sizes inspected |
| Relevant old branch comparison | Success | All accessible non-main references point to 9ef4cfb; current delta classified |
| Primary clean | Success | esp32wroom32d_manual_boot |
| Primary firmware build | Success | ESP-IDF 6.0.1; RAM 4.3%, app flash 15.4% |
| Primary build warnings | No meaningful application warning | Tool output included normal Git-version absence in archive build |
| SPIFFS build | Success | 15,138,816-byte image with all three data entries |
| Devboard firmware build | Failed | Stale full sdkconfig causes ESP-IDF kconfgen crash before compilation |
| Native test discovery | Success | One suite; 77 functions, 76 registered |
| Native test execution | Not possible | No host C/C++ compiler; zero cases executed |
| Hardware tests | Not possible | No boards/radios/schematic supplied through repository |
| CI workflow | None found | No automated clean build/test |

Tests present cover nRF24 register/FIFO behavior through FakeHal, packet encode/decode shapes, AudioReassembler behavior, retries, audio pacing, RX draining, FrameIO, validation, Morse/CW, and ReceiverGate transitions.

Important missing tests:

- Actual file segmentation in sendDataFile.
- Empty/exact-multiple/rollover sizes.
- Active SPIFFS reassembly/publication.
- Full 0x00/0xFF/every-byte files.
- Transfer ID isolation and back-to-back sessions.
- Timeout/cancel/restart.
- End-to-end hash/completion.
- Filesystem-full/close/rename faults.
- Two-board or loopback automation.
- Build matrix/CI.

The smallest useful host strategy is the existing Unity/FakeHal approach plus one extracted, platform-neutral transfer-session core using byte-source/byte-sink interfaces and a fake clock. No new heavyweight framework is needed.

## 10. Integration-readiness assessment

| Category | Rating | Explanation |
|---|---|---|
| Interface clarity | Needs work | Serial/file behavior can be reconstructed, but no stable subsystem API or machine-readable completion contract exists; docs/pins conflict |
| Reliability | Not ready | Missing packets can publish corruption; no peer acknowledgment/retry/integrity |
| Recoverability | Not ready | No peer cancel/timeout; reset epochs and stale partials are unmanaged |
| Diagnostics | Needs work | Useful local status exists, but it reports local radio state rather than delivery and lacks reasoned session outcomes |
| Binary safety | Needs work | Per-chunk bytes are safe, but size boundaries, rollover, and loss violate whole-file fidelity |
| Repeatability | Not ready | Stream IDs reset and overwrite; devboard build fails; dependencies and ports float |
| Maintainability | Needs work | Monolithic main, duplicate reassembly policy, legacy naming/configs, and no active-path tests |
| Hardware dependencies | Cannot determine | PCB mapping, reset circuit, PA supply, antenna, RF range, and IRQ performance require hardware evidence |

Current upstream interface:

- A PC/user stages a regular file into data and uploads SPIFFS.
- At runtime another actor issues FILES/SELECT/TX by serial, or optional HTTP.
- Generated binary data from another embedded subsystem has no direct API; it must first become a SPIFFS file or be added through new code.

Current downstream interface:

- The receiver creates rx_####.bin in SPIFFS.
- Another subsystem must poll/list/open it and cannot trust success without an out-of-band byte/hash comparison.
- There is no data-valid GPIO, event queue, callback, stream reader, completion object, or error code interface.

Required operational assumptions:

- Matching firmware protocol/address/channel/pins.
- Receiver explicitly enters RX before sender begins.
- Correct SPIFFS image is uploaded.
- File fits both SPIFFS and the undocumented 16-bit sequence limit.
- User chooses/accepts generated filenames.
- Manual-boot/reset behavior may be needed for upload.
- At least size / 8,000 seconds plus overhead is available.

An integration partner should not be required to understand nRF24 register details, but today it must understand serial commands, SPIFFS staging, timing, receiver-first ordering, generated names, and ambiguous local status. That is not an integration-ready abstraction.

## 11. Dependency-ordered remediation backlog

### Phase 1: Reproducible build and active-code identification

| Task | Title | Findings addressed | Files involved | Dependencies | Expected result / verification | Hardware | Complexity | Risk to demonstrated behavior |
|---|---|---|---|---|---|---|---|---|
| B1-01 | Pin a passing build/toolchain matrix and minimize sdkconfig | F-CFG-002, F-BUILD-001 | platformio.ini, board JSON, sdkconfig defaults/generated files, README, CI | Supported ESP-IDF choice | Fresh primary/devboard build+buildfs resolve identical versions; CI passes | No; flash smoke later | Medium | Low |
| B1-02 | Establish authoritative board-revision pin manifests | F-CFG-001, R-HW-001 | platformio.ini, HAL config, docs | Schematic/netlist/continuity | Each env prints and compiles one reviewed map; SPI probe succeeds | Yes to confirm | Small | High if guessed; low after confirmation |
| B1-03 | Remove generated/local artifacts and secrets from active configuration | F-SEC-001, F-BUILD-001 | sdkconfigs, InstallationLog, .scanoss, .vscode, Wi-Fi config | B1-01; credential rotation | Clean secret scan and portable setup | Network smoke only | Small | Low |

### Phase 2: Protocol definition and binary safety

| Task | Title | Findings addressed | Files involved | Dependencies | Expected result / verification | Hardware | Complexity | Risk |
|---|---|---|---|---|---|---|---|---|
| B2-01 | Write a versioned generic wire-protocol specification | F-PROTO-001, F-SESSION-001, F-PROTO-002 | New protocol doc, packet headers/source | B1-01; decide backward compatibility and max size | Byte layouts, endian, states, limits, ACK/NACK/END/CANCEL/error defined | No | Medium | Medium; on-air compatibility decision |
| B2-02 | Implement strict portable serialization/deserialization | F-SESSION-001 | audio_packet/stream_sync replacement or incremental extension | B2-01 | Golden byte vectors; reject unknown flags/reserved fields/wrong IDs | No | Medium | Medium |
| B2-03 | Make segmentation size-correct and generic | F-TX-001, F-PROTO-002, F-ARCH-001 | Extracted segmenter, main integration, staging limits | B2-01/B2-02 | Boundary and max-size matrix passes; optional rate policy outside core | No | Medium | Medium |

### Phase 3: Transfer state and reliability

| Task | Title | Findings addressed | Files involved | Dependencies | Expected result / verification | Hardware | Complexity | Risk |
|---|---|---|---|---|---|---|---|---|
| B3-01 | Implement explicit peer session and readiness | F-SESSION-001 | Session layer, main orchestration | B2 | Every data frame bound to transfer; legal transition table enforced | Later | Large | High; coordinate both peers |
| B3-02 | Add loss recovery with bounded ACK/NACK/window retry | F-PROTO-001, F-RX-001, R-RF-001 | Session layer, radio orchestration | B3-01 | Drop/duplicate/reorder simulations converge or fail explicitly | Later | Large | High |
| B3-03 | Add verified END/completion using size and hash/CRC | F-RX-001, F-PROTO-001 | Session, sink, status API | B3-01/B3-02 | Sender success only after receiver byte count and digest match | Later | Medium | Medium |
| B3-04 | Add cancellation, timeout, and reset recovery | F-STATE-001 | Session, fake clock, boot cleanup | B3-01 | Fault-injection matrix leaves both peers idle and no valid partial | Later | Medium | Medium |

### Phase 4: Error recovery and diagnostics

| Task | Title | Findings addressed | Files involved | Dependencies | Expected result / verification | Hardware | Complexity | Risk |
|---|---|---|---|---|---|---|---|---|
| B4-01 | Make received-file publication atomic and collision-safe | F-FS-001, F-FS-002, R-FS-001 | File sink/naming, main | B3-03 | No overwrite; partials hidden/quarantined; close/rename faults explicit | Power-cut tests later | Medium | Medium |
| B4-02 | Replace volatile stop and structure initialization errors | F-CONC-001, F-INIT-001 | main, HAL interfaces | B1 and session seam | Event-driven cancellation; degraded diagnostics shell survives faults | Some later | Medium | Low-medium |
| B4-03 | Expose stable machine-readable transfer results/counters | Integration gaps | Command/API/status layer | B3/B4-01 | Transfer ID, state, bytes, retries, error, digest and peer completion available | No | Medium | Low |
| B4-04 | Fix staging replacement and enforce actual filesystem/size budgets | F-TOOL-001, F-PROTO-002 | tools/stage_demo_file.py | B2 limits, B1 configs | Validated tree equals built tree; oversize rejected early | No | Small | Low |

### Phase 5: Automated software tests

| Task | Title | Findings addressed | Files involved | Dependencies | Expected result / verification | Hardware | Complexity | Risk |
|---|---|---|---|---|---|---|---|---|
| B5-01 | Make native tests reproducible and register every test | F-TEST-001 | platformio native env, tests, CI | B1-01 | Compiler documented/provisioned; all registered tests run | No | Small | Low |
| B5-02 | Add host transfer/session and fault-injection suite | All protocol/state findings | Extracted core, FakeHal/fake sink/clock | B2-B4 seams | Required validation matrix deterministic in CI | No | Medium | Low |
| B5-03 | Add build, secret, config, and source-list CI checks | Build/security/duplicate findings | CI, scripts | B1 | Both env builds, buildfs, tests, secret scan, generated-file guard | No | Small | Low |

### Phase 6: Hardware bench validation

| Task | Title | Findings addressed | Files involved | Dependencies | Expected result / verification | Hardware | Complexity | Risk |
|---|---|---|---|---|---|---|---|---|
| B6-01 | Validate boot, pins, SPI, IRQ, and radio state transitions | F-CFG-001, F-INIT-001, R-HW-001, R-PCB-001 | Test procedure/logs | B1/B4 | Repeatable flash/boot/probe/RX/TX on each revision | Two boards/radios | Medium | Low |
| B6-02 | Execute loss/restart/power-cut transfer matrix | Protocol/filesystem findings | Bench harness/logs | B3-B5 | Every success hash-matches; every failure cleans up | Two boards, controlled power/RF | Large | Medium |
| B6-03 | Measure throughput, FIFO service, soak, and RF behavior | R-RF-001/002/003, R-OPS-001 | Instrumentation/logs | Reliable protocol | Quantified rates/PER/range/supply margin and long-run pass criteria | RF/power equipment helpful | Large | Low |

### Phase 7: Subsystem integration

| Task | Title | Findings addressed | Files involved | Dependencies | Expected result / verification | Hardware | Complexity | Risk |
|---|---|---|---|---|---|---|---|---|
| B7-01 | Define upstream/downstream subsystem API | Integration assessment | Generic source/sink/service API | B2 protocol; partner requirements | Producer submits bytes/file; consumer receives verified artifact/event without nRF24 knowledge | No | Medium | Medium |
| B7-02 | Integrate data-valid/completion/error contract | Integration assessment | Queue/callback/status or agreed IPC | B4-03/B7-01 | End-to-end subsystem acceptance tests | Full subsystem | Medium | Medium |
| B7-03 | Harden optional network/remote controls | F-SEC-001 | HTTP/RF command layer | Threat model/B7-01 | Disabled-by-default or authenticated/minimal controls | Network hardware | Medium | Low |

### Phase 8: Optional audio functionality and cleanup

| Task | Title | Findings addressed | Files involved | Dependencies | Expected result / verification | Hardware | Complexity | Risk |
|---|---|---|---|---|---|---|---|---|
| B8-01 | Move audio rate/format behavior into an optional handler | F-ARCH-001 | tx_helpers, application layer, docs | Generic transport complete | .u8 transfers as ordinary file; optional playback/pacing separately tested | Audio output if playback needed | Medium | Medium |
| B8-02 | Rename generic packet fields and remove duplicate/dead code | F-TEST-001 and section 8 | audio_packet/reassembler, ReceiverGate, scripts/comments | Compatibility migration complete | One reassembly policy and accurate names/source lists | No | Medium | Medium |
| B8-03 | Retain Morse/CW as isolated diagnostics | Cleanup | morse/CW modules and command docs | Stable driver/API | Diagnostics do not share transfer state or imply external PA | RF bench | Small | Low |

## 12. Validation plan

Every successful file-transfer test must compare source and received length and bytes. On a development computer, record SHA-256 for both. “TX completed” without a peer digest match is not a pass.

### 12.1 Host-automated tests

Use Unity plus the existing FakeHal, a fake byte source/sink, fake clock, and paired session endpoints.

| Test | Required assertion |
|---|---|
| Clean build | Pinned primary/devboard configs and buildfs succeed from empty caches |
| Empty file | Explicit successful zero-byte artifact or documented rejection on both peers; never silent |
| One-byte file | One correct final data representation and exact hash |
| Smaller than payload | 1-27-byte matrix reconstructs exactly |
| Exactly one payload | 28 bytes completes exactly |
| One byte larger | 29 bytes creates correct full+short segmentation |
| Exactly two payloads | 56 bytes completes exactly |
| Large file | Streamed without whole-file RAM buffering; digest exact |
| Only 0x00 | Exact length/hash, no sentinel behavior |
| Only 0xFF | Exact length/hash |
| Every byte value | Repeated 0x00-0xFF fixture exact |
| Random binary | Seeded sizes around every boundary and digest exact |
| Text/no newline/embedded newlines | Content preserved; no line parsing |
| .u8 audio | Treated identically to binary fixture |
| Partial read | Continue correctly or fail explicitly; no premature LAST |
| Partial write | Retry per sink contract or fail/clean; no final publication |
| Missing packet | Retransmit or fail; never publish corrupt file |
| Duplicate packet | Exactly-once output |
| Out-of-order packet | Reorder within defined window or request retry/fail |
| Corrupted frame/file | Per-frame validation and final digest reject |
| Interrupted transfer | Timeout/cancel resets both ends and cleans partial |
| Receiver restart | Sender receives failure/re-handshakes; no mixed output |
| Transmitter restart | Receiver times out old epoch; new ID cannot overwrite/mix |
| Back-to-back transfers | Distinct artifacts/session results, no delayed-frame mixing |
| Same filename repeatedly | Explicit collision policy; no silent overwrite |
| Sequence rollover | Wider field/window or enforced rejection at exact boundary |
| Maximum size | Maximum passes; maximum+1 is rejected before TX |
| Filesystem full | Explicit failed result and preserved prior files |
| Radio unavailable | Diagnostics state, no false queued/delivered result |
| Invalid serial command | Stable error, no state mutation |
| Command during active transfer | Defined busy/cancel/queued behavior with no races |
| Repeated TX/RX cycles | Thousands of simulated cycles, stable counters/resources |
| Long duration | Counter/timer wrap and soak simulation |

### 12.2 ESP32 tests without radio

- Boot with valid, blank, missing, corrupt, and near-full SPIFFS.
- Verify FILES excludes internal partials and lists arbitrary extensions.
- Stage/open/read boundary fixtures and compare ESP32-computed SHA-256.
- Exercise serial CR, LF, CRLF, case, repeated whitespace, maximum line, overflow, invalid arguments, and filename case.
- Inject a fake/loopback radio HAL if practical; verify initialization errors do not abort the diagnostics shell.
- Record task stack high-water marks, heap before/after cycles, file-handle cleanup, and watchdog status.

### 12.3 Two-board bench tests

- Confirm continuity and runtime pin manifest before RF tests.
- Receiver starts before and after transmitter; expected handshake behavior must be explicit.
- Transfer every fixture above and retrieve both files for SHA-256.
- Force one missing, duplicate, delayed, and corrupted frame with a debug fault-injection mode.
- STOP at multiple offsets; power-cycle each peer at multiple offsets.
- Wrong channel, radio absent, antenna disconnected only where electrically safe, and receiver FIFO stress.
- Back-to-back/repeated transfers across reboots.
- Near-full filesystem, close/rename failure where injectable, and controlled power loss.
- Repeat TX/RX cycles and at least an 8-24 hour soak.

### 12.4 RF range tests

- Measure packet error/retry rate and application completion rate, not merely carrier detect.
- Test selected channels in the actual Wi-Fi environment, orientations, enclosures, obstruction, and intended range.
- Measure 3.3 V rail droop/current during PA bursts and validate regulator/decoupling.
- Record module identity, antenna, channel, rate, power bits, firmware revision, distance, and environment.
- Check applicable 2.4 GHz regulatory/EIRP constraints before using PA+LNA/CW modes.

### 12.5 Full subsystem-integration tests

- Producer submits generated binary without PC staging.
- Receiver consumer gets a transfer ID, verified length/hash, atomic data-valid event, and durable error.
- Define backpressure, maximum size, latency/throughput, retry budget, cancellation ownership, and startup order.
- Run concurrent partner workload while measuring radio task latency.
- Demonstrate that neither partner needs nRF24 register knowledge or manually parsed console text.
- Repeat with generic binary, text, and .u8 audio payloads.

## 13. Recommended architecture

Incremental correction is preferable to a ground-up rewrite. The HAL, much of the nRF24 driver, FakeHal, Morse encoder, and staging concept are reusable. The key change is to extract transfer responsibility from DemoConsoleApp and put a real tested session between packet serialization and file I/O.

~~~mermaid
flowchart TB
    A["5. Application handlers: subsystem data, optional audio, Morse/CW diagnostics"]
    B["4. Generic source/sink: stream or file, atomic destination, hash"]
    C["3. Reliable session: transfer ID, size, sequence/window, ACK/NACK, timeout, cancel, END/complete"]
    D["2. Strict packet codec: versioned byte layouts and validation"]
    E["1. nRF24 driver/HAL: SPI, FIFO, IRQ, modes, link-level retry"]
    A --> B --> C --> D --> E
~~~

1. nRF24 hardware/driver layer
   - Keep register/FIFO operations and board HAL independent.
   - Add a clear local-send result and measured IRQ service path.
   - Hardware auto-ack may reduce loss but is not the file protocol.

2. Packet serialization layer
   - Use explicit byte arrays and byte order as today.
   - Add common version/type and transfer ID to every frame.
   - Strictly validate type, flags, reserved bytes, sizes, and IDs.
   - Define START/METADATA, DATA, ACK/NACK, END, COMPLETE, CANCEL, and ERROR within the 32-byte constraint.

3. Reliable transfer/session layer
   - Own legal states, receiver readiness, window/sequence arithmetic, retries, timers, cancel, reset epochs, completion, and final digest.
   - Stream data rather than retaining a whole file in RAM.
   - Expose a deterministic result object to commands and partner subsystems.

4. Generic file source/destination layer
   - Provide read(offset/span) and write(offset/span) or sequential stream interfaces.
   - Own declared size, storage budget, hidden partial, atomic publish, collision policy, cleanup, and SHA-256/CRC.
   - Permit a non-file producer/consumer without teaching it nRF24 details.

5. Application-specific handlers
   - Serial/HTTP/subsystem adapters submit transfers and consume results.
   - Audio may select .u8 playback metadata or an optional rate policy, but file transport remains format-agnostic.
   - Morse/CW should invoke driver diagnostics and never share file-session state.

This design does not require transmitting filenames if the integration prefers application-assigned object IDs. It does require a stable transfer identifier, declared length, completion proof, and destination policy. Backward compatibility with the current wire format should be an explicit decision; silently accepting both formats in the same state machine is unsafe.

## 14. Ten highest-priority actions

1. Establish a version-pinned build and board/pin contract.
   - Why now: every later code/protocol result must build deterministically and target the actual nets.
   - Prerequisite: PCB revision/netlist answer for CSN/IRQ; supported ESP-IDF choice.
   - Completion: primary and devboard clean build/buildfs pass in CI with reviewed pin manifests.
   - Hardware: required only to confirm pins and final flash/boot.

2. Make the existing host-test environment reproducible and capture characterization tests.
   - Why now: protects demonstrated driver behavior before protocol changes.
   - Prerequisite: action 1.
   - Completion: compiler provisioned/documented, all intended tests registered, current packet vectors recorded.
   - Hardware: no.

3. Approve a versioned generic transfer protocol and compatibility policy.
   - Why now: boundary, session, retry, and completion fixes depend on one wire contract.
   - Prerequisite: actions 1-2; answer whether old firmware interoperability is required.
   - Completion: exact layouts, byte order, size limit, states, ACK/NACK, END/COMPLETE, CANCEL/ERROR documented and tested as golden vectors.
   - Hardware: no.

4. Implement/test correct segmentation, empty files, exact multiples, and size enforcement.
   - Why now: removes deterministic generic-file failures before RF tuning.
   - Prerequisite: action 3.
   - Completion: 0/1/27/28/29/56/57/max matrix byte-matches in host tests.
   - Hardware: no.

5. Bind every frame to a unique transfer and enforce the session state machine.
   - Why now: prevents cross-transfer/reset mixing before adding recovery.
   - Prerequisite: actions 3-4.
   - Completion: interleaved/back-to-back/restart simulations remain isolated.
   - Hardware: later bench confirmation.

6. Add receiver readiness and bounded loss recovery.
   - Why now: local no-ack TX cannot support reliable delivery.
   - Prerequisite: action 5.
   - Completion: deterministic drop/duplicate/reorder tests either recover or report explicit failure with no final file.
   - Hardware: two-board validation required.

7. Add declared size, final digest, verified COMPLETE, and atomic collision-safe publication.
   - Why now: success must mean byte-for-byte delivery.
   - Prerequisite: actions 5-6.
   - Completion: sender reports success only after peer digest; prior files survive failed replacements.
   - Hardware: two-board and power-cut validation.

8. Add cancel, timeout, reset recovery, and synchronized task signaling.
   - Why now: reliable happy-path states must exist before failure cleanup is finalized.
   - Prerequisite: actions 5-7.
   - Completion: STOP/power/reset/link-loss matrix returns both peers to known state and removes/quarantines partials.
   - Hardware: required after simulation.

9. Expose a stable subsystem API and harden/disable auxiliary controls.
   - Why now: integration should consume verified transfer results, not console strings; unauthenticated controls must not ship by accident.
   - Prerequisite: actions 7-8.
   - Completion: producer/consumer API, data-valid/error/completion schema, credential removal, and authenticated or disabled HTTP/RF control.
   - Hardware: full subsystem/network test.

10. Run the two-board, RF, power, range, and long-duration validation plan; then isolate optional audio cleanup.
    - Why now: software correctness must precede claims about RF reliability and performance.
    - Prerequisite: actions 1-9.
    - Completion: every successful fixture SHA-256 matches, fault matrix passes, measured operating envelope is documented, and .u8 remains an ordinary payload.
    - Hardware: yes.

### Open questions requiring owner/hardware input

1. Which PCB revision is current, and what do its schematic/netlist/continuity checks show for CE, CSN, IRQ, EN, and GPIO0?
2. Must new firmware interoperate over the air with the current 32-byte format, or may both boards move together to a new protocol?
3. What maximum file size, acceptable transfer time, range, retry budget, and failure probability are required?
4. Should the downstream integration deliver a SPIFFS file, byte stream, queue/callback, or another subsystem-specific object?
5. Is overwriting a same logical destination ever allowed, and who owns destination naming?
6. Are Wi-Fi/HTTP and RF remote commands required for the final system? The committed credential should be treated as exposed.
7. Can the PCB schematic/BOM and nRF24 module/antenna/power-decoupling details be provided?
8. Are two assembled boards and a repeatable way to retrieve received files available for bench automation?

### Final audit checks

- Critical/High findings were re-read against active code and build evidence.
- Runtime/hardware-dependent effects are separated into probable/possible risks.
- Active defects are separated from historical branches, legacy Feather/audio names, and generated files.
- Recommendations prioritize generic file fidelity/session reliability; audio is retained as an optional ordinary payload/application.
- No production code was modified.
- Best first task: B1-01, the pinned passing build and board/pin contract.
