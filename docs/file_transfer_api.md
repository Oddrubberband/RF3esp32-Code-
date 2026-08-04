# Subsystem file-transfer API

The header include/file_transfer_service.hpp is the hardware-independent
integration surface for Protocol v2. It provides:

- startTransfer(IDataSource&, TransferMetadata, now_ms);
- cancelTransfer(transfer_id, now_ms);
- getTransferStatus(transfer_id, status, now_ms);
- registerReceiveHandler(handler, context).

The API is fixed-memory. StreamingDataSource adapts a rewindable SPIFFS file or
generated stream through explicit reset/read callbacks. Those callbacks
distinguish bytes read, clean EOF, and error. BoundedMemoryDataSource references
caller-owned storage and rejects data above its explicit bound; it does not
copy or allocate the complete input.

TransferMetadata carries an optional logical filename, descriptive media type,
optional expected length, collision policy, and caller context. Media type does
not change transport behavior: .u8 audio remains supported as an ordinary
binary stream. Protocol v2 does not currently carry filenames or media types
over the radio, so received metadata contains the declared length and transport
values while the published path is assigned locally.

TransferStatus exposes the transfer ID, role, lifecycle state, byte and packet
progress, sequence, retry count, stable error, CRC32, verified peer completion,
final published path, and monotonic timing.

The completion handler runs synchronously only after receiver size/packet/CRC
verification and collision-safe publication. Production invokes it from the
radio RX FreeRTOS task, never an interrupt service routine. Handler code must
return promptly and must hand off blocking work to its own task.

The source object must outlive an active sender session. A future sink API can
mirror IDataSource and the current receiver storage callbacks without a wire
protocol change.

Optional transfer rate limiting is expressed in generic bytes per second with
RF3_TRANSFER_RATE_LIMIT_BPS. It defaults to zero (disabled); no audio sample
rate controls Protocol v2.
