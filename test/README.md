# RF3 Host Tests

Run the hardware-free unit tests from the repository root with:

```sh
platformio test -e native
```

These tests cover packet encoding/decoding, nRF24 register/FIFO behavior through
`FakeHal`, retry and pacing helpers, RX-drain loop behavior, and stream sequence
gap handling. They do not require an ESP32 or nRF24 module.
