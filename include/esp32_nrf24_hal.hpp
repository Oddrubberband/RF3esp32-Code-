#pragma once

#include "nrf24_hal.hpp"
#include "hardware_profile.hpp"
#include "driver/gpio.h"
#include "driver/spi_master.h"

#ifndef RF3_HARDWARE_PROFILE_ID
#error "Select an RF3 hardware profile through a PlatformIO firmware environment"
#endif

// Esp32Nrf24Config collects the board-specific wiring choices for the radio.
//
// Pin values are compile-time checked against the selected named profile in
// hardware_profile.hpp so an incomplete or mixed pin assignment cannot build.
struct Esp32Nrf24Config {
    static constexpr gpio_num_t kNoIrqPin = static_cast<gpio_num_t>(-1);

    spi_host_device_t host = SPI3_HOST;  // ESP-IDF Serial Peripheral Interface (SPI) host used to talk to the radio.

    gpio_num_t sck_pin  = static_cast<gpio_num_t>(NRF24_SCK_PIN);   // SPI clock driven by the ESP32.
    gpio_num_t mosi_pin = static_cast<gpio_num_t>(NRF24_MOSI_PIN);  // Master-out, slave-in data line.
    gpio_num_t miso_pin = static_cast<gpio_num_t>(NRF24_MISO_PIN);  // Master-in, slave-out data line.

    gpio_num_t ce_pin   = static_cast<gpio_num_t>(NRF24_CE_PIN);    // Chip enable: controls receive/transmit state transitions.
    gpio_num_t csn_pin  = static_cast<gpio_num_t>(NRF24_CSN_PIN);   // SPI chip select for register and payload access.
    gpio_num_t irq_pin  = static_cast<gpio_num_t>(NRF24_IRQ_PIN);   // Active-low interrupt pin from the nRF24; set to kNoIrqPin if your module does not wire IRQ.

    int spi_clock_hz = 1000000;          // Conservative bus speed for reliable module bring-up.
};

// Esp32Nrf24Hal is the Espressif IoT Development Framework (ESP-IDF)-specific
// bridge that satisfies the generic Nrf24Hal interface used by the driver code.
class Esp32Nrf24Hal : public Nrf24Hal {
public:
    explicit Esp32Nrf24Hal(const Esp32Nrf24Config& config);

    // Transfer n bytes on SPI while optionally collecting the response.
    void spiTxRx(const uint8_t* tx, uint8_t* rx, size_t n) override;
    // Toggle the nRF24 Chip Enable (CE) pin.
    void ce(bool level) override;
    // Report whether the optional nRF24 IRQ signal is wired.
    bool irqConnected() const override;
    // Return true while the active-low IRQ line is asserted.
    bool irqAsserted() const override;
    // Busy-wait for a small number of microseconds during radio state changes.
    void delayUs(uint32_t us) override;
    // Return a microsecond timer suitable for timeout loops.
    uint64_t nowUs() override;

private:
    Esp32Nrf24Config config_{};
    spi_device_handle_t spi_ = nullptr;  // ESP-IDF handle for the attached radio device.
};
