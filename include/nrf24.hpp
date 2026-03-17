#pragma once
#include <cstddef>
#include <cstdint>
#include "nrf24_hal.hpp"

class Nrf24 {
public:
    explicit Nrf24(Nrf24Hal& hal) : hal_(hal) {}

    uint8_t status();
    uint8_t readReg(uint8_t reg);
    void writeReg(uint8_t reg, uint8_t value);

    bool begin();

private:
    Nrf24Hal& hal_;
};