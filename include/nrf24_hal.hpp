#pragma once

#include <cstddef>
#include <cstdint>

// Nrf24Hal is the portability seam between the driver and the host platform.
//
// The nRF24 driver only needs four services: Serial Peripheral Interface (SPI)
// transfers, Chip Enable (CE) control, small delays, and a microsecond timer.
// Native tests can provide a fake implementation, while the firmware provides
// an ESP32-backed implementation.
struct Nrf24Hal {
    virtual ~Nrf24Hal() = default;

    // Shift n bytes across the SPI bus.
    virtual void spiTxRx(const uint8_t* tx, uint8_t* rx, size_t n) = 0;
    // Drive the radio's Chip Enable (CE) line.
    virtual void ce(bool level) = 0;
    // Busy-wait for short hardware timing windows.
    virtual void delayUs(uint32_t us) = 0;
    // Return a microsecond-resolution timer for timeout loops.
    virtual uint64_t nowUs() = 0;
};
