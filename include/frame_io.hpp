#include "esp32_nrf24_hal.hpp"

#include "esp_rom_sys.h"
#include "esp_timer.h"

Esp32Nrf24Hal::Esp32Nrf24Hal(const Esp32Nrf24Config& config)
    : config_(config)
{
    // TODO:
    // 1. configure config_.ce_pin as output
    // 2. initialize SPI bus using:
    //    config_.host
    //    config_.sck_pin
    //    config_.mosi_pin
    //    config_.miso_pin
    // 3. add SPI device using:
    //    config_.csn_pin
    //    config_.spi_clock_hz
}

void Esp32Nrf24Hal::spiTxRx(const uint8_t* tx, uint8_t* rx, size_t n)
{
    (void)tx;
    (void)rx;
    (void)n;

    // TODO
}

void Esp32Nrf24Hal::ce(bool level)
{
    (void)level;

    // TODO
}

void Esp32Nrf24Hal::delayUs(uint32_t us)
{
    esp_rom_delay_us(us);
}

uint64_t Esp32Nrf24Hal::nowUs()
{
    return static_cast<uint64_t>(esp_timer_get_time());
}