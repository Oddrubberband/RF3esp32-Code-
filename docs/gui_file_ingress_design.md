# GUI-to-ESP file ingress design

## Implementation status

The first bounded implementation is now present in the firmware and desktop
GUI. `UPLOAD BEGIN/DATA/END/CANCEL` stages and verifies PC files in SPIFFS;
`DOWNLOAD BEGIN/DATA/END/CANCEL` exports a SPIFFS file to a verified temporary
PC file. The serial backend uses acknowledged 64-byte hex chunks over either
USB serial or Bluetooth SPP. Normal mode starts the existing `TX` path after
upload. Devboard test mode instead uses **Upload & Verify** to download the
published file into bounded memory, validate size/CRC32, and compare all bytes
with the original upload. It reports SHA-256 and never issues `TX`; no nRF24
board is required for that host transport/storage check.

The current reliability changes add capability-gated DATA retries, connection
ownership and inactivity expiry, complete-line buffering, and serialized SPP
output with congestion/write-completion handling. Verified SPIFFS replacements
use a backup plus checksummed recovery journal. Full reconnect resume is not
implemented; see `bluetooth_spp_control.md` for exact behavior and limitations.

The transport-neutral module split described below remains a recommended
follow-up refactor. The initial firmware state machines live in `main.cpp` so
the behavior can be exercised on hardware before moving ownership boundaries.

The original firmware could transmit only files that already existed in
SPIFFS. The first implementation now lets the Windows GUI upload a file to the
ESP32 at runtime, verifies its size and CRC32, and then invokes the existing
`TX <filename>` path to stream it through Protocol v2 over nRF24.

## Current confirmed behavior

- The serial console accepts line-oriented commands in `DemoConsoleApp::run()`.
- `dispatchCommand()` routes commands to handlers such as `TX`, `RX`,
  `CHANNEL`, `POWER`, `STOP`, and `STATUS`.
- `commandTx()` expects a filename that already exists in SPIFFS.
- `sendDataFile()` wraps the SPIFFS file as `FileTransfer::StreamingDataSource`.
- `FileTransfer::Service` and `ReliableTransferV2::SenderSession` create the
  nRF24 Protocol v2 frames.
- RF packetization, retries, ACK/NACK handling, CRC32, and final verification
  are already owned by firmware and should remain there.

## Implemented first firmware change

Add a transport-neutral host file-ingress service that can receive bounded
chunks from USB serial first and Bluetooth SPP later, write them into a
temporary SPIFFS file, verify them, publish the completed file, and then allow
the existing `TX <filename>` path to send it.

This keeps the first integration low risk because it reuses the proven RF
transmit path.

Suggested command flow:

```text
UPLOAD BEGIN <name> <size> <crc32>
UPLOAD DATA <sequence> <hex-or-base64-payload>
UPLOAD END
TX <published-name>
```

Better long-term framing:

```text
RF3H binary frame:
  magic
  version
  message_type
  message_id
  payload_length
  payload_crc
  payload
```

Use explicit message IDs so the GUI can retry a command without accidentally
executing a duplicate operation.

## Firmware modules to add or split

- `include/host_command_service.hpp`
  - Small command API used by serial, Bluetooth, and optional HTTP.
  - Owns command result objects instead of printing raw text directly.

- `include/host_file_ingress.hpp`
  - State machine for `BEGIN`, `DATA`, `END`, `CANCEL`.
  - Tracks filename, expected size, received size, CRC32, sequence, timeout,
    and temporary/final SPIFFS paths.

- `src/host_file_ingress.cpp`
  - SPIFFS partial-file implementation.
  - Rejects names ending in `.part`.
  - Publishes only after size and CRC match.

- `src/bluetooth_spp_transport.cpp`
  - Future ESP32 Bluetooth Classic Serial Port Profile endpoint.
  - Feeds complete host messages into the same command service.

## Minimal firmware integration points

1. Extract `dispatchCommand()` or its command handlers from `DemoConsoleApp`
   into a reusable service that can return structured results.
2. Keep `DemoConsoleApp::run()` as the USB console frontend, but let it call
   the shared command service.
3. Add `UPLOAD` commands to the shared command service.
4. Add a SPIFFS-backed upload sink similar to the existing RF receive sink.
5. Make `STATUS` report upload state fields, such as:

```text
upload_state=Idle upload_name=payload.bin upload_bytes=0/1234 upload_error=none
```

6. Later, add Bluetooth SPP and feed received messages into the same command
   service.

## Architecture options

### Option A: GUI uploads to SPIFFS, then firmware runs TX

Status: recommended first implementation.

Pros:

- Reuses current `TX` implementation.
- Easy to debug with existing `FILES`, `SELECT`, `TX`, and `STATUS`.
- Survives Bluetooth hiccups before RF transmission begins.
- Keeps ESP RAM use bounded.

Cons:

- Uses SPI flash writes.
- Requires enough SPIFFS free space for the uploaded object.

### Option B: GUI streams directly into RF transmitter

Status: later optimization.

Pros:

- Avoids writing the complete file to SPIFFS.
- Better for large or frequent transmissions.

Cons:

- Current sender inspects and rewinds the source before starting Protocol v2.
- A direct Bluetooth stream would need a rewindable source or a modified sender
  preparation path.
- More timing-sensitive because Bluetooth ingress and nRF24 send/ACK loops
  would run at the same time.

### Option C: GUI creates nRF24 Protocol v2 RF frames itself

Status: not recommended.

Reason:

The firmware already owns nRF24 framing, CRC32, retries, radio timing, state
transitions, and receiver compatibility. Duplicating that logic in Python would
create two protocol implementations that can drift apart.

## GUI-side impact

The current GUI backend interface can stay mostly unchanged. Add a transfer
method to the production backend that performs:

```text
connect
handshake/capabilities
UPLOAD BEGIN
UPLOAD DATA chunks with ACKs
UPLOAD END
TX published-name
poll STATUS until completed/failed
```

Mock mode should simulate the same high-level events without importing serial
or Bluetooth-specific logic into the UI.
