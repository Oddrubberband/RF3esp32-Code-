#pragma once

#include "nrf24.hpp"

// Higher-level application-facing radio state.
enum class RadioState {
    Boot,
    Standby,
    RxListening,
    TxBusy,
    Sleep,
    PowerDown,
    CwTest,
    Fault
};

// Status structure for the rest of the firmware.
struct RadioStatus {
    // TODO: add fields you want to track, such as:
    // - current state
    // - last status register value
    // - last TX result
    // - last RX length
    // - last fault code
    // - current channel
};

// Higher-level controller for radio behavior.
// This should use Nrf24, not raw SPI.
class RadioManager {
public:
    // TODO: constructor

    // TODO: boot / init sequence
    // TODO: status getter
    // TODO: RX enter/leave
    // TODO: send / receive helpers
    // TODO: sleep / wake / power-down
    // TODO: CW mode helpers

private:
    // TODO: store reference to Nrf24
    // TODO: store RadioStatus
};