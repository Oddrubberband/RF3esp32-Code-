#include "nrf24.hpp"

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
    //
    // The individual register values are the bare minimum needed for a
    // predictable static-payload test/demo workflow.
    hal_.ce(false);

    writeReg(0x00, 0x0C);
    writeReg(0x01, 0x00);
    writeReg(0x02, 0x01);
    writeReg(0x03, 0x03);
    writeReg(0x04, 0x03);
    writeReg(0x05, channel);
    writeReg(0x06, 0x06);
    writeReg(0x11, static_payload_size_);

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

    // Force transmit (TX) mode, queue one payload, pulse Chip Enable (CE),
    // then poll until success, failure, or timeout.
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

    hal_.ce(true);
    hal_.delayUs(15);
    hal_.ce(false);

    const uint64_t start = hal_.nowUs();

    // Poll the STATUS register until the chip reports transmit (TX) success,
    // maximum retries, or timeout.
    while ((hal_.nowUs() - start) < timeoutUs) {
        const uint8_t status = getStatus();

        if (status & (1 << 5)) {
            clearIrq(false, true, false);
            return true;
        }

        if (status & (1 << 4)) {
            clearIrq(false, false, true);
            flushTx();
            return false;
        }
    }

    flushTx();
    return false;
}

bool Nrf24::readOnePacket(uint8_t* out, size_t capacity, size_t& outLen)
{
    outLen = 0;

    if (!out || capacity < static_payload_size_) {
        return false;
    }

    // The receive-data-ready flag (RX_DR) indicates at least one payload is
    // waiting in the receive first-in, first-out queue (RX FIFO).
    const uint8_t status = getStatus();
    if ((status & (1 << 6)) == 0) {
        return false;
    }

    // The project currently uses fixed-width payloads, so read the configured
    // number of bytes.
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
    flushRx();
    return true;
}

bool Nrf24::startContinuousCarrier(uint8_t channel, uint8_t rfPowerBits)
{
    if (channel > 125) {
        return false;
    }

    // Minimal continuous-wave (CW) test mode: configure the channel and
    // radio-frequency (RF) power, put the radio in transmit (TX) mode, then
    // keep Chip Enable (CE) asserted so the chip emits an unmodulated carrier.
    hal_.ce(false);

    writeReg(0x05, channel);
    writeReg(0x06, static_cast<uint8_t>(0x90 | (rfPowerBits & 0x06)));

    uint8_t config = readReg(0x00);
    config |= (1 << 1);
    config &= static_cast<uint8_t>(~(1 << 0));
    writeReg(0x00, config);

    clearIrq();
    flushTx();

    hal_.ce(true);
    hal_.delayUs(150);

    return true;
}

void Nrf24::stopContinuousCarrier()
{
    // Leave continuous-wave (CW) mode by dropping Chip Enable (CE) and
    // restoring the normal radio-frequency (RF) setup used by the demo while
    // staying in powered transmit standby.
    hal_.ce(false);
    writeReg(0x06, 0x06);
    clearIrq();
    flushTx();
}

void Nrf24::setStaticPayloadSize(uint8_t size)
{
    // Tests may shrink or expand the fixed payload width. The main firmware
    // keeps this at 32 bytes to maximize audio bytes per packet.
    static_payload_size_ = size;
}

uint8_t Nrf24::staticPayloadSize() const
{
    return static_payload_size_;
}
