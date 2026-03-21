#include "nrf24.hpp"

Nrf24::Nrf24(Nrf24Hal& hal)
    : hal_(hal),
      static_payload_size_(32)
{
}

bool Nrf24::probe()
{
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
    const uint8_t tx[1] = {0xFF};
    uint8_t rx[1] = {0};

    hal_.spiTxRx(tx, rx, 1);
    return rx[0];
}

uint8_t Nrf24::readReg(uint8_t reg)
{
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
    const uint8_t tx[1] = {0xE1};
    uint8_t rx[1] = {0};

    hal_.spiTxRx(tx, rx, 1);
}

void Nrf24::flushRx()
{
    const uint8_t tx[1] = {0xE2};
    uint8_t rx[1] = {0};

    hal_.spiTxRx(tx, rx, 1);
}

void Nrf24::clearIrq(bool rx_dr, bool tx_ds, bool max_rt)
{
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
    uint8_t config = readReg(0x00);
    config |= (1 << 1);
    writeReg(0x00, config);
    hal_.delayUs(1500);
    return true;
}

bool Nrf24::powerDown()
{
    uint8_t config = readReg(0x00);
    config &= static_cast<uint8_t>(~(1 << 1));
    writeReg(0x00, config);
    return true;
}

bool Nrf24::startRx()
{
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

    const uint8_t status = getStatus();
    if ((status & (1 << 6)) == 0) {
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
    flushRx();
    return true;
}

bool Nrf24::startContinuousCarrier(uint8_t channel, uint8_t rfPowerBits)
{
    if (channel > 125) {
        return false;
    }

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
    hal_.ce(false);
    powerDown();
}

void Nrf24::setStaticPayloadSize(uint8_t size)
{
    static_payload_size_ = size;
}

uint8_t Nrf24::staticPayloadSize() const
{
    return static_payload_size_;
}