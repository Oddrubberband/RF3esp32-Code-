#include "nrf24.hpp"

#include <array>

namespace {
constexpr std::array<uint8_t, 5> kDemoAddress = {0x52, 0x46, 0x33, 0x24, 0x01};
#ifndef RF3_NRF24_RF_SETUP
#define RF3_NRF24_RF_SETUP 0x06
#endif
#ifndef RF3_NRF24_TX_CE_PULSE_US
#define RF3_NRF24_TX_CE_PULSE_US 15
#endif
// Default to 1 Mbps because it is broadly supported across genuine parts and
// clone modules. Boards that reliably support other rates can override this at
// build time with RF3_NRF24_RF_SETUP.
constexpr uint8_t kDemoRfSetup = RF3_NRF24_RF_SETUP;
constexpr uint32_t kTxCePulseUs = RF3_NRF24_TX_CE_PULSE_US;
}

Nrf24::Nrf24(Nrf24Hal& hal)
    : hal_(hal),
      static_payload_size_(32)
{
}

bool Nrf24::probe()
{
    // Probe by writing the radio-frequency channel register (RF_CH) and
    // verifying that the written value can be read back. If this fails, Serial
    // Peripheral Interface (SPI) wiring, power, or chip-select handling is
    // wrong.
    const uint8_t old_channel = readReg(0x05);
    const uint8_t test_value = 0x4B;

    writeReg(0x05, test_value);
    const uint8_t read_back = readReg(0x05);
    writeReg(0x05, old_channel);

    return read_back == test_value;
}

bool Nrf24::initDefaults(uint8_t channel)
{
    if (channel > 125) {
        return false;
    }

    // This demo keeps the radio setup intentionally small:
    // - no auto-ack
    // - one enabled receive (RX) pipe
    // - 5-byte addresses
    // - fixed payload width
    // - fixed demo channel and radio-frequency (RF) setup
    // - no auto retransmit, because this demo does not use ACKs
    //
    // The individual register values are the bare minimum needed for a
    // predictable static-payload test/demo workflow.
    hal_.ce(false);

    writeReg(0x00, 0x0C);
    writeReg(0x01, 0x00);
    writeReg(0x02, 0x01);
    writeReg(0x03, 0x03);
    writeReg(0x04, 0x00);
    writeReg(0x05, channel);
    writeReg(0x06, kDemoRfSetup);
    writeRegs(0x0A, kDemoAddress.data(), kDemoAddress.size());
    writeRegs(0x10, kDemoAddress.data(), kDemoAddress.size());
    writeReg(0x11, static_payload_size_);
    writeReg(0x1C, 0x00);
    writeReg(0x1D, 0x00);

    if (readReg(0x05) != channel) {
        return false;
    }

    clearIrq();
    flushTx();
    flushRx();

    return powerUp();
}

uint8_t Nrf24::getStatus()
{
    // A no-operation (NOP) Serial Peripheral Interface (SPI) transfer returns
    // the current STATUS byte in the first response byte.
    const uint8_t tx[1] = {0xFF};
    uint8_t rx[1] = {0};

    hal_.spiTxRx(tx, rx, 1);
    return rx[0];
}

uint8_t Nrf24::readReg(uint8_t reg)
{
    // A register read is a two-byte Serial Peripheral Interface (SPI) exchange:
    // command byte first, then one
    // dummy byte that clocks back the register value.
    const uint8_t tx[2] = {static_cast<uint8_t>(reg & 0x1F), 0xFF};
    uint8_t rx[2] = {0, 0};

    hal_.spiTxRx(tx, rx, 2);
    return rx[1];
}

uint8_t Nrf24::readRfPowerLevel()
{
    // RF_SETUP bits 2:1 encode the output power level as a small 0-3 value.
    return static_cast<uint8_t>((readReg(0x06) >> 1) & 0x03);
}

uint8_t Nrf24::readRpd()
{
    // The nRF24 Received Power Detector (RPD) latches when in RX mode and the
    // input exceeds the chip's coarse sensitivity threshold.
    return static_cast<uint8_t>(readReg(0x09) & 0x01);
}

void Nrf24::readRegs(uint8_t reg, uint8_t* out, size_t len)
{
    if (!out || len == 0 || len > 32) {
        return;
    }

    // Multi-byte reads work like single-byte reads, but the command is followed
    // by one dummy byte per returned register byte.
    uint8_t tx[33] = {0};
    uint8_t rx[33] = {0};

    tx[0] = static_cast<uint8_t>(reg & 0x1F);
    for (size_t i = 0; i < len; ++i) {
        tx[i + 1] = 0xFF;
    }

    hal_.spiTxRx(tx, rx, len + 1);

    for (size_t i = 0; i < len; ++i) {
        out[i] = rx[i + 1];
    }
}

void Nrf24::writeReg(uint8_t reg, uint8_t value)
{
    // The write command is just the register address with bit 5 set.
    const uint8_t tx[2] = {
        static_cast<uint8_t>(0x20 | (reg & 0x1F)),
        value
    };
    uint8_t rx[2] = {0, 0};

    hal_.spiTxRx(tx, rx, 2);
}

void Nrf24::writeRegs(uint8_t reg, const uint8_t* data, size_t len)
{
    if (!data || len == 0 || len > 32) {
        return;
    }

    // Multi-byte writes are used for address fields or larger payload-like
    // register blocks.
    uint8_t tx[33] = {0};
    uint8_t rx[33] = {0};

    tx[0] = static_cast<uint8_t>(0x20 | (reg & 0x1F));
    for (size_t i = 0; i < len; ++i) {
        tx[i + 1] = data[i];
    }

    hal_.spiTxRx(tx, rx, len + 1);
}

void Nrf24::flushTx()
{
    // The FLUSH_TX command drops any queued transmit (TX) payloads so the next
    // send begins from a clean state.
    const uint8_t tx[1] = {0xE1};
    uint8_t rx[1] = {0};

    hal_.spiTxRx(tx, rx, 1);
}

void Nrf24::flushRx()
{
    // The FLUSH_RX command discards unread receive (RX) payloads when
    // switching modes or recovering from an error.
    const uint8_t tx[1] = {0xE2};
    uint8_t rx[1] = {0};

    hal_.spiTxRx(tx, rx, 1);
}

void Nrf24::clearIrq(bool rx_dr, bool tx_ds, bool max_rt)
{
    // STATUS IRQ bits clear on write-1, so build exactly the mask we want to acknowledge.
    uint8_t value = 0;

    if (rx_dr) {
        value |= (1 << 6);
    }
    if (tx_ds) {
        value |= (1 << 5);
    }
    if (max_rt) {
        value |= (1 << 4);
    }

    writeReg(0x07, value);
}

bool Nrf24::powerUp()
{
    // The power-up bit (PWR_UP) requires a small settling delay before the
    // radio is fully ready.
    uint8_t config = readReg(0x00);
    config |= (1 << 1);
    writeReg(0x00, config);
    hal_.delayUs(1500);
    return true;
}

bool Nrf24::powerDown()
{
    // Power-down is immediate from the driver's perspective; the chip handles
    // the lower-level transition internally.
    uint8_t config = readReg(0x00);
    config &= static_cast<uint8_t>(~(1 << 1));
    writeReg(0x00, config);
    return true;
}

bool Nrf24::startRx()
{
    // Receive (RX) mode is entered by setting the primary-receive bit
    // (PRIM_RX) and then holding Chip Enable (CE) high.
    hal_.ce(false);

    uint8_t config = readReg(0x00);
    config |= (1 << 1);
    config |= (1 << 0);
    writeReg(0x00, config);

    clearIrq();
    flushRx();

    hal_.ce(true);
    hal_.delayUs(150);

    return true;
}

bool Nrf24::stopRx()
{
    // Leaving receive (RX) mode is the inverse: drop Chip Enable (CE) and
    // clear PRIM_RX.
    hal_.ce(false);

    uint8_t config = readReg(0x00);
    config &= static_cast<uint8_t>(~(1 << 0));
    writeReg(0x00, config);

    return true;
}

bool Nrf24::transmitOnce(const uint8_t* payload, size_t len, uint32_t timeoutUs)
{
    if (!payload || len == 0 || len > 32) {
        return false;
    }

    auto attempt_transmit = [&](bool rearm_radio, bool hold_ce_until_done) {
        if (rearm_radio) {
            const uint8_t channel = readReg(0x05);

            hal_.ce(false);
            writeReg(0x00, 0x0C);
            writeReg(0x01, 0x00);
            writeReg(0x02, 0x01);
            writeReg(0x03, 0x03);
            writeReg(0x04, 0x00);
            writeReg(0x05, channel);
            writeReg(0x06, kDemoRfSetup);
            writeRegs(0x0A, kDemoAddress.data(), kDemoAddress.size());
            writeRegs(0x10, kDemoAddress.data(), kDemoAddress.size());
            writeReg(0x11, static_payload_size_);
            writeReg(0x1C, 0x00);
            writeReg(0x1D, 0x00);
            clearIrq();
            flushTx();
            flushRx();
            powerUp();
        }

        last_tx_status_ = getStatus();
        last_tx_fifo_status_ = readReg(0x17);
        last_tx_observe_ = readReg(0x08);
        last_tx_timed_out_ = false;
        last_tx_saw_irq_ = false;

        // Force transmit (TX) mode, queue one payload, then trigger the send
        // with either the normal short CE pulse or the longer hold-high
        // fallback used by some clone modules.
        hal_.ce(false);

        uint8_t config = readReg(0x00);
        config |= (1 << 1);
        config &= static_cast<uint8_t>(~(1 << 0));
        writeReg(0x00, config);

        clearIrq();
        flushTx();

        uint8_t tx[33] = {0};
        uint8_t rx[33] = {0};

        tx[0] = 0xA0;
        for (size_t i = 0; i < len; ++i) {
            tx[i + 1] = payload[i];
        }

        hal_.spiTxRx(tx, rx, len + 1);

        // Leave extra time between the payload write and CE assertion.
        hal_.delayUs(150);
        hal_.ce(true);
        if (!hold_ce_until_done) {
            hal_.delayUs(kTxCePulseUs);
            hal_.ce(false);
        }

                const bool irq_connected = hal_.irqConnected();
        const uint64_t start = hal_.nowUs();
        uint64_t last_status_poll = start;

        while ((hal_.nowUs() - start) < timeoutUs) {
            bool should_read_status = !irq_connected;
            if (irq_connected) {
                const uint64_t now = hal_.nowUs();
                const bool irq_now = hal_.irqAsserted();
                last_tx_saw_irq_ = last_tx_saw_irq_ || irq_now;
                should_read_status = irq_now || ((now - last_status_poll) >= 200);
            }

            if (!should_read_status) {
                hal_.delayUs(25);
                continue;
            }

            last_status_poll = hal_.nowUs();

            const uint8_t status = getStatus();
            const uint8_t fifo_status = readReg(0x17);

            if (status & (1 << 5)) {
                last_tx_status_ = status;
                last_tx_fifo_status_ = fifo_status;
                last_tx_observe_ = readReg(0x08);
                last_tx_timed_out_ = false;
                hal_.ce(false);
                clearIrq(false, true, false);
                return true;
            }

            if (status & (1 << 4)) {
                last_tx_status_ = status;
                last_tx_fifo_status_ = fifo_status;
                last_tx_observe_ = readReg(0x08);
                last_tx_timed_out_ = false;
                hal_.ce(false);
                clearIrq(false, false, true);
                flushTx();
                return false;
            }

            // Some real modules appear to dequeue the payload without ever
            // presenting TX_DS cleanly. In no-ACK mode, an empty TX FIFO after
            // launch is a practical fallback indicator that the one-shot send
            // completed.
            if ((hal_.nowUs() - start) >= 300 && (fifo_status & (1 << 4)) != 0) {
                last_tx_status_ = status;
                last_tx_fifo_status_ = fifo_status;
                last_tx_observe_ = readReg(0x08);
                last_tx_timed_out_ = false;
                hal_.ce(false);
                clearIrq(false, true, true);
                return true;
            }

            hal_.delayUs(25);
        }

        if (irq_connected) {
            last_tx_saw_irq_ = last_tx_saw_irq_ || hal_.irqAsserted();
        }
        last_tx_status_ = getStatus();
        last_tx_fifo_status_ = readReg(0x17);
        last_tx_observe_ = readReg(0x08);
        last_tx_timed_out_ = true;
        hal_.ce(false);
        flushTx();
        return false;
    };

    // Try the datasheet-style CE pulse first. If that stalls, fall back to the
    // stronger re-prime-and-hold path that some modules prefer.
    if (attempt_transmit(false, false)) {
        return true;
    }

    // Some clone modules and long-wire bench setups need a stronger re-prime
    // after cold start or a stalled first transmit. Re-apply the known packet
    // configuration and try once more before reporting failure to the app.
    return attempt_transmit(true, true);
}

bool Nrf24::readOnePacket(uint8_t* out, size_t capacity, size_t& outLen)
{
    outLen = 0;

    if (!out || capacity < static_payload_size_) {
        return false;
    }

    // Treat either RX_DR asserted or RX FIFO not empty as pending data.
    const uint8_t status = getStatus();
    const uint8_t fifo_status = readReg(0x17);
    if ((status & (1 << 6)) == 0 && (fifo_status & 0x01) != 0) {
        return false;
    }

    uint8_t tx[33] = {0};
    uint8_t rx[33] = {0};

    tx[0] = 0x61;
    for (size_t i = 0; i < static_payload_size_; ++i) {
        tx[i + 1] = 0xFF;
    }

    hal_.spiTxRx(tx, rx, static_payload_size_ + 1);

    for (size_t i = 0; i < static_payload_size_; ++i) {
        out[i] = rx[i + 1];
    }

    outLen = static_payload_size_;
    clearIrq(true, false, false);
    return true;
}

bool Nrf24::startContinuousCarrier(uint8_t channel, uint8_t rfPowerBits)
{
    if (channel > 125) {
        return false;
    }

    if (cw_mode_ != CwMode::None) {
        stopContinuousCarrier();
    }

    // Save the live radio configuration so stopContinuousCarrier() can restore
    // the normal packet settings after the bench-test carrier stops.
    cw_restore_.valid = true;
    cw_restore_.config = readReg(0x00);
    cw_restore_.en_aa = readReg(0x01);
    cw_restore_.setup_retr = readReg(0x04);
    cw_restore_.rf_ch = readReg(0x05);
    cw_restore_.rf_setup = readReg(0x06);
    readRegs(0x10, cw_restore_.tx_addr, sizeof(cw_restore_.tx_addr));

    const uint8_t rf_setup_base = static_cast<uint8_t>(cw_restore_.rf_setup & 0x29);
    uint8_t config = cw_restore_.config;
    const bool was_powered = (config & (1 << 1)) != 0;
    config |= (1 << 1);
    config &= static_cast<uint8_t>(~(1 << 0));

    hal_.ce(false);
    writeReg(0x00, config);
    writeReg(0x05, channel);
    writeReg(0x06, static_cast<uint8_t>(rf_setup_base | 0x90 | (rfPowerBits & 0x06)));

    clearIrq();
    flushTx();

    if (!was_powered) {
        hal_.delayUs(1500);
    }

    // Genuine nRF24L01+ parts support CONT_WAVE. If the bit does not stick,
    // fall back to the older payload-reuse sequence so CW still produces RF
    // output on older radios and some clone modules.
    if ((readReg(0x06) & 0x80) != 0) {
        cw_mode_ = CwMode::ContWave;
        hal_.ce(true);
        hal_.delayUs(150);
        return true;
    }

    constexpr std::array<uint8_t, 5> kCarrierAddress = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    constexpr std::array<uint8_t, 32> kCarrierPayload = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };

    config &= static_cast<uint8_t>(~0x0C);
    writeReg(0x00, config);
    writeReg(0x01, 0x00);
    writeReg(0x04, 0x00);
    writeReg(0x05, channel);
    writeReg(0x06, static_cast<uint8_t>(rf_setup_base | 0x10 | (rfPowerBits & 0x06)));
    writeRegs(0x10, kCarrierAddress.data(), kCarrierAddress.size());

    uint8_t tx[33] = {0};
    uint8_t rx[33] = {0};
    tx[0] = 0xA0;
    for (size_t i = 0; i < kCarrierPayload.size(); ++i) {
        tx[i + 1] = kCarrierPayload[i];
    }

    clearIrq();
    flushTx();
    hal_.spiTxRx(tx, rx, sizeof(tx));

    hal_.ce(true);
    hal_.delayUs(15);
    hal_.ce(false);

    const uint64_t start = hal_.nowUs();
    while ((hal_.nowUs() - start) < 2000) {
        const uint8_t status = getStatus();
        if (status & (1 << 5)) {
            clearIrq(false, true, true);

            const uint8_t reuse_tx_payload[1] = {0xE3};
            uint8_t reuse_rx[1] = {0};

            hal_.ce(true);
            hal_.spiTxRx(reuse_tx_payload, reuse_rx, 1);
            cw_mode_ = CwMode::PayloadReuse;
            return true;
        }

        if (status & (1 << 4)) {
            clearIrq(false, false, true);
            flushTx();
            stopContinuousCarrier();
            return false;
        }
    }

    flushTx();
    stopContinuousCarrier();
    return false;
}

void Nrf24::stopContinuousCarrier()
{
    // Leave continuous-wave (CW) mode by dropping Chip Enable (CE), clearing
    // any queued payload reuse, and restoring the packet-oriented demo setup.
    hal_.ce(false);
    clearIrq();
    flushTx();

    if (!cw_restore_.valid) {
        writeReg(0x06, kDemoRfSetup);
        cw_mode_ = CwMode::None;
        return;
    }

    writeReg(0x00, cw_restore_.config);
    writeReg(0x01, cw_restore_.en_aa);
    writeReg(0x04, cw_restore_.setup_retr);
    writeReg(0x05, cw_restore_.rf_ch);
    writeReg(0x06, cw_restore_.rf_setup);
    writeRegs(0x10, cw_restore_.tx_addr, sizeof(cw_restore_.tx_addr));

    cw_restore_ = CwRestoreState{};
    cw_mode_ = CwMode::None;
}

void Nrf24::setStaticPayloadSize(uint8_t size)
{
    // Tests may shrink or expand the fixed payload width. The main firmware
    // keeps this at 32 bytes to maximize payload bytes per packet.
    static_payload_size_ = size;
}

uint8_t Nrf24::staticPayloadSize() const
{
    return static_payload_size_;
}

bool Nrf24::irqConnected() const
{
    return hal_.irqConnected();
}

bool Nrf24::irqAsserted() const
{
    return hal_.irqAsserted();
}

uint8_t Nrf24::lastTxStatus() const
{
    return last_tx_status_;
}

uint8_t Nrf24::lastTxFifoStatus() const
{
    return last_tx_fifo_status_;
}

uint8_t Nrf24::lastTxObserve() const
{
    return last_tx_observe_;
}

bool Nrf24::lastTxTimedOut() const
{
    return last_tx_timed_out_;
}

bool Nrf24::lastTxSawIrq() const
{
    return last_tx_saw_irq_;
}
