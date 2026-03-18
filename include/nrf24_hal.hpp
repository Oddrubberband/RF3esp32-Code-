#pragma once

#include <cstddef>
#include <cstdint>

// Hardware abstraction layer for the nRF24 driver.
// This should describe only the operations the radio driver needs,
// not any ESP32-specific details.

struct Nrf24Hal {
    virtual ~Nrf24Hal() = default;

    // TODO: add SPI transfer function
    // TODO: add CE pin control function
    // TODO: add timing helper(s)
};