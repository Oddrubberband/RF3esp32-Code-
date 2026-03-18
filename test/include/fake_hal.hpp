#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "nrf24_hal.hpp"

class FakeHal : public Nrf24Hal {
public:
    std::vector<uint8_t> last_tx;
    std::vector<uint8_t> next_rx;
    bool ce_level = false;
    uint64_t time_us = 0;

    void spiTxRx(const uint8_t* tx, uint8_t* rx, size_t n) override
    {
        last_tx.assign(tx, tx + n);

        for (size_t i = 0; i < n; ++i) {
            rx[i] = (i < next_rx.size()) ? next_rx[i] : 0;
        }
    }

    void ce(bool level) override
    {
        ce_level = level;
    }

    void delayUs(uint32_t us) override
    {
        time_us += us;
    }

    uint64_t nowUs() override
    {
        return time_us;
    }
};