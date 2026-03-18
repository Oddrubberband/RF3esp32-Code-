#pragma once

#include <cstddef>
#include <cstdint>
#include "nrf24.hpp"

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

struct RadioStatus {
    RadioState state = RadioState::Boot;
    uint8_t last_status = 0;
    bool last_tx_ok = false;
    size_t last_rx_len = 0;
    int last_fault = 0;
    uint8_t channel = 76;
};

class RadioManager {
public:
    explicit RadioManager(Nrf24& radio);

    bool boot(uint8_t channel = 76);
    RadioStatus status() const;

    bool enterRx();
    bool leaveRx();

    bool sendPayload(const uint8_t* payload, size_t len);
    bool receivePayload(uint8_t* out, size_t capacity, size_t& outLen);

    bool sleep();
    bool wake();
    bool powerDown();

    bool startCw(uint8_t channel, uint8_t rfPowerBits);
    bool stopCw();

    static const char* stateName(RadioState state);

private:
    Nrf24& radio_;
    RadioStatus status_{};
};