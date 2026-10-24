# Implemented desktop file flow

The GUI offers two distinct inputs: a selected PC source and files already in ESP SPIFFS. Import stores and verifies a PC source without transmitting. Select on ESP changes the stored-file selection. Transmit Stored sends a selected SPIFFS file without another upload. Export saves a selected ESP file to PC with size/CRC verification and atomic publication; Delete requires confirmation and free ownership.

During RF receive, the Incoming RF panel reports observed state, received bytes, progress, elapsed time and a sampled payload data rate. Repeated Receive preserves an active transfer. Cancel Receive explicitly stops it and retains terminal diagnostics. Receiving and storage operations protect one another through GUI and firmware guards.

RF START metadata preserves the final safe ASCII filename suffix, including case, up to ten bytes including the dot. Both peers require the current firmware for extension-bearing START. Updated receivers also accept legacy START without suffix metadata, using `.bin`. Full original filenames/MIME inference and migration of existing received files are outside scope.

The implemented behavior and exact verification limits are documented in [CHANGELOG](../CHANGELOG.md) and [current verification](verification/current_verification.md).
