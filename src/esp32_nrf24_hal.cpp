#include "esp32_nrf24_hal.hpp"

#include <cstring>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_rom_sys.h"

Esp32Nrf24Hal::Esp32Nrf24Hal(spi_host_device_t host,
                             gpio_num_t sck,
                             gpio_num_t mosi,
                             gpio_num_t miso,
                             gpio_num_t ce_pin,
                             gpio_num_t csn_pin)
    : ce_pin_(ce_pin)
{
    // Configure CE pin as output
    gpio_config_t ce_conf = {};
    ce_conf.pin_bit_mask = (1ULL << ce_pin_);
    ce_conf.mode = GPIO_MODE_OUTPUT;
    ce_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    ce_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    ce_conf.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&ce_conf));
    ESP_ERROR_CHECK(gpio_set_level(ce_pin_, 0));

    // Configure SPI bus
    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num = sck;
    buscfg.mosi_io_num = mosi;
    buscfg.miso_io_num = miso;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = 32;

    ESP_ERROR_CHECK(spi_bus_initialize(host, &buscfg, SPI_DMA_DISABLED));

    // Configure nRF24 as SPI device
    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = 1000000;   // 1 MHz for bring-up
    devcfg.mode = 0;
    devcfg.spics_io_num = csn_pin;
    devcfg.queue_size = 1;

    ESP_ERROR_CHECK(spi_bus_add_device(host, &devcfg, &spi_));
}

void Esp32Nrf24Hal::spiTxRx(const uint8_t* tx, uint8_t* rx, size_t n)
{
    spi_transaction_t t = {};
    t.length = static_cast<size_t>(n * 8);
    t.tx_buffer = tx;
    t.rx_buffer = rx;

    ESP_ERROR_CHECK(spi_device_polling_transmit(spi_, &t));
}

void Esp32Nrf24Hal::ce(bool level)
{
    ESP_ERROR_CHECK(gpio_set_level(ce_pin_, level ? 1 : 0));
}

void Esp32Nrf24Hal::delayUs(uint32_t us)
{
    esp_rom_delay_us(us);
}