#pragma once

#include "nrf24_hal.hpp"
#include "driver/gpio.h"
#include "driver/spi_master.h"

class Esp32Nrf24Hal : public Nrf24Hal {
public:
    Esp32Nrf24Hal(spi_host_device_t host,
                  gpio_num_t sck,
                  gpio_num_t mosi,
                  gpio_num_t miso,
                  gpio_num_t ce_pin,
                  gpio_num_t csn_pin);

    ~Esp32Nrf24Hal() override = default;

    void spiTxRx(const uint8_t* tx, uint8_t* rx, size_t n) override;
    void ce(bool level) override;
    void delayUs(uint32_t us) override;

private:
    spi_device_handle_t spi_ = nullptr;
    gpio_num_t ce_pin_;
};