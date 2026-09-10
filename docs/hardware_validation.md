# RF3 ordered hardware-validation checklist

This procedure applies to the current `rf3_custom_pcb` image on two custom
ESP32-WROOM-32UE-N16 boards. Call them node A and node B. Do not flash the
devboard environment onto the custom PCB: its CE, CSN, and IRQ pins are different.

Start with [board_bringup.md](board_bringup.md) for checked image preparation,
manual BOOT/EN steps, separate sender/receiver filesystem images, and storage
headroom. Physical readiness remains conditional until the actual schematic,
module ratings, power, SPI and RF tests have passed. A successful host build or
software qualification is not evidence that these hardware gates have passed.

Keep a bench log with firmware commit, environment, node, serial port, supply
voltage/current, radio module identity, antenna, command, result, and any STATUS
line. Stop at the first failed dependency; later RF tests will otherwise obscure
the cause.

## Bench prerequisites

- Two assembled RF3 boards, two nRF24L01+PA+LNA modules, and their antennas.
- Two USB-UART connections (on-board bridges or external 3.3 V logic adapters)
  and two 115200-baud serial sessions. Verify programming access on the actual PCB.
- Current-limited bench supply and DMM; oscilloscope is strongly recommended.
- Logic analyzer capable of SPI mode 0 at 1 MHz.
- Optional 2.4 GHz spectrum analyzer/SDR, attenuator or shield box for CW.
- The pinned environment installed as described in `build_and_test.md`.
- Use only channels/power levels and radiated tests permitted at the test site.
- Never power or key a PA+LNA radio without its antenna or suitable RF load.
- Power down before connecting/disconnecting a radio module or probe ground.

Build once before the session:

```powershell
platformio run -e rf3_custom_pcb
platformio run -e rf3_custom_pcb -t buildfs
```

Upload and monitor each node, substituting its COM port:

Do this only after the unpowered inspection and safe-power gates below. The
custom profile uses `no_reset`: hold GPIO0/BOOT low through an EN/reset pulse to
enter download mode before each upload, then release BOOT and reset for normal
execution. Save wanted files first; `uploadfs` replaces their filesystem.
For the lean receiver image, use the generated handoff instructions instead of
the default `uploadfs` command below.

```powershell
platformio run -e rf3_custom_pcb -t upload --upload-port COMx
platformio run -e rf3_custom_pcb -t uploadfs --upload-port COMx
platformio device monitor --baud 115200 --port COMx
```

## Stage 1 — Physical and power sanity

### Test 1.1 — Unpowered assembly and wiring inspection

**Purpose**

Prove that it is safe to apply power and that the physical wiring matches the
compiled custom-PCB profile.

**Setup**

Disconnect USB and external power. Remove the radio if its orientation cannot
be inspected in place. Have the PCB schematic/layout and module pin legend
available.

**Action**

Inspect both boards under magnification. Confirm module orientation and trace or
continuity-check CE=GPIO17, CSN=GPIO27, SCK=GPIO18, MOSI=GPIO23, MISO=GPIO19,
IRQ=GPIO16, 3.3 V, and ground. Check solder bridges, lifted pins, reversed
headers, damaged coax/antenna connectors, and polarity. Measure 3.3 V-to-ground
resistance after capacitors settle. Attach the correct antenna securely.

**Expected result**

Both nodes match the pin table, no signal or supply is shorted, the radio is not
rotated, and the antenna connection is mechanically sound.

**Pass criterion**

Every named net has correct continuity, no adjacent unintended continuity is
found, and 3.3 V-to-ground does not remain at a near-zero resistance.

**Fail indications**

Reversed VCC/GND, mismatched GPIO, solder bridge, persistent rail short, damaged
module, loose antenna, or ambiguous header orientation.

**Most likely causes**

1. Radio/header orientation error.
2. Assembly bridge/open joint.
3. PCB revision does not match the repository pinout.

**Next diagnostic step**

Do not power the board. Isolate the radio from the PCB, repeat rail resistance
and point-to-point continuity, then repair or reconcile the PCB revision.

### Test 1.2 — Idle rail and boot-strap sanity

**Purpose**

Verify the 3.3 V rail and normal ESP32 boot with the radio connected. The custom
PCB uses GPIO27 for CSN, not GPIO5.

**Setup**

Radio and antenna connected; no RF command active. Power one node at a time from
a current-limited source/USB arrangement appropriate for the board. Put the DMM
at the radio VCC/GND pins, not only at the regulator.

**Action**

Apply power, observe current and the 3.3 V rail through reset, then press reset
five times. If available, scope the rail at the radio and EN during reset.
Repeat on the second node.

**Expected result**

The radio rail is stable near the intended 3.3 V and stays within the exact
PA+LNA module manufacturer's operating limits. Do not substitute a generic IC
supply range for the assembled module's rating. The ESP32 boots normally on
every reset. See the power-design references in [board_bringup.md](board_bringup.md).

**Pass criterion**

No current-limit event or sustained rail collapse occurs; both nodes reach
normal serial boot five of five times with the radio connected.

**Fail indications**

Rail outside the approved module/board operating limits, oscillation, regulator
overheating, bootloader-only output,
brownout/reset loop, or boot behavior changing when the radio is installed.

**Most likely causes**

1. Supply/regulator/decoupling deficiency or short.
2. Reset or boot-strapping signals held incorrectly by an assembly fault.
3. USB cable or connector voltage drop.

**Next diagnostic step**

Power down, remove the radio, and repeat. If boot recovers, measure EN and the
radio supply separately; if it does not, diagnose the ESP32 power/reset circuit.

## Stage 2 — ESP32 boot sanity

### Test 2.1 — Flash, boot identity, reset, and console

**Purpose**

Verify that each ESP32 runs the intended custom-PCB firmware and remains
diagnosable even if a peripheral fails.

**Setup**

Upload firmware and SPIFFS with `rf3_custom_pcb`. Open separate 115200-baud
serial monitors for A and B. Do not use `rf3_esp32_devboard`.

**Action**

Reset each node. Capture startup output from ROM boot through the `rf24>` prompt.
At the prompt enter `HELP`, `STATUS`, and `FILES`, then reset and repeat once.

**Expected result**

Startup identifies profile `custom-pcb`/node `rf3-pcb` and pins 17/27/16,
SPIFFS mounts, radio boot reports success or an explicit fault, HELP lists the
current commands, STATUS returns one coherent line, and FILES lists assets.

**Pass criterion**

Both nodes reach a responsive prompt twice, report the custom profile/pins, and
do not panic, watchdog-reset, or enter a reboot loop.

**Fail indications**

Wrong profile/pins, flash-size/partition error, mount error, garbled/no serial,
panic backtrace, repeated reset, or no prompt.

**Most likely causes**

1. Wrong environment or wrong COM port/image.
2. SPIFFS was not uploaded or partition table does not match the flash.
3. Power/reset/USB problem.

**Next diagnostic step**

Confirm the upload log names `rf3_custom_pcb`, erase/reflash only the affected
node, uploadfs again, and capture the complete first boot log.

## Stage 3 — SPI bus sanity

### Test 3.1 — SPI electrical activity and probe

**Purpose**

Distinguish an SPI/power/wiring failure from an over-the-air RF failure before
attempting a link.

**Setup**

Connect a logic analyzer to GPIO18 SCK, GPIO23 MOSI, GPIO19 MISO, GPIO27 CSN,
GPIO17 CE, GPIO16 IRQ, and ground on node A. Configure SPI decode as mode 0,
MSB-first. Keep leads short. Repeat later on B.

**Action**

Reset the node and capture startup. Confirm approximately 1 MHz clock while CSN
is low. Observe the RF_CH write/read/restore probe and subsequent register
writes. Run `STATUS` after boot.

**Expected result**

CSN frames SPI transactions, SCK idles low, MOSI is driven, MISO is neither
permanently floating nor stuck, and firmware reports `Radio boot OK`. STATUS and
FIFO values are not uniformly 0x00 or 0xFF artifacts.

**Pass criterion**

The probe reads back its temporary RF_CH value and both nodes complete radio
boot with valid 1 MHz mode-0 traffic.

**Fail indications**

No SCK/CSN, swapped MOSI/MISO, MISO fixed high/low, 0x00/0xFF-only reads, probe
fault, or unexpected CE activity during register access.

**Most likely causes**

1. Wiring/orientation/power/common-ground error.
2. Wrong probe reference or analyzer decode settings.
3. Damaged or incompatible nRF24 module.

**Next diagnostic step**

At the radio header measure VCC and continuity again, then decode the RF_CH
command byte directly. Swap in a known-good radio only after powering down.

## Stage 4 — Individual nRF24 validation

### Test 4.1 — Register configuration and state transitions on each radio

**Purpose**

Prove that A and B receive identical on-chip radio configuration and respond to
operator state commands independently.

**Setup**

Keep the analyzer from Test 3.1 connected. Test one powered node at a time so no
received RF traffic confuses the capture.

**Action**

Decode reset initialization. Verify the baseline CONFIG write is 0x0C and the
subsequent powered packet-standby value is 0x0E (RX uses 0x0F). Verify
EN_AA=0x00,
EN_RXADDR=0x01, SETUP_AW=0x03, SETUP_RETR=0x00, RF_CH=76,
RF_SETUP=0x26, RX_ADDR_P0 and TX_ADDR=`52 46 33 24 01`, RX_PW_P0=32,
DYNPD=0, FEATURE=0. Then run `RX`, `STATUS`, `STANDBY`, `SLEEP`, `WAKE`,
`POWERDOWN`, `WAKE`, `CHANNEL 75`, `CHANNEL 76`, `POWER 0`, and `POWER 3`.

**Expected result**

The register sequence matches on both radios. STATUS follows RxListening,
Standby, Sleep/PowerDown and recovery states; channel returns to 76 and packet
power to level 3. CE is high in RX and low in standby/sleep.

**Pass criterion**

Both nodes match every register value and complete every transition without a
fault; final STATUS is Standby, channel=76, power=3.

**Fail indications**

Different address/rate/width/channel between nodes, failed command, stuck CE,
IRQ stuck asserted after clearing, or a Fault state.

**Most likely causes**

1. SPI corruption or marginal radio supply.
2. Wrong/clone module behavior.
3. Firmware/image mismatch between nodes.

**Next diagnostic step**

Compare the first differing SPI write/read between A and B. Correct electrical
integrity or image mismatch before any RF link test.

## Stage 5 — Minimum RF link

### Test 5.1 — One-byte Protocol v2 transfer at close range

**Purpose**

Demonstrate the smallest useful real link: control handshake, one DATA packet,
ACK, end verification, publication, and COMPLETE response.

**Setup**

Place A and B 1–2 m apart with antennas in the same orientation. Do not place
PA+LNA antennas directly against each other. Create and stage a one-byte file,
then build/upload the filesystem to both nodes:

```powershell
python -c "from pathlib import Path; p=Path('.pio/bench'); p.mkdir(parents=True, exist_ok=True); (p/'one.bin').write_bytes(bytes([0x5A]))"
python tools\stage_demo_file.py .pio\bench\one.bin --output-name one.bin
platformio run -e rf3_custom_pcb -t buildfs
platformio run -e rf3_custom_pcb -t uploadfs --upload-port COM_A
platformio run -e rf3_custom_pcb -t uploadfs --upload-port COM_B
```

Set `CHANNEL 76` and `POWER 0` on both.

Alternatively, the board handoff already includes `one.bin` in both role
images without changing `data/`. Its CRC32 is `59BC5767`. Use the lean receiver
image before large-file testing, especially with a 4 MB devboard receiver.

**Action**

On B enter `RX`. On A enter `TX one.bin`. Watch both consoles until completion,
then enter `STATUS` on both and `FILES` on B.

**Expected result**

A logs one DATA packet and `remote receiver verified and published` with
`peer_complete=true`, `elapsed_ms`, and `throughput_bps`. B logs a verified
one-byte `rx_XXXXXXXX.bin` publication. No retry is required in a clean
close-range setup.

**Pass criterion**

The same transfer ID and CRC32 appear on both sides; A reports completion and B
lists exactly one new one-byte received file.

**Fail indications**

No READY, repeated timeout/retry, wrong transfer ID, CRC/sequence error,
publication error, or a received file not exactly one byte.

**Most likely causes**

1. Channel/address/rate mismatch or one node not in RX.
2. RF overload, antenna, supply droop, or IRQ/SPI fault.
3. Filesystem not uploaded or insufficient receiver space.

**Next diagnostic step**

Compare final STATUS fields (`tx_state`, `tx_error`, `tx_retries`,
`tx_elapsed_ms`, `tx_bps`, `rx_state`, `rx_error`, `rx_duplicates`,
`rx_drain_hits`, `fault`). If no frames are seen, repeat at power 0 and 2 m; if
frames arrive but fail, capture both serial logs and SPI/IRQ around the first
mismatch.

## Stage 6 — Repeated packet transfer

### Test 6.1 — Repetition, retry, duplicate, and sequence stability

**Purpose**

Exercise repeated sessions and verify that RF loss causes bounded retries rather
than corrupt publication or state lockup.

**Setup**

Keep B in `RX`, both on channel 76, initially at power 0 and close range. Use the
one-byte fixture. Have an RF shield/attenuator available; do not disconnect or
short a powered antenna.

**Action**

On A run `TX LOOP 20 one.bin`. Count successful publications on B. During one
middle transfer, briefly increase attenuation enough to provoke a retry, then
restore the link before retry exhaustion. After the loop, run STATUS and FILES.

**Expected result**

All 20 sessions complete and create 20 collision-safe receive names. The
disturbed session may increment `tx_retries`; duplicate START/DATA/END traffic
is handled idempotently and each completed file remains one byte with the same
CRC. Both nodes return to a usable state.

**Pass criterion**

Twenty verified publications, no incorrect file size/CRC, no panic/deadlock,
and at least one observed retry if attenuation was applied without any duplicate
publication for the same transfer.

**Fail indications**

Missing/extra publication, sequence/CRC error after link recovery, unbounded
hang, increasing partial files, or inability to start the next transfer.

**Most likely causes**

1. Real RF/supply margin is insufficient.
2. IRQ/polling misses FIFO service under traffic.
3. Unexpected module retry/clone behavior or a software timing issue.

**Next diagnostic step**

Repeat without induced attenuation at power 3. If clean, characterize the loss
threshold; if still failing, preserve both timestamped logs and capture IRQ,
CE, and SPI for the first failed sequence.

### Test 6.2 — Receiver-absent bounded failure

**Purpose**

Verify timeout/retry behavior when no peer can answer, without confusing it for
a firmware hang.

**Setup**

Keep A powered. Put B in `STANDBY` or power it off normally. A has `one.bin` and
is on channel 76.

**Action**

Run `TX one.bin` on A and wait. Do not press STOP. Record elapsed time, retry
messages, final error, and STATUS.

**Expected result**

A retries the START/control exchange up to the configured bound (maximum retry
count 5), reports failure/no peer completion, and remains console-responsive.

**Pass criterion**

The attempt terminates without publication or `peer_complete=true`, does not
hang beyond the bounded protocol timing, and a subsequent `STATUS` and `STOP`
respond.

**Fail indications**

Infinite wait, false success, reset/panic, permanently busy command system, or
file publication on B while B was not receiving.

**Most likely causes**

1. Sender timeout/state-machine defect exposed only by real scheduling.
2. Another receiver is unexpectedly active on the same RF settings.
3. Serial observation ended before the asynchronous TX worker finished.

**Next diagnostic step**

Issue STATUS and inspect `tx_state`, `tx_error`, `tx_retries`, and
`peer_complete`. If still active past 10 s plus bounded retry overhead, capture
task/watchdog output and test STOP responsiveness.

## Stage 7 — Command behavior

### Test 7.1 — Valid command/state matrix

**Purpose**

Verify the operator interface used for all later diagnosis and confirm that
commands normalize state safely.

**Setup**

Use one node with a good radio and filesystem. Antenna connected. Do not start
CW yet.

**Action**

Run in order: `HELP`, `FILES`, `SELECT one.bin`, `STATUS`, `RX`, `STATUS`,
`STANDBY`, `SLEEP`, `STATUS`, `WAKE`, `POWERDOWN`, `WAKE`, `CHANNEL 75`,
`CHANNEL 76`, `POWER 0`, `POWER 3`, `TX one.bin` with the peer in RX, and
`STOP`. Check STATUS after each state-changing command.

**Expected result**

Each command reports its action, STATUS matches it, selected file persists, and
STOP returns any active work to standby. Commands are case-insensitive.

**Pass criterion**

Every command either succeeds as described or gives an accurate busy/failure
message; final STATUS is Standby/channel 76/power 3 with a responsive prompt.

**Fail indications**

Silent command, misleading STATUS, unexpected state, command deadlock, selected
file change, or inability to STOP.

**Most likely causes**

1. Command/radio mutex contention under live scheduling.
2. Prior transfer did not finish when the next command was entered.
3. Serial terminal line-ending or echo configuration.

**Next diagnostic step**

Repeat the first failing command from a fresh reset with no background transfer,
then capture the command, response, and immediately following STATUS.

### Test 7.2 — Invalid input and buffer boundaries

**Purpose**

Confirm invalid operator input cannot silently become a valid but dangerous
radio/timing command.

**Setup**

Node in standby with serial prompt available.

**Action**

Try `CHANNEL -1`, `CHANNEL 126`, `POWER 4`, `CW LOOP -1 10`,
`CW LOOP 4294967296 10`, `TX does_not_exist.u8`, `SELECT does_not_exist.u8`,
`MORSE !!!`, an unknown command, and a line longer than 159 characters followed
by Enter. Run STATUS afterward.

**Expected result**

Every input is rejected with a specific usage/range/file/length message. The
overlong prefix is not executed. Radio state and selected file do not change.

**Pass criterion**

Zero invalid commands initiate TX/RX/CW or change channel/power, and the prompt
remains responsive.

**Fail indications**

Wraparound numeric acceptance, silent truncation/execution, crash, state change,
or command system stuck busy.

**Most likely causes**

1. Old firmware image was flashed.
2. Terminal is sending unexpected control/line-ending characters.
3. A remaining parser/dispatcher defect.

**Next diagnostic step**

Confirm the current firmware was rebuilt/flashed, reproduce with a minimal
terminal, and save the exact byte sequence and console response.

## Stage 8 — SPIFFS and audio sanity

### Test 8.1 — Filesystem inventory and raw-audio assumptions

**Purpose**

Verify the files used for long transfer tests and separate file-format mistakes
from RF transport mistakes.

**Setup**

Current repository data image uploaded. Host has an audio tool capable of raw
unsigned 8-bit playback, for example ffplay.

**Action**

On both nodes run FILES. Confirm `speech_test.u8`=360,000 bytes and
`song.u8`=1,112,701 bytes. On the host explicitly interpret each as unsigned
8-bit, 8 kHz, mono, for example:

```powershell
ffplay -f u8 -ar 8000 -ac 1 data\speech_test.u8
ffplay -f u8 -ar 8000 -ac 1 data\song.u8
```

Compute host CRC32 values for the bench log:

```powershell
python -c "import pathlib,zlib; p=pathlib.Path(r'data\speech_test.u8'); print(f'{zlib.crc32(p.read_bytes()) & 0xffffffff:08X}')"
python -c "import pathlib,zlib; p=pathlib.Path(r'data\song.u8'); print(f'{zlib.crc32(p.read_bytes()) & 0xffffffff:08X}')"
```

**Expected result**

Both nodes report exact sizes and the host interpretation has plausible speed,
duration, and level for the known fixture. RF3 itself treats `.u8` as raw bytes
and intentionally carries no audio metadata.

**Pass criterion**

Sizes match on host/A/B, CRCs are recorded, and the external format check is
consistent with unsigned 8-bit/8 kHz/mono provenance.

**Fail indications**

Missing/different size, SPIFFS mount failure, obviously wrong speed/format when
interpreted as specified, or host file differs from the uploaded image source.

**Most likely causes**

1. Stale/missing uploadfs image.
2. Asset was converted with wrong sample format/rate/channels.
3. Wrong data directory or environment used for buildfs.

**Next diagnostic step**

Rebuild/uploadfs from the current checkout and compare FILES again. If only
audio interpretation fails, reconvert the source asset; do not change RF
packet code.

## Stage 9 — Complete audio-file transmission

### Test 9.1 — Exact-full final DATA boundary

**Purpose**

Prove a long transfer whose final DATA packet is completely full.

**Setup**

Both nodes channel 76; begin at power 0/close range. B in RX. Host CRC for
`speech_test.u8` recorded. This file is 360,000 bytes = exactly 18,000 DATA
packets of 20 bytes.

**Action**

On A run `TX speech_test.u8`. Allow it to finish without another command. Record
the TX start/completion lines, including `elapsed_ms` and `throughput_bps`, and
B's verified publication line. Run STATUS on both and FILES on B.

**Expected result**

Sequence starts at zero, reaches 18,000 DATA packets without rollover, END is
accepted, CRC32 matches the host, B publishes exactly 360,000 bytes, and A sees
COMPLETE/`peer_complete=true`.

**Pass criterion**

Matching transfer ID, byte count, packet count and CRC on A/B/host; exactly one
new 360,000-byte receive file; no protocol or storage error.

**Fail indications**

Off-by-one final packet, unexpected zero-length DATA, CRC mismatch, incomplete
file, excessive retry growth, timeout, reset, or no COMPLETE.

**Most likely causes**

1. RF/power integrity degrades during sustained traffic.
2. Real-time FIFO/IRQ service misses under load.
3. Remaining exact-boundary or filesystem issue.

**Next diagnostic step**

Use the first nonzero `tx_error`/`rx_error` and sequence in the paired logs. If
retries climb first, scope the 3.3 V rail and IRQ; if CRC fails without RF
errors, repeat with logic-analyzer capture near the final sequence.

### Test 9.2 — Partial final DATA boundary

**Purpose**

Prove first-to-last streaming and the one-byte final payload path on a large
real fixture.

**Setup**

Same as Test 9.1, with `song.u8`. It is 1,112,701 bytes: 55,635 full DATA
packets plus a final one-byte DATA packet.

**Action**

On B ensure RX is active. On A run `TX song.u8`. Preserve both complete logs,
then run STATUS on both and FILES on B.

**Expected result**

The final DATA frame declares one valid payload byte and zero padding; receiver
CRC32 and size match the sender/host; B publishes one 1,112,701-byte file and A
receives COMPLETE.

**Pass criterion**

Matching transfer ID, 1,112,701 bytes, 55,636 packets, host/sender/receiver CRC,
and `peer_complete=true`, with no malformed/padding/sequence/storage error.

**Fail indications**

Last byte lost/duplicated, 1,112,700 or 1,112,720-byte output, padding rejection,
CRC mismatch, timeout, reboot, or partial file left visible.

**Most likely causes**

1. Sustained RF/supply margin problem.
2. Final-partial packet behavior differs on real path.
3. Receiver storage capacity or write/close problem.

**Next diagnostic step**

Compare reported final byte/packet/CRC fields. A size of 1,112,700 points at the
partial final frame; broader loss/retries point at RF/power/IRQ service.

## Stage 10 — Failure and recovery

### Test 10.1 — Operator interruption and reset during transfer

**Purpose**

Verify cancellation/reset cannot publish an unverified file and that stale
partial storage is recoverable.

**Setup**

B in RX, A ready to send `song.u8`. Both serial logs visible.

**Action**

Start TX on A, wait for data progress, then enter STOP on A. Confirm recovery.
Start again and reset B mid-transfer. Wait beyond the 10 s receiver/sender
inactivity window, then reset both. Run FILES and STATUS; finally repeat the
one-byte successful transfer.

**Expected result**

Interrupted transfers do not appear as completed receive files. Internal
`.part` data is removed/hidden during abort/startup cleanup. A times out or is
stopped predictably; both nodes return to standby/RX and can transfer again.

**Pass criterion**

No interrupted file is published, no `.part` is listed, both prompts recover,
and the final one-byte transfer succeeds without reflashing.

**Fail indications**

Truncated file published as complete, visible/stuck partial, reboot loop,
permanent Busy/Fault state, filesystem mount failure, or subsequent TX failure.

**Most likely causes**

1. Reset occurred during a flash operation and exposed storage cleanup weakness.
2. Power brownout affected both radio and flash.
3. Cancellation/state recovery defect.

**Next diagnostic step**

Capture the startup cleanup and first STATUS after reset. If SPIFFS will not
mount, re-uploadfs and repeat once while monitoring the 3.3 V rail.

### Test 10.2 — Radio initialization failure and recovery

**Purpose**

Confirm a missing/bad radio is reported as a peripheral fault rather than an
opaque ESP32 failure, and that repair plus reset restores operation.

**Setup**

Power down one node. Disconnect the radio module deliberately and safely while
keeping the serial connection available for the next power-up.

**Action**

Power up without the radio, capture startup, and run HELP, STATUS, FILES, and
TX one.bin. Power down, reconnect the radio/antenna, power up, and rerun STATUS
plus the one-byte link test.

**Expected result**

The first boot reports radio probe/boot failure but preserves the console and
filesystem diagnostics. TX fails explicitly. After reconnection/reset, radio
boot succeeds and the small link works.

**Pass criterion**

No crash/reboot loop without the radio; explicit fault is visible; normal
operation returns after a cold reconnect without firmware changes.

**Fail indications**

Console unavailable, false radio success with all-0/all-1 reads, unexplained
ESP32 reset, or failure persists with a known-good reconnected module.

**Most likely causes**

1. Expected open-bus/probe failure (during the deliberate fault).
2. Reconnection/orientation/power error.
3. SPI bus or module damage.

**Next diagnostic step**

Repeat Test 3.1 on the recovered assembly and inspect the RF_CH probe readback.

### Test 10.3 — Missing file and transmitter-absent receive behavior

**Purpose**

Verify benign absence cases do not corrupt state or produce false files.

**Setup**

One node in standby, then RX, with a valid mounted filesystem.

**Action**

Run `TX missing.u8` and `SELECT missing.u8`; then enter RX with no transmitter
active for at least 15 s. Run STATUS and FILES, then STOP.

**Expected result**

Missing-file commands fail immediately. Idle RX remains listening without
creating a file or reporting a transfer failure; STOP returns to standby.

**Pass criterion**

No file/state mutation from missing names, no phantom receive publication, and
the node remains responsive throughout idle RX.

**Fail indications**

TX begins for a missing file, phantom `.bin` appears, RX faults merely because
no transmitter exists, or STOP fails.

**Most likely causes**

1. Stale image/old command behavior.
2. Ambient node using the same address/channel.
3. Filesystem listing or RX state bug.

**Next diagnostic step**

Shield the node or change both test nodes to an unused channel temporarily. If
phantom traffic stops, identify ambient RF3 traffic; otherwise capture STATUS.

## Stage 11 — RF/CW observations and range

### Test 11.1 — Low-power CW and Morse observation

**Purpose**

Verify RF test modes, channel placement, keying, STOP behavior, and packet-mode
restoration without using CW as the first proof of radio health.

**Setup**

Packet tests through Stage 10 have passed. Antenna or rated RF load attached.
Use a spectrum analyzer/SDR at safe coupling/distance and comply with local RF
rules. Start at power level 0 on channel 76.

**Action**

Run `CW START 76 0`, observe briefly, then STOP and STATUS. Run
`CW LOOP 100 900 76 0 EVERY 5`, observe five keyed bursts, STOP. Run
`MORSE SOS`, observe dot/dash timing, then STATUS. Finish by repeating the
one-byte packet transfer.

**Expected result**

Energy appears at the channel-76 center frequency (2.476 GHz), follows CW loop
and SOS keying, stops promptly, and STATUS returns to packet channel 76/power 0
or the subsequently selected packet power. Packet transfer still works.

**Pass criterion**

Correct frequency/keying visible, no unintended continuous output after STOP,
no reset/brownout, and post-CW packet link succeeds.

**Fail indications**

No energy, wrong channel, malformed timing, output that will not stop, rail
collapse/reset, or packet mode broken afterward.

**Most likely causes**

1. Instrument coupling/settings or clone-radio CW behavior.
2. PA supply droop or antenna/RF path fault.
3. CW fallback/state restoration defect.

**Next diagnostic step**

Check CE/RF_SETUP/RF_CH over SPI and scope 3.3 V during keying. If packet mode
works but CW does not, classify it as module-specific CW behavior before
changing transfer firmware.

### Test 11.2 — Repeatability and progressive separation

**Purpose**

Establish a first reliability baseline only after close-range correctness is
known.

**Setup**

Both nodes channel 76, power 0 initially, antennas in repeatable orientation.
Use `one.bin` or another small known file and a logged set of distances. Avoid
moving people/cables during each run.

**Action**

At 2 m run `TX LOOP 100 one.bin`; record successes, retries, errors, resets, and
rail minimum. Repeat at progressively larger separation. Increase power one
level at a time only after recording the previous result. Stop when reliability
drops or the planned safe test boundary is reached.

**Expected result**

At close range all transfers verify with stable state. With separation, retry
rate may rise before failures; no corrupted file is published because CRC and
completion gate publication.

**Pass criterion**

Documented 100/100 verified close-range transfers with zero wrong-size/CRC
files, no reset/deadlock, and a repeatable distance/power/retry trend.

**Fail indications**

Close-range failures, non-repeatable cliff, CRC-corrupt file publication,
reboots at higher power, or retries unrelated to separation/orientation.

**Most likely causes**

1. Antenna/orientation/interference or expected link-budget limit.
2. PA burst-current supply integrity, especially at higher power.
3. Module mismatch/clone behavior or timing margin.

**Next diagnostic step**

Return to the last known-good distance/power. If it does not recover, inspect
rail/temperature/state; if it does, vary one factor at a time (orientation,
channel, power, then module).

## Shortest critical path to a convincing RF link

Do these in order and stop on failure:

1. Test 1.1: correct unpowered wiring, orientation, antenna, and no rail short.
2. Test 1.2: stable 3.3 V and repeatable ESP32 boot with radio attached.
3. Test 2.1: flash `rf3_custom_pcb`; confirm profile, prompt, SPIFFS, and STATUS
   on both nodes.
4. Test 3.1: prove the RF_CH SPI probe and valid MISO response on both nodes.
5. Test 4.1: confirm both radios have channel 76, 250 kbit/s, identical address,
   32-byte width, and usable RX/standby transitions.
6. Test 5.1: transfer one byte at close range and match transfer ID, CRC, size,
   publication, and `peer_complete=true`.
7. Test 6.1 without induced attenuation: complete 20 repeated transfers.
8. Test 9.1: complete `speech_test.u8` (exact-full final boundary).
9. Test 9.2: complete `song.u8` (one-byte partial final boundary).

Step 6 is the first demonstrated working RF link. Steps 7–9 make the result
convincing across repetition, sustained traffic, both final-packet boundaries,
end-to-end CRC, and receiver publication. Failure/recovery, CW, and range tests
then characterize robustness without being prerequisites for the first proof.
