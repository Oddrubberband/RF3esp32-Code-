# RF3 Host Tests

Run the hardware-free unit tests from the repository root with:

```sh
platformio test -e native
```

These tests cover packet encoding/decoding, nRF24 register/FIFO behavior through
`FakeHal`, retry and pacing helpers, RX-drain loop behavior, and stream sequence
gap handling. They do not require an ESP32 or nRF24 module.

Channel preview tests cover nominal MHz at the channel boundaries, the explicit
preview/select distinction, and rejection of malformed or ambiguous commands
before a selection can be acted on. The serial/HTTP handlers still require
on-board verification; the host tests do not run ESP-IDF console/HTTP tasks.

Run the separate quantitative software qualification with:

```sh
python tools/run_firmware_qualification.py
```

`qualification/main.cpp` uses the existing production V2 service/state machines
and `include/protocol_v2_fake_transport.hpp`, with streaming host file callbacks.
It is intentionally outside PlatformIO's `test_*` Unity discovery. See
[the qualification specification](../docs/firmware_qualification.md) for its
measured campaigns and hardware exclusions. This does not replace the Unity
or Python tests.
