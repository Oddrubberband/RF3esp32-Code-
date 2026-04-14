#pragma once

#include "nrf24_hal.hpp"
#include "driver/gpio.h"
#include "driver/spi_master.h"

#ifndef NRF24_SCK_PIN
#define NRF24_SCK_PIN 18
#endif

#ifndef NRF24_MOSI_PIN
#define NRF24_MOSI_PIN 23
#endif

#ifndef NRF24_MISO_PIN
#define NRF24_MISO_PIN 19
#endif

#ifndef NRF24_CE_PIN
#define NRF24_CE_PIN 17
#endif

#ifndef NRF24_CSN_PIN
#define NRF24_CSN_PIN 27
#endif

#ifndef NRF24_IRQ_PIN
#define NRF24_IRQ_PIN 16
#endif

#ifndef NRF24_PINSET_NAME
#define NRF24_PINSET_NAME "pcb"
#endif

// Esp32Nrf24Config collects the board-specific wiring choices for the radio.
//
// The defaults assume the ESP32's common Serial Peripheral Interface (SPI) bus
// pins plus one General-Purpose Input/Output (GPIO) pin for Chip Enable (CE)
// and one GPIO pin for Chip Select Not (CSN). Keeping this in a config struct
// makes it easy to port the code to another board without touching the
// higher-level radio logic.
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
