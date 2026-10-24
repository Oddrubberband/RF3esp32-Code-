# Bluetooth connection handshake correction

The H2 change made host identity independent of radio initialization and shared radio locks. Immutable HELLO reports host/RF protocol, node and profile identity. GUI port opening and handshaking have separate deadlines and visible stages. Bounded writes, response handling and connection generations prevent stale-peer state from completing a new connection. A connection can identify the ESP even when the nRF24 is unpowered; RF controls still require healthy radio telemetry.

The H2 suite passed 87 GUI tests and strict native HELLO tests, both firmware builds and packaged mock startup. Later R2 verification passed 208 GUI tests, including these regressions. Windows physical driver behavior is not established by mocks.

See [the complete change and verification note](../CHANGELOG.md) and [Bluetooth control](bluetooth_spp_control.md).
