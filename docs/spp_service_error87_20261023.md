# Windows SPP error 87 correction

The GUI reuses exactly one outgoing Serial Port Profile mapping for the selected Bluetooth address before requesting redundant activation. Error 87 takes a bounded mapping-discovery fallback. Incoming, ambiguous and other-device ports are rejected. The message no longer incorrectly diagnoses BLE-only hardware.

Verification: the H3 suite passed 91 GUI tests covering address binding, ambiguity and fallback behavior; the final R2 suite passed 208. These controlled tests do not establish every Windows driver failure cause. Both profiles use Classic SPP, not BLE.

See [the complete change and verification note](../CHANGELOG.md).
