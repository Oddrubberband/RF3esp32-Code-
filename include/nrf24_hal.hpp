#pragma once

#include <cstddef>
#include <cstdint>

struct Nrf24Hal {
    virtual ~Nrf24Hal() = default;

    virtual void spiTxRx(const uint8_t* tx, uint8_t* rx, size_t n) = 0;
    virtual void ce(bool level) = 0;
    virtual void delayUs(uint32_t us) = 0;
    virtual uint64_t nowUs() = 0;
};