#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "nrf24_hal.hpp"

// Tiny nRF24 simulation used by host-side tests to exercise driver logic without hardware.
class FakeHal : public Nrf24Hal {
public:
    FakeHal()
    {
        regs[0x07] = 0x0E;
        syncFifoStatus();
    }

    std::array<uint8_t, 0x20> regs{};
    std::vector<uint8_t> last_tx;
    std::vector<std::vector<uint8_t>> tx_log;
    std::vector<uint8_t> tx_fifo;
    std::vector<std::vector<uint8_t>> rx_fifo_packets;
    std::vector<uint8_t> last_payload_write;
    bool ce_level = false;
    bool irq_connected = true;
    bool next_tx_success = true;
    bool supports_cont_wave = true;
    bool tx_reuse = false;
    uint64_t time_us = 0;
    uint32_t now_step_us = 100;
    int tx_trigger_count = 0;

    void loadRxPayload(std::initializer_list<uint8_t> payload)
    {
        rx_fifo_packets.clear();
        queueRxPayload(payload);
    }

    void queueRxPayload(std::initializer_list<uint8_t> payload)
    {
        // Queue one fixed payload and raise RX_DR so the driver sees data ready.
        rx_fifo_packets.emplace_back(payload.begin(), payload.end());
        regs[0x07] |= (1 << 6);
        syncFifoStatus();
    }

    void syncFifoStatus()
    {
        if (rx_fifo_packets.empty()) {
            regs[0x17] |= (1 << 0);
        } else {
            regs[0x17] &= static_cast<uint8_t>(~(1 << 0));
        }

        if (tx_fifo.empty()) {
            regs[0x17] |= (1 << 4);
        } else {
            regs[0x17] &= static_cast<uint8_t>(~(1 << 4));
        }

        if (tx_reuse) {
            regs[0x17] |= (1 << 6);
        } else {
            regs[0x17] &= static_cast<uint8_t>(~(1 << 6));
        }
    }

    void spiTxRx(const uint8_t* tx, uint8_t* rx, size_t n) override
    {
        last_tx.assign(tx, tx + n);
        tx_log.push_back(last_tx);

        if (rx) {
            for (size_t i = 0; i < n; ++i) {
                rx[i] = 0;
            }
        }

        if (n == 0) {
            return;
        }

        const uint8_t status = regs[0x07];
        if (rx) {
            rx[0] = status;
        }

        const uint8_t cmd = tx[0];

        if (cmd == 0xFF) {
            return;
        }

        // Only the command subset used by the current driver needs to be emulated here.
        if ((cmd & 0xE0) == 0x00) {
            const uint8_t reg = cmd & 0x1F;
            for (size_t i = 1; i < n; ++i) {
                const size_t index = reg + i - 1;
                if (rx && index < regs.size()) {
                    rx[i] = regs[index];
                }
            }
            return;
        }

        if ((cmd & 0xE0) == 0x20) {
            const uint8_t reg = cmd & 0x1F;
            for (size_t i = 1; i < n; ++i) {
                const size_t index = reg + i - 1;
                if (index >= regs.size()) {
                    continue;
                }

                if (index == 0x07) {
                    regs[0x07] &= static_cast<uint8_t>(~(tx[i] & 0x70));
                } else if (index == 0x06 && !supports_cont_wave) {
                    regs[index] = static_cast<uint8_t>(tx[i] & 0x7F);
                } else {
                    regs[index] = tx[i];
                }
            }
            return;
        }

        switch (cmd) {
            case 0x61:
                for (size_t i = 1; i < n; ++i) {
                    if (!rx_fifo_packets.empty() &&
                        i - 1 < rx_fifo_packets.front().size() &&
                        rx) {
                        rx[i] = rx_fifo_packets.front()[i - 1];
                    }
                }
                if (!rx_fifo_packets.empty()) {
                    rx_fifo_packets.erase(rx_fifo_packets.begin());
                }
                if (rx_fifo_packets.empty()) {
                    regs[0x07] &= static_cast<uint8_t>(~(1 << 6));
                }
                syncFifoStatus();
                break;

            case 0xA0:
                last_payload_write.assign(tx + 1, tx + n);
                tx_fifo = last_payload_write;
                tx_reuse = false;
                syncFifoStatus();
                break;

            case 0xE3:
                tx_reuse = true;
                tx_fifo = last_payload_write;
                if (ce_level && !tx_fifo.empty()) {
                    ++tx_trigger_count;
                    if (next_tx_success) {
                        regs[0x07] |= (1 << 5);
                    } else {
                        regs[0x07] |= (1 << 4);
                    }
                }
                syncFifoStatus();
                break;

            case 0xE1:
                tx_fifo.clear();
                tx_reuse = false;
                syncFifoStatus();
                break;

            case 0xE2:
                rx_fifo_packets.clear();
                regs[0x07] &= static_cast<uint8_t>(~(1 << 6));
                syncFifoStatus();
                break;

            default:
                break;
        }
    }

    void ce(bool level) override
    {
        const bool rising = !ce_level && level;
        ce_level = level;

        // In TX mode, a CE rising edge is what actually launches the queued payload.
        const bool prim_rx = (regs[0x00] & (1 << 0)) != 0;
        const bool power_up = (regs[0x00] & (1 << 1)) != 0;

        if (rising && power_up && !prim_rx && !tx_fifo.empty()) {
            ++tx_trigger_count;
            if (next_tx_success) {
                regs[0x07] |= (1 << 5);
                if (!tx_reuse) {
                    tx_fifo.clear();
                }
            } else {
                regs[0x07] |= (1 << 4);
            }
            syncFifoStatus();
        }
    }

    bool irqConnected() const override
    {
        return irq_connected;
    }

    bool irqAsserted() const override
    {
        return irq_connected && (regs[0x07] & 0x70) != 0;
    }

    void delayUs(uint32_t us) override
    {
        time_us += us;
    }

    uint64_t nowUs() override
    {
        time_us += now_step_us;
        return time_us;
    }
};
