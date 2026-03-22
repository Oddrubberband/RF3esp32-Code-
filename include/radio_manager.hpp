#pragma once

#include <cstddef>
#include <cstdint>

#include "nrf24.hpp"

// RadioState names the coarse operating modes the app exposes to the console.
enum class RadioState {
    Boot,         // Radio is being probed or initialized.
    Standby,      // Radio is awake and idle.
    RxListening,  // Radio is actively waiting for payloads.
    TxBusy,       // Radio is transmitting a payload sequence.
    Sleep,        // Radio is in the chip's low-power mode.
    PowerDown,    // Radio has been explicitly powered down by the app.
    CwTest,       // Radio is generating a continuous-wave (CW) carrier for test work.
    Fault         // A recent operation failed; last_fault explains where.
};

// RadioStatus is the app-facing state snapshot shown by STATUS and logs.
struct RadioStatus {
    RadioState state = RadioState::Boot;
    uint8_t last_status = 0;  // Most recently observed STATUS register value.
    bool last_tx_ok = false;  // Whether the most recent one-shot transmit (TX) succeeded.
    size_t last_rx_len = 0;   // Number of bytes returned by the last receive (RX) read.
    int last_fault = 0;       // Small numeric breadcrumb describing the failure point.
    uint8_t channel = 76;     // Current radio-frequency (RF) channel the app believes the radio uses.
};

// RadioManager wraps Nrf24 with a stable state model for the rest of the app.
//
// The underlying driver exposes hardware-oriented primitives. RadioManager adds
// remembered status, fault breadcrumbs, and coarse state transitions so the
// console user interface (UI) does not need to reason about raw register state.
class RadioManager {
public:
    explicit RadioManager(Nrf24& radio);

    // Boot/probe the radio and enter the default standby configuration.
    bool boot(uint8_t channel = 76);
    // Return the latest state snapshot.
    RadioStatus status() const;

    // State transitions used by the console commands.
    bool enterRx();
    bool leaveRx();
    bool hasPendingRx();

    // Data plane operations.
    bool sendPayload(const uint8_t* payload, size_t len);
    bool receivePayload(uint8_t* out, size_t capacity, size_t& outLen);

    // Power controls.
    bool sleep();
    bool wake();
    bool powerDown();

    // Continuous-wave (CW) test controls.
    bool startCw(uint8_t channel, uint8_t rfPowerBits);
    bool stopCw();

    // Helper for human-readable log output.
    static const char* stateName(RadioState state);

private:
    Nrf24& radio_;
    RadioStatus status_{};
};
