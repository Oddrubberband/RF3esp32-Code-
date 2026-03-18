#include "nrf24.hpp"

Nrf24::Nrf24(Nrf24Hal& hal)
    : hal_(hal),
      static_payload_size_(32)
{
}

bool Nrf24::probe()
{
    // TODO:
    // Read/write a few known registers and verify response
    return false;
}

bool Nrf24::initDefaults(uint8_t channel)
{
    (void)channel;

    // TODO:
    // Set CRC, channel, payload width, retries, power state, etc.
    return false;
}

uint8_t Nrf24::getStatus()
{
    // TODO:
    // Send NOP command and return STATUS
    return 0;
}

uint8_t Nrf24::readReg(uint8_t reg)
{
    (void)reg;

    // TODO:
    return 0;
}

void Nrf24::readRegs(uint8_t reg, uint8_t* out, size_t len)
{
    (void)reg;
    (void)out;
    (void)len;

    // TODO
}

void Nrf24::writeReg(uint8_t reg, uint8_t value)
{
    (void)reg;
    (void)value;

    // TODO
}

void Nrf24::writeRegs(uint8_t reg, const uint8_t* data, size_t len)
{
    (void)reg;
    (void)data;
    (void)len;

    // TODO
}

void Nrf24::flushTx()
{
    // TODO
}

void Nrf24::flushRx()
{
    // TODO
}

void Nrf24::clearIrq(bool rx_dr, bool tx_ds, bool max_rt)
{
    (void)rx_dr;
    (void)tx_ds;
    (void)max_rt;

    // TODO
}

bool Nrf24::powerUp()
{
    // TODO
    return false;
}

bool Nrf24::powerDown()
{
    // TODO
    return false;
}

bool Nrf24::startRx()
{
    // TODO
    return false;
}

bool Nrf24::stopRx()
{
    // TODO
    return false;
}

bool Nrf24::transmitOnce(const uint8_t* payload, size_t len, uint32_t timeoutUs)
{
    (void)payload;
    (void)len;
    (void)timeoutUs;

    // TODO
    return false;
}

bool Nrf24::readOnePacket(uint8_t* out, size_t capacity, size_t& outLen)
{
    (void)out;
    (void)capacity;

    outLen = 0;

    // TODO
    return false;
}

bool Nrf24::startContinuousCarrier(uint8_t channel, uint8_t rfPowerBits)
{
    (void)channel;
    (void)rfPowerBits;

    // TODO
    return false;
}

void Nrf24::stopContinuousCarrier()
{
    // TODO
}

void Nrf24::setStaticPayloadSize(uint8_t size)
{
    static_payload_size_ = size;
}

uint8_t Nrf24::staticPayloadSize() const
{
    return static_payload_size_;
}