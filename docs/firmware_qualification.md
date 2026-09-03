# RF3 software-only firmware qualification

The physical RF link is **not working/validated yet**. This campaign provides
repeatable engineering evidence about the hardware-independent firmware transfer
path, not evidence of RF operation. Existing Unity/FakeHal and Python tests remain
separate and unchanged in purpose.

## Run and retain evidence

From the repository root, with Python 3.10+ and GCC/Clang supporting C++17:

```sh
python tools/run_firmware_qualification.py
```

With the repository's local Windows virtual environment, the equivalent command
without activating it is `.\.venv\Scripts\python.exe tools\run_firmware_qualification.py`.

The runner compiles a dedicated native executable with `-std=c++17 -O2 -Wall
-Wextra -Werror -pedantic`, executes it, independently checks final files using
Python's `zlib.crc32`, and prints category and overall PASS/FAIL. Set `CXX` to a
compiler executable or pass `--cxx clang++` / `--cxx /path/to/g++`. `CXX` is an
executable, not a shell command containing flags. No extra Python packages are
needed for qualification; PlatformIO is needed for the separate regression and
firmware matrix below. The default source is tracked `data/song.u8`; `--fixture`
can select another file, but the 1,112,701-byte baseline is still enforced.

Every invocation uses a new ignored `.pio/qualification/run-*` directory:

| Artifact | Evidence |
|---|---|
| `report.txt` | Concise presentation-friendly results; explicit software-only scope |
| `report.json` | Results plus host/compiler/Python versions, Git revision and dirty status, input SHA-256 hashes, compile command, and independent checks |
| `transfers.jsonl` | One measured record per executed transfer, including every configured fault and its actual application count |
| `build.log`, `execution.log` | Compiler and executable diagnostics |
| Category directories / `rx_<id>.bin` | Actual host files published by successful receiver sessions |

No failed transfer may leave a final or `.part` file. Successful final files are
kept for inspection. Runs are not automatically deleted or overwritten. Allow
roughly 25 MB plus compiler/evidence files per successful run. The runner returns
0 only if all expected categories and all 264 transfer records pass their checks;
compile errors, missing fixtures, missing records, timeouts, failed assertions,
and independent verification failures return nonzero. It does not use C/C++
`assert`, so qualification checks are not removed by `NDEBUG`.

## Actual path and observation boundary

```text
file/generated streaming callbacks
  -> FileTransfer::StreamingDataSource / Service::startTransfer
  -> production inspectSource (size, packet count, CRC32)
  -> production SenderSession (20-byte packetization, encode, retry/ACK state)
  -> existing FakeDuplexTransport (deterministic frame faults)
  -> production ReceiverSession (decode, sequence, size, CRC, publication gate)
  -> host SinkCallbacks (write .part, close/read back, rename or remove)
  -> streamed comparison of published file + independent Python CRC
```

The executable includes production `file_transfer_service.hpp`,
`reliable_transfer_v2.hpp`, and `protocol_v2.hpp`. It does not implement a second
protocol or segmenter: the active V2 sender performs segmentation itself. The
legacy `file_segmenter.hpp` / `file_receiver.hpp` implementations are not the
active V2 path and remain covered by existing unit tests. The only extension to
the existing fake transport is read-only per-rule application instrumentation.

Host source/sink callbacks are test adapters at the same interfaces used by the
firmware. They are **not the ESP-IDF adapter in `src/main.cpp`**. The sink performs
real host file operations and observes the receiver's publication callback state,
accepted size/count and calculated CRC, and the closed file's byte equality/CRC.
Crucially, it does not refuse a publication because its independent oracle finds
bad data: that would mask a defective firmware CRC gate. Instead it records an
invalid publication and fails qualification. `.part` removal is performed by the
production receiver invoking the sink cleanup callback, not by test teardown.

Every transfer checks acknowledged bytes/packets, accepted bytes/packets, observed
accept events and writes, decoded DATA sequence/length, service status, terminal
states, retry/drop accounting, one publication for success, and no publication
for corruption. Wire counts include retries; accepted counts exclude duplicates.
There is no full-file heap buffer: source inspection uses the production
128-byte buffer; disk comparison uses two 4,096-byte buffers; the existing fake
transport has its fixed 256-frame queue. Host disk files are retained, not RAM
duplicates of the large inputs.

## Deterministic campaign specification

All runs use production defaults: 32-byte frames, 20 DATA bytes, 500 ms control
response timeout, 250 ms DATA ACK timeout, five retries per exchange, 10,000 ms
sender/receiver inactivity timeouts. A step drains sender-to-receiver frames,
then receiver-to-sender frames, then ticks both sessions. Simulated monotonic time
advances by 10 ms per step. There is a 300,000-step bound per transfer, 120-second
host compilation timeout, and 180-second host executable timeout. Simulated time
does not represent RF throughput, processor timing, or real scheduling.

The base seed is `0x52463332`. The fault schedule PRNG is xorshift32, in order:
`x ^= x << 13; x ^= x >> 17; x ^= x << 5`, using unsigned 32-bit arithmetic.
The generated byte at address `i` with seed `s` is the low byte of:

```text
x = (i XOR s) + 0x9E3779B9
x = (x XOR (x >> 16)) * 0x85EBCA6B
x = (x XOR (x >> 13)) * 0xC2B2AE35
x = x XOR (x >> 16)
```

All operations wrap modulo 2^32. Unlike a short repeating ramp, the address- and
seed-dependent pattern makes packet offsets and cross-transfer contamination
observable. This pattern is an integrity oracle, not cryptography.

### Large file integrity: one transfer

Stream tracked `song.u8`, exactly 1,112,701 bytes and 55,636 DATA packets, using
source reads capped at seven bytes. The sender must assemble each DATA payload
from multiple short reads. The last packet contains one byte. Require equal
transmitted, acknowledged, accepted and persisted byte counts; exact packet
counts; source/receiver/disk CRC equality; zero byte errors; both endpoints
Completed; and exactly one publication after verification. Python additionally
performs a separate full streamed source/destination comparison and zlib CRC.

### Targeted loss recovery: six transfers

Individually drop the first START, READY, DATA(sequence 0), ACK(sequence 0), END,
and COMPLETE. Each transfer is 4,097 generated bytes, source chunk seven, seeds
base + 0 through base + 5. Each rule must actually execute once, cause exactly
one retry, and still produce a valid final file. The COMPLETE case explicitly
checks that retrying END does not publish a second time.

### Controlled loss campaign: 100 transfers

Transfer `i` (0..99) uses seed `base + i` for its data and schedule. Start a local
xorshift state at that seed. Consume PRNG outputs in this exact order:

1. Size = `4096 + next % 61441`; source chunk = `1 + next % 31`.
2. In order START, READY, END, COMPLETE, configure drops of the first
   `1 + next % 2` occurrences of each type.
3. Split `[0, packet_count)` into eight strata using integer boundaries
   `packet_count*j/8` and `packet_count*(j+1)/8`. In each stratum choose one
   sequence `begin + next % (end-begin)`. Drop its first `1 + next % 2` DATA
   transmissions and its first `1 + next % 2` ACK responses, in that order.

There are 20 rules per transfer. START+READY, DATA+ACK, or END+COMPLETE can lose
at most four frames in the same exchange, within the five-retry budget. This is
a bounded burst-loss schedule, **not** a Bernoulli loss rate or an estimate of
over-the-air reliability. The fake transport's seed alone does not randomize
drops: these generated explicit rules do. Records retain exact seeds, rules,
sequences, repeated-drop counts, and measured applications. Every rule must be
fully exercised; retries must equal measured drops. ACK loss must create exactly
the measured number of duplicate DATA deliveries without duplicate sink writes.

JSON fault fields use the existing enums: destination 0 = sender, 1 = receiver;
kind 0 = drop, 2 = corrupt; packet types 1 = START, 2 = READY, 3 = DATA, 4 = ACK,
6 = END, 7 = COMPLETE. Corruption offsets are zero-based positions in the full
32-byte frame (DATA starts at byte 12). `applied` is observed by the existing
transport when it selects a rule, not copied from the requested repeat count.

Require 100/100 completed, zero final integrity errors, no unexpected failure,
and all per-transfer accounting/publication checks passing.

### Corruption rejection: 28 negative transfers + 28 recovery probes

For each absolute payload position in `[0, 19, 20, 2048, 4079, 4096, 1310719]`,
test XOR masks `[0x01, 0x80, 0x55, 0xFF]`, in that order. These cover first/last
payload bytes, a packet boundary, the middle, the last full packet, the partial
final packet, and byte 19 of DATA sequence 65,535 at maximum size. Source size is
4,097 except the last position, which uses 1,310,720 bytes. Corruption touches
only payload bytes, leaving the codec-valid header/length intact so the
end-to-end CRC gate, not frame parsing, must catch it.

Negative case `i` uses seed `base + i`, source chunk seven, one changed byte, and
one injection at the first matching DATA sequence. Both endpoints must report
Failed / CrcMismatch (error code 16); the entire payload is accepted/ACKed before
END verification; persisted comparison must measure exactly one wrong byte;
source/destination CRC must differ; no publish callback may occur; the real host
partial must be closed and removed exactly once. CRC is not a cryptographic
integrity guarantee and these cases do not prove detection of every possible
multi-byte error or deliberate CRC collision.

Immediately after each rejection, reuse the same service, receiver and sink for
a clean 21-byte transfer, source chunk three, seed `base XOR (i+1)`. Require full
success and ignore stale DATA/ACK frames carrying the preceding transfer ID.
No test-driven receiver reset hides failed-session cleanup bugs.

### Maximum transfer: one transfer

Generate exactly 1,310,720 bytes / 65,536 DATA packets, seed base, chunk 13.
Require full byte/pattern/CRC checks, DATA sequences 0..65,535 accepted once,
sender and receiver next sequence 65,536 (32-bit counters, no premature wrap),
exact ACK counts, and successful publication/completion. This checks the actual
V2 maximum, not a scaled-down proxy.

### Back-to-back stress: 100 transfers on the same objects

Repeat the size list `[0, 1, 19, 20, 21, 255, 4097, 65535, 65536, 1310720]`
ten times. Transfer `i` uses seed `base + 0x10000 + i` and chunk `1 + i % 31`.
Keep one production service, receiver, sink, and monotonic clock throughout.
Only per-transfer test measurements and transport rule storage are new; do not
call receiver.reset or reconstruct the sessions. Check counter/CRC/ID reset on
new START, and inject the most recent prior DATA/ACK with stale transfer IDs
after a new START (196 ignored stale frames across this size schedule). After
the last transfer, re-read **all 100** earlier final files to verify their size,
CRC and full byte patterns are still correct. This includes ten complete
maximum-size transfers and transitions through empty and partial transfers.

## What this does not establish

Software qualification does **not** establish any of:

- Physical RF transmission or real nRF24 interoperability.
- SPI electrical behavior, signal integrity, or timing.
- Power integrity or radiated RF output.
- Packet error rate over the air.
- Antenna performance or range.
- Fabricated PCB functionality.
- ESP32 runtime memory/stack use, real-time scheduling, radio interrupts, or
  long-duration hardware stability.
- Actual SPIFFS capacity, rename behavior under power loss, flash wear, or
  durability; host filesystem callbacks are not SPIFFS.
- The compiled `src/main.cpp` transport/task/storage integration at runtime.

Firmware builds and SPIFFS image builds are compile/image evidence only.
Existing FakeHal tests exercise driver logic against a fake HAL, not a real
nRF24, electrical SPI bus, or antenna. Hardware work remains necessary; see
[hardware_validation.md](hardware_validation.md).

## Full regression/build validation

With the pinned requirements installed as in [build_and_test.md](build_and_test.md):

```sh
python tools/run_firmware_qualification.py
platformio test -e native -v
python -m unittest discover -s test -p "test_*.py"
platformio run -e rf3_custom_pcb -e rf3_esp32_devboard -e esp32wroom32d -e esp32wroom32d_manual_boot -e esp32wroom32d_devboard
platformio run -e rf3_custom_pcb -e rf3_esp32_devboard -t buildfs
python tools/check_repository_hygiene.py
git diff --check
git status --short
```

All five tracked firmware environments include the three compatibility aliases.
The two canonical SPIFFS environments are tested separately. CI also runs the
qualification command. A dirty tree is expected while reviewing uncommitted
changes; generated qualification/build artifacts must remain ignored.

## Recorded results

The measured run and full regression/build results are recorded in
[firmware_qualification_results.md](firmware_qualification_results.md). Always
retain `report.json` and `transfers.jsonl` with a presentation so the exact source
hashes and injected schedules remain tied to the claimed results.
