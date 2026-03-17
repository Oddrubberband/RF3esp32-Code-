#include "nrf24.hpp"

static constexpr uint8_t CMD_R_REGISTER = 0x00;
static constexpr uint8_t CMD_W_REGISTER = 0x20;
static constexpr uint8_t CMD_NOP        = 0xFF;

static constexpr uint8_t REG_CONFIG     = 0x00;
static constexpr uint8_t REG_EN_AA      = 0x01;
static constexpr uint8_t REG_EN_RXADDR  = 0x02;
static constexpr uint8_t REG_SETUP_AW   = 0x03;
static constexpr uint8_t REG_SETUP_RETR = 0x04;
static constexpr uint8_t REG_RF_CH      = 0x05;
static constexpr uint8_t REG_RF_SETUP   = 0x06;
static constexpr uint8_t REG_STATUS     = 0x07;

uint8_t Nrf24::status()
{
    uint8_t tx[1] = { CMD_NOP };
    uint8_t rx[1] = { 0 };
    hal_.spiTxRx(tx, rx, 1);
    return rx[0];
}

uint8_t Nrf24::readReg(uint8_t reg)
{
    uint8_t tx[2] = {
        static_cast<uint8_t>(CMD_R_REGISTER | (reg & 0x1F)),
        0xFF
    };
    uint8_t rx[2] = { 0, 0 };
    hal_.spiTxRx(tx, rx, 2);
    return rx[1];
}

void Nrf24::writeReg(uint8_t reg, uint8_t value)
{
    uint8_t tx[2] = {
        static_cast<uint8_t>(CMD_W_REGISTER | (reg & 0x1F)),
        value
    };
    uint8_t rx[2] = { 0, 0 };
    hal_.spiTxRx(tx, rx, 2);
}

bool Nrf24::begin()
{
    hal_.ce(false);
    hal_.delayUs(5000);

    // Simple, stable bring-up config
    writeReg(REG_EN_AA,      0x00); // disable auto-ack for now
    writeReg(REG_EN_RXADDR,  0x01); // enable pipe 0
    writeReg(REG_SETUP_AW,   0x03); // 5-byte address
    writeReg(REG_SETUP_RETR, 0x00); // disable retries for now
    writeReg(REG_RF_CH,      76);   // 2.476 GHz
    writeReg(REG_RF_SETUP,   0x06); // 1 Mbps, 0 dBm
    writeReg(REG_STATUS,     0x70); // clear IRQ flags
    writeReg(REG_CONFIG,     0x0E); // PWR_UP=1, EN_CRC=1, CRCO=1, TX mode

    hal_.delayUs(5000);

    uint8_t cfg = readReg(REG_CONFIG);
    return (cfg == 0x0E);
}