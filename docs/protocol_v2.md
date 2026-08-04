# RF3 Reliable File-Transfer Protocol v2

Status: implementation specification for `codex/reliable-protocol-v2`

## Goals

RF3 Protocol v2 transfers arbitrary binary files over the existing ESP32 and
nRF24L01+ radio link. Audio files, including `.u8`, are ordinary binary
payloads rather than a special transport mode.

The protocol provides:

- a version and transfer identifier in every frame;
- explicit control and data packet types;
- a START/READY handshake;
- stop-and-wait DATA/ACK delivery with bounded retries;
- idempotent duplicate handling;
- declared byte and packet counts;
- incremental end-to-end CRC32 verification;
- END/COMPLETE verified remote completion;
- cancellation and inactivity cleanup;
- fixed-memory streaming at both endpoints;
- partial-file isolation and collision-safe publication.

Protocol v2 does not change the nRF24 channel, data rate, output power,
addresses, fixed payload width, SPI pins, GPIO profiles, or hardware auto-ACK
configuration.

## Compatibility policy

Protocol v2 is not wire-compatible with Protocol v1. Both boards must run
Protocol v2. A v2 receiver rejects the `RF2` marker with any unsupported
version and never interprets a Protocol v1 file frame as a v2 frame.

Legacy source components may remain for component tests or non-file remote
console control, but the production file-transfer path uses Protocol v2.

## Byte order and encoding rules

All multibyte integers use little-endian byte order. Frames are encoded and
decoded field by field. The implementation must not transmit a C++ structure,
depend on compiler packing, or reinterpret raw frame bytes as a structure.

Every frame is exactly 32 bytes. An encoder clears the complete frame before
writing fields. Unless a layout explicitly assigns a byte to payload, every
reserved or unused byte must be zero. A decoder rejects a frame with a nonzero
reserved byte or nonzero DATA padding.

## Common frame header

Every frame begins with this nine-byte header:

| Offset | Size | Field | Value |
| ---: | ---: | --- | --- |
| 0 | 1 | Magic 0 | ASCII `R` (`0x52`) |
| 1 | 1 | Magic 1 | ASCII `F` (`0x46`) |
| 2 | 1 | Magic 2 | ASCII `2` (`0x32`) |
| 3 | 1 | Protocol version | `2` |
| 4 | 1 | Packet type | Defined below |
| 5 | 4 | Transfer ID | Nonzero 32-bit little-endian value |

Packet-type values are:

| Value | Type |
| ---: | --- |
| 1 | START |
| 2 | READY |
| 3 | DATA |
| 4 | ACK |
| 5 | NACK |
| 6 | END |
| 7 | COMPLETE |
| 8 | ERROR |
| 9 | CANCEL |

Unknown packet types are rejected.

## Packet layouts

### START

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 9 | Common header |
| 9 | 4 | Total file size in bytes |
| 13 | 4 | Total DATA packet count |
| 17 | 4 | Final file CRC32 |
| 21 | 1 | Flags; currently zero |
| 22 | 10 | Reserved; zero |

The receiver validates the size/count relationship, limit, flags, transfer ID,
and reserved bytes before creating a partial file or returning READY.

### READY

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 9 | Common header |
| 9 | 4 | Next expected DATA sequence |
| 13 | 1 | Accepted: `1` yes, `0` no |
| 14 | 1 | Error code; zero when accepted |
| 15 | 17 | Reserved; zero |

A duplicate START for the active transfer returns READY again without
resetting accepted data. A receiver busy with another transfer returns a
rejected READY with `Busy`.

### DATA

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 9 | Common header |
| 9 | 2 | DATA sequence, 0 through 65,535 |
| 11 | 1 | Payload length, 1 through 20 |
| 12 | 20 | Payload followed by zero padding |

`kDataPayloadCapacity` is derived as `32 - 12 = 20`; application code and
tests must use that named constant rather than the Protocol v1 value of 28.

Every nonfinal DATA packet contains 20 bytes. The final packet contains the
remaining 1 through 20 bytes. A zero-byte file sends no DATA packet.

### ACK

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 9 | Common header |
| 9 | 2 | DATA sequence accepted |
| 11 | 21 | Reserved; zero |

ACK identifies the exact DATA sequence accepted. The sender advances only when
this value equals the sequence currently awaiting acknowledgment.

### NACK

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 9 | Common header |
| 9 | 4 | Next sequence expected by the receiver |
| 13 | 1 | Error code |
| 14 | 18 | Reserved; zero |

DATA ahead of the expected sequence produces NACK without writing the payload.

### END

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 9 | Common header |
| 9 | 4 | Total bytes sent |
| 13 | 4 | Total DATA packets sent |
| 17 | 4 | Final CRC32 |
| 21 | 11 | Reserved; zero |

The receiver accepts END only after all declared DATA packets have been
accepted contiguously.

### COMPLETE

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 9 | Common header |
| 9 | 4 | Verified byte count |
| 13 | 4 | Verified CRC32 |
| 17 | 1 | Status; zero for success |
| 18 | 14 | Reserved; zero |

The sender reports success only after receiving a matching COMPLETE. A local
radio transmission finishing is not remote success.

### ERROR

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 9 | Common header |
| 9 | 1 | Stable error code |
| 10 | 4 | Relevant sequence or next expected sequence |
| 14 | 1 | Endpoint state value for diagnostics |
| 15 | 17 | Reserved; zero |

### CANCEL

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 9 | Common header |
| 9 | 1 | Cancellation reason code |
| 10 | 22 | Reserved; zero |

Cancellation is idempotent. A receiver removes the active partial and returns
ERROR with `Cancelled`; a duplicate CANCEL returns the same result without
performing a second write or publication.

## Transfer identifiers

Transfer IDs are nonzero 32-bit values. Production uses `esp_random()` and
rejects zero and the immediately previous ID. Host tests inject deterministic
IDs. A frame for another transfer cannot change source position, sink content,
sequence counters, CRC state, timeout state, or publication state.

## Size and sequence limits

The DATA sequence is 16 bits, so a transfer may contain at most 65,536 DATA
packets numbered 0 through 65,535. With a 20-byte DATA payload capacity:

- maximum packet count: 65,536;
- maximum nonempty file size: 1,310,720 bytes;
- supported file-size range: 0 through 1,310,720 bytes.

Internal calculations use wider counters. A source larger than 1,310,720
bytes is rejected during the streaming inspection pass before START is sent.
Sequence and size counters cannot wrap silently.

Zero-byte files are unambiguous: START declares size and packet count zero,
READY accepts, the sender transmits END without DATA, and the receiver verifies
the standard empty-input CRC before publishing an empty file.

## CRC32

Protocol v2 uses CRC-32/ISO-HDLC (also called CRC-32/ADCCP):

- polynomial: `0x04C11DB7`;
- reflected implementation polynomial: `0xEDB88320`;
- reflected input and output;
- initial value: `0xFFFFFFFF`;
- final XOR: `0xFFFFFFFF`;
- check value for ASCII `123456789`: `0xCBF43926`;
- empty-input value: `0x00000000`.

CRC is calculated incrementally. The sender performs a fixed-buffer inspection
pass before START, resets the source, and calculates CRC again implicitly over
the transmitted bytes. The receiver updates CRC only when it accepts a new
in-order DATA packet. Duplicate DATA never updates CRC twice.

CRC mismatch removes the partial file, prevents publication, and returns ERROR
with `CrcMismatch`.

## Reliability and retry semantics

Protocol v2 is correctness-first stop-and-wait:

1. Sender transmits START and waits for READY.
2. Sender transmits one DATA frame and waits for ACK of that exact sequence.
3. Lost DATA causes sender timeout and retransmission.
4. Lost ACK causes retransmission of the same DATA.
5. Receiver recognizes a sequence below `expected_sequence` as a duplicate,
   does not append it, and reissues ACK.
6. DATA above `expected_sequence` produces NACK for the expected sequence.
7. After all DATA is acknowledged, sender transmits END and waits for COMPLETE.
8. Lost END causes END retransmission.
9. Lost COMPLETE causes END retransmission; duplicate END returns COMPLETE.

The sender never advances its source after an unacknowledged DATA packet.

Named defaults are:

| Setting | Default |
| --- | ---: |
| Control response timeout | 500 ms |
| DATA ACK timeout | 250 ms |
| Maximum retransmissions per frame | 5 |
| Receiver inactivity timeout | 10,000 ms |
| Sender overall inactivity timeout | 10,000 ms |

All times use monotonic milliseconds supplied to the state machines. Timeout
and retry constants are centralized in `protocol_v2.hpp`.

## Sender state machine

The sender states are:

- `Idle`: no transfer;
- `Preparing`: validating metadata and resetting the source;
- `WaitingForReady`: START is pending or awaiting READY;
- `SendingData`: named public state reserved for transport integration;
- `WaitingForDataAck`: one DATA sequence is pending or awaiting ACK;
- `WaitingForComplete`: END is pending or awaiting COMPLETE;
- `Completed`: matching COMPLETE received;
- `Cancelling`: CANCEL is pending or awaiting the cancellation result;
- `Cancelled`: cancellation completed or exhausted safely;
- `Failed`: stable error recorded.

Invalid state/event combinations fail deterministically or are explicitly
ignored when they are stale peer traffic. Retry exhaustion is `Failed` with
`RetryExhausted`. Success is possible only in `Completed` with peer COMPLETE.

## Receiver state machine

The receiver states are:

- `Idle`: no active partial;
- `Receiving`: accepting the next contiguous DATA sequence;
- `WaitingForEnd`: all declared DATA accepted;
- `Verifying`: closing and checking count/CRC metadata;
- `Publishing`: collision-safe rename in progress;
- `Completed`: verified file published;
- `Cancelled`: cancellation cleanup completed;
- `Failed`: stable error recorded and cleanup attempted.

The receiver does not publish until all declared packets are present, byte and
packet counts match, END metadata matches START, the sink closes successfully,
the calculated CRC matches, and collision-safe publication succeeds.

## Partial-file and publication policy

Production receives into an internal `.part` file. Internal partials are hidden
from normal file listings and stale partials are removed at startup. Open,
write, flush/close, rename, and cleanup outcomes are checked.

A failed, cancelled, timed-out, truncated, or CRC-mismatched transfer cannot
appear as completed. Existing completed files are never deleted to make room.
Publication selects a collision-free completed name and atomically renames the
verified partial. Repeated cleanup is safe.

## Error codes

The stable error enumeration includes busy, unsupported version/size, invalid
metadata/frame, wrong transfer, unexpected packet/sequence, retry exhaustion,
timeout, cancellation, source read, sink open/write/close, CRC mismatch,
publication failure, cleanup failure, byte/packet-count mismatch, transport
failure, and state violation. Numeric values are defined by
`ProtocolV2::ErrorCode` and must not be renumbered without a protocol version
change.

## Known limitations and deferred work

- Stop-and-wait prioritizes correctness over throughput. A future sliding
  window may improve throughput without changing public completion semantics.
- The protocol does not provide encryption, authentication, or adversarial
  tamper resistance.
- The 16-bit DATA sequence limits one transfer to 1,310,720 bytes.
- Only one receive transfer is active at a time.
- Physical two-board loss, range, interference, reset, and soak testing remains
  required after host and firmware-build validation.
- Hardware-profile pin correction, stable toolchain pinning, and Wi-Fi/HTTP
  security cleanup are separate tasks.
