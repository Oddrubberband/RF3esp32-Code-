#pragma once

#include <cstddef>
#include <cstdint>

#include "nrf24_hal.hpp"

// Nrf24 is a small driver wrapper around the subset of the chip used by the
// demo application.
//
// The class deliberately exposes register helpers and a few high-level radio
// actions instead of trying to be a complete nRF24L01+ driver. That keeps the
// code focused on the static-payload audio demo and the associated tests.
class Nrf24 {
public:
    explicit Nrf24(Nrf24Hal& hal);

    // Basic bring-up helpers.
    bool probe();
    bool initDefaults(uint8_t channel = 76);

    // Low-level register access used both internally and by diagnostics.
    uint8_t getStatus();
    uint8_t readReg(uint8_t reg);
    uint8_t readRfPowerLevel();
    uint8_t readRpd();
    void readRegs(uint8_t reg, uint8_t* out, size_t len);
    void writeReg(uint8_t reg, uint8_t value);
    void writeRegs(uint8_t reg, const uint8_t* data, size_t len);

    // First-in, first-out (FIFO) queue and interrupt housekeeping.
    void flushTx();
    void flushRx();
    void clearIrq(bool rx_dr = true, bool tx_ds = true, bool max_rt = true);

    // Coarse radio state controls.
    bool powerUp();
    bool powerDown();
    bool startRx();
    bool stopRx();

    // One-shot payload send/receive helpers used by the console demo.
    bool transmitOnce(const uint8_t* payload, size_t len, uint32_t timeoutUs = 20000);
    bool readOnePacket(uint8_t* out, size_t capacity, size_t& outLen);

    // Minimal continuous-wave test helpers.
    bool startContinuousCarrier(uint8_t channel = 76, uint8_t rfPowerBits = 0x03);
    void stopContinuousCarrier();

    // The demo uses a fixed payload width, but tests can override it.
    void setStaticPayloadSize(uint8_t size);
    uint8_t staticPayloadSize() const;
    bool irqConnected() const;
    bool irqAsserted() const;
    uint8_t lastTxStatus() const;
    uint8_t lastTxFifoStatus() const;
    uint8_t lastTxObserve() const;
    bool lastTxTimedOut() const;
    bool lastTxSawIrq() const;

private:
    enum class CwMode {
        None,
        ContWave,
        PayloadReuse
    };

    struct CwRestoreState {
        bool valid = false;
        uint8_t config = 0;
        uint8_t en_aa = 0;
        uint8_t setup_retr = 0;
        uint8_t rf_ch = 0;
        uint8_t rf_setup = 0;
        uint8_t tx_addr[5] = {};
    };

    Nrf24Hal& hal_;
    uint8_t static_payload_size_ = 32;  // Number of bytes expected in each receive (RX) payload read.
    uint8_t last_tx_status_ = 0;
    uint8_t last_tx_fifo_status_ = 0;
    uint8_t last_tx_observe_ = 0;
    bool last_tx_timed_out_ = false;
    bool last_tx_saw_irq_ = false;
    CwMode cw_mode_ = CwMode::None;
    CwRestoreState cw_restore_{};
};
