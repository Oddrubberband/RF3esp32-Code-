# RF3 Host Tests

Run the hardware-free unit tests from the repository root with:

```sh
platformio test -e native
```

These tests cover packet encoding/decoding, nRF24 register/FIFO behavior through
`FakeHal`, retry and pacing helpers, RX-drain loop behavior, and stream sequence
gap handling. They do not require an ESP32 or nRF24 module.

Standalone host transport safety tests (any desktop C++11 compiler):

```sh
g++ -std=c++11 -Wall -Wextra -Werror -Iinclude tools/tests/host_transport_safety.cpp -o host_transport_safety
./host_transport_safety
```

These cover Bluetooth write completion/congestion, connection ownership and
expiry, and file-replacement recovery with injected rename/journal failures.
