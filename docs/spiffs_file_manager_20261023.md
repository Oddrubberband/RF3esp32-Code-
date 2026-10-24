# SPIFFS file manager

Refresh lists visible files and sizes using structured pagination. Selecting a stored file and Select on ESP makes it the ESP selection; Transmit Stored sends that file without re-uploading a PC file. A PC source selection is separate.

Import stores a PC file without RF transmission, checks CRC, then reads it back to compare exact bytes. Export checks the received size and CRC before atomically publishing a PC destination through a unique temporary file. Delete requires confirmation and refuses files while the filesystem or radio transfer owns them. Internal/partial/path traversal names are rejected; UTF-8 hex commands preserve valid spaces and Unicode.

Verification: S1 passed 104 GUI tests and native storage/fragmentation checks. P1 added bounded buffered reads and negotiated 64/256-byte chunks; R1/R2 extended ownership, timeout, selection and receive protection checks. The final GUI suite passed 208 tests. One user-operated round trip produced a byte-identical 411,014-byte original/exported PDF with matching SHA-256. The document and fingerprint remain private; this is not a full filesystem wear or RF reliability qualification.

See [the complete change and verification note](../CHANGELOG.md).
