#pragma once

#include "nrf24_hal.hpp"
#include "driver/gpio.h"
#include "driver/spi_master.h"

struct Esp32Nrf24Config {
    spi_host_device_t host = SPI3_HOST;

    gpio_num_t sck_pin = GPIO_NUM_18;
    gpio_num_t mosi_pin = GPIO_NUM_23;
    gpio_num_t miso_pin = GPIO_NUM_19;

    gpio_num_t ce_pin = GPIO_NUM_27;
    gpio_num_t csn_pin = GPIO_NUM_17;

    int spi_clock_hz = 1000000;
};

class Esp32Nrf24Hal : public Nrf24Hal {
public:
    explicit Esp32Nrf24Hal(const Esp32Nrf24Config& config);

    void spiTxRx(const uint8_t* tx, uint8_t* rx, size_t n) override;
    void ce(bool level) override;
    void delayUs(uint32_t us) override;
    uint64_t nowUs() override;

private:
    Esp32Nrf24Config config_{};
    spi_device_handle_t spi_ = nullptr;
};
