#include "esp32_nrf24_hal.hpp"

#include "esp_err.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

Esp32Nrf24Hal::Esp32Nrf24Hal(const Esp32Nrf24Config& config)
    : config_(config)
{
    gpio_config_t io_conf{};
    io_conf.pin_bit_mask = (1ULL << config_.ce_pin);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    gpio_set_level(config_.ce_pin, 0);

    spi_bus_config_t buscfg{};
    buscfg.sclk_io_num = config_.sck_pin;
    buscfg.mosi_io_num = config_.mosi_pin;
    buscfg.miso_io_num = config_.miso_pin;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;

    ESP_ERROR_CHECK(spi_bus_initialize(config_.host, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg{};
    devcfg.clock_speed_hz = config_.spi_clock_hz;
    devcfg.mode = 0;
    devcfg.spics_io_num = config_.csn_pin;
    devcfg.queue_size = 1;

    ESP_ERROR_CHECK(spi_bus_add_device(config_.host, &devcfg, &spi_));
}

void Esp32Nrf24Hal::spiTxRx(const uint8_t* tx, uint8_t* rx, size_t n)
{
    if (n == 0) {
        return;
    }

    spi_transaction_t t{};
    t.length = static_cast<uint32_t>(n * 8);
    t.tx_buffer = tx;
    t.rx_buffer = rx;

    ESP_ERROR_CHECK(spi_device_transmit(spi_, &t));
}

void Esp32Nrf24Hal::ce(bool level)
{
    gpio_set_level(config_.ce_pin, level ? 1 : 0);
}

void Esp32Nrf24Hal::delayUs(uint32_t us)
{
    esp_rom_delay_us(us);
}

uint64_t Esp32Nrf24Hal::nowUs()
{
    return static_cast<uint64_t>(esp_timer_get_time());
}
