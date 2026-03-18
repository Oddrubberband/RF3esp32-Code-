#pragma once

#include <cstddef>
#include <cstdint>
#include "nrf24_hal.hpp"

// Low-level nRF24L01+ driver.
// This class should handle register access, FIFO commands,
// power/mode control, and packet-level radio operations.

class Nrf24 {
public:
    // TODO: constructor that accepts a Nrf24Hal reference

    // TODO: basic bring-up / probe functions
    // TODO: register read/write functions
    // TODO: FIFO helper functions
    // TODO: IRQ/status helper functions
    // TODO: TX/RX control functions
    // TODO: CW/test mode functions

private:
    // TODO: store reference to HAL
    // TODO: add constants/helpers as needed
};