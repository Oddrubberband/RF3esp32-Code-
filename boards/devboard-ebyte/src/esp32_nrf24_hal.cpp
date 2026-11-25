#include "esp32_nrf24_hal.hpp"
#include <cstring>
#include <initializer_list>

#include "esp_err.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#if RF3_AUTOMATIC_FEM
namespace {
constexpr gpio_num_t kCtx = GPIO_NUM_32, kCrx = GPIO_NUM_33, kCsd = GPIO_NUM_14;
constexpr gpio_num_t kCps = GPIO_NUM_13, kAnt = GPIO_NUM_17;
constexpr gpio_num_t kFemPins[] = {kCtx, kCrx, kCsd, kCps, kAnt};
static_assert(RF3_HARDWARE_PROFILE_ID == 2, "Automatic FEM requires the explicit devboard profile");
}
#endif

// The constructor performs all one-time Espressif IoT Development Framework
// (ESP-IDF) hardware setup needed by the generic nRF24 driver: configure Chip
// Enable (CE) as a General-Purpose Input/Output (GPIO) pin, then create a
// Serial Peripheral Interface (SPI) device whose chip-select (CS) line is
// driven automatically by the SPI master.
Esp32Nrf24Hal::Esp32Nrf24Hal(const Esp32Nrf24Config& config)
    : config_(config)
{
#if RF3_AUTOMATIC_FEM
    gpio_config_t fem{};
    fem.mode = GPIO_MODE_OUTPUT;
    for (auto pin : kFemPins) {
        ESP_ERROR_CHECK(gpio_set_level(pin, 0));
        fem.pin_bit_mask |= 1ULL << pin;
    }
    ESP_ERROR_CHECK(gpio_config(&fem));
    frontEnd(FrontEndMode::Off);
#endif
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
    if (!signal_outputs_enabled_) {
        if (rx) std::memset(rx, 0xFF, n);
        return;
    }
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
    const bool requested=level && signal_outputs_enabled_;
    if (gpio_set_level(config_.ce_pin,requested ? 1 : 0)==ESP_OK && requested!=ce_asserted_) {
        ce_asserted_=requested;
        last_ce_activity_ms_=static_cast<uint64_t>(esp_timer_get_time())/1000u;
    }
}

bool Esp32Nrf24Hal::setSignalOutputsEnabled(bool enabled)
{
    // ESP-IDF GPIO direction control preserves SPI matrix routing. Release
    // outputs before rail OFF to avoid driving an unpowered nRF via its inputs.
    // RF Protocol v2 remains unchanged; caller owns the radio mutex.
    ce(false);
    frontEnd(FrontEndMode::Off);
    signal_outputs_enabled_ = false;
    const gpio_mode_t mode = enabled ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT;
    bool ok = true;
    for (gpio_num_t pin : {config_.csn_pin, config_.sck_pin, config_.mosi_pin}) {
        if (gpio_set_direction(pin, mode) != ESP_OK) ok = false;
    }
    signal_outputs_enabled_ = enabled && ok;
    return ok;
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

void Esp32Nrf24Hal::frontEnd(FrontEndMode mode)
{
#if RF3_AUTOMATIC_FEM
    // CPS first removes gain. Break both directions before enabling the next path.
    ESP_ERROR_CHECK(gpio_set_level(kCps, 0));
    ESP_ERROR_CHECK(gpio_set_level(kCtx, 0));
    ESP_ERROR_CHECK(gpio_set_level(kCrx, 0));
    ESP_ERROR_CHECK(gpio_set_level(kCsd, 0));
    ESP_ERROR_CHECK(gpio_set_level(kAnt, 0));
    esp_rom_delay_us(5);
    front_end_mode_ = FrontEndMode::Off;
    if (mode != FrontEndMode::Off && signal_outputs_enabled_) {
        ESP_ERROR_CHECK(gpio_set_level(kCsd, 1));
        ESP_ERROR_CHECK(gpio_set_level(mode == FrontEndMode::Tx ? kCtx : kCrx, 1));
        // SE2436L page 5 Logic Controls: CPS selects receive gain, while TX always uses the PA.
        ESP_ERROR_CHECK(gpio_set_level(kCps, mode == FrontEndMode::Tx || !fem_lna_bypass_.load() ? 1 : 0));
        esp_rom_delay_us(5);
        front_end_mode_ = mode;
    }
#else
    (void)mode;
#endif
}

const char* Esp32Nrf24Hal::femLna() const
{
#if RF3_AUTOMATIC_FEM
    return fem_lna_bypass_.load() ? "bypass" : "gain";
#else
    return nullptr;
#endif
}

bool Esp32Nrf24Hal::selectFemLna(bool bypass)
{
#if RF3_AUTOMATIC_FEM
    // The console also requires standby and reserves against receive ownership.
    if (!signal_outputs_enabled_ || ce_asserted_ || front_end_mode_ != FrontEndMode::Off)
        return false;
    fem_lna_bypass_.store(bypass);
    return true;
#else
    (void)bypass;
    return false;
#endif
}
