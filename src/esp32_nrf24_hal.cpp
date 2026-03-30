#include "esp32_nrf24_hal.hpp"

#include "esp_err.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

// The constructor performs all one-time Espressif IoT Development Framework
// (ESP-IDF) hardware setup needed by the generic nRF24 driver: configure Chip
// Enable (CE) as a General-Purpose Input/Output (GPIO) pin, then create a
// Serial Peripheral Interface (SPI) device whose chip-select (CS) line is
// driven automatically by the SPI master.
Esp32Nrf24Hal::Esp32Nrf24Hal(const Esp32Nrf24Config& config)
    : config_(config)
{
    // Chip Enable (CE) is controlled outside normal SPI transfers, so it is a
    // plain output pin that starts low to keep the radio quiescent during
    // setup.
    gpio_config_t io_conf{};
    io_conf.pin_bit_mask = (1ULL << config_.ce_pin);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    gpio_set_level(config_.ce_pin, 0);

    if (config_.irq_pin != Esp32Nrf24Config::kNoIrqPin) {
        gpio_config_t irq_conf{};
        irq_conf.pin_bit_mask = (1ULL << config_.irq_pin);
        irq_conf.mode = GPIO_MODE_INPUT;
        irq_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        irq_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        irq_conf.intr_type = GPIO_INTR_DISABLE;
        ESP_ERROR_CHECK(gpio_config(&irq_conf));
    }

    // Build an SPI bus description from the chosen board wiring.
    spi_bus_config_t buscfg{};
    buscfg.sclk_io_num = config_.sck_pin;
    buscfg.mosi_io_num = config_.mosi_pin;
    buscfg.miso_io_num = config_.miso_pin;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;

    ESP_ERROR_CHECK(spi_bus_initialize(config_.host, &buscfg, SPI_DMA_CH_AUTO));

    // Register the nRF24 as an SPI device. ESP-IDF will assert and release Chip
    // Select Not (CSN) for each transaction automatically.
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

    // The nRF24 protocol is command/response oriented, so each transfer clocks
    // out a small contiguous frame and reads back any simultaneous response.
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

bool Esp32Nrf24Hal::irqConnected() const
{
    return config_.irq_pin != Esp32Nrf24Config::kNoIrqPin;
}

bool Esp32Nrf24Hal::irqAsserted() const
{
    if (!irqConnected()) {
        return false;
    }

    return gpio_get_level(config_.irq_pin) == 0;
}

void Esp32Nrf24Hal::delayUs(uint32_t us)
{
    // The radio requires short guard times between some register writes and
    // Chip Enable (CE) transitions. Busy-waiting is acceptable at these small
    // intervals.
    esp_rom_delay_us(us);
}

uint64_t Esp32Nrf24Hal::nowUs()
{
    // esp_timer_get_time already returns microseconds, which is exactly what the
    // driver's polling loops expect.
    return static_cast<uint64_t>(esp_timer_get_time());
}
