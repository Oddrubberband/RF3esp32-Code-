#pragma once

#include <cstddef>
#include <cstdint>

#include "nrf24_hal.hpp"

// Small nRF24 driver layer used by the app and the host-side tests.
class Nrf24 {
public:
    explicit Nrf24(Nrf24Hal& hal);

    bool probe();
    bool initDefaults(uint8_t channel = 76);

    uint8_t getStatus();
    uint8_t readReg(uint8_t reg);
    void readRegs(uint8_t reg, uint8_t* out, size_t len);
    void writeReg(uint8_t reg, uint8_t value);
    void writeRegs(uint8_t reg, const uint8_t* data, size_t len);

    void flushTx();
    void flushRx();
    void clearIrq(bool rx_dr = true, bool tx_ds = true, bool max_rt = true);

    bool powerUp();
    bool powerDown();
    bool startRx();
    bool stopRx();

    bool transmitOnce(const uint8_t* payload, size_t len, uint32_t timeoutUs = 20000);
    bool readOnePacket(uint8_t* out, size_t capacity, size_t& outLen);

    bool startContinuousCarrier(uint8_t channel = 76, uint8_t rfPowerBits = 0x03);
    void stopContinuousCarrier();

    void setStaticPayloadSize(uint8_t size);
    uint8_t staticPayloadSize() const;

private:
    Nrf24Hal& hal_;
    uint8_t static_payload_size_ = 32;
};
