// Scripted peer timing against the real driver. No physical radio is accessed.
#include "fake_hal.hpp"
#include "nrf24.hpp"
#include "radio_manager.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>

class ExchangeHal : public FakeHal {
public:
    FrontEndMode mode = FrontEndMode::Off;
    uint64_t tx_end = 0, tx_ce = 0, rx_ready = 0, powered_down = 0;
    uint64_t reply_start = 0, reply_end = 0, reply_delay = 5000;
    bool tx_pending = false, reply_pending = false, reply_captured = false;
    bool omit_done = false, corrupt_rx_config = false, reply_enabled = false;
    uint64_t irq_delay = 0;
    unsigned rx_flushes = 0;

    void events() {
        if (tx_pending && time_us >= tx_end && !omit_done) {
            tx_pending = false;
            regs[7] |= 0x20;
        }
        if (reply_pending && time_us >= reply_start && !reply_captured) {
            reply_captured = ce_level && (regs[0] & 3) == 3 &&
                mode == FrontEndMode::Rx && rx_ready <= reply_start;
            if (!reply_captured) reply_pending = false;
        }
        if (reply_pending && reply_captured && time_us >= reply_end) {
            rx_fifo_packets.push_back(std::vector<uint8_t>(32, 0xA5));
            regs[7] |= 0x40;
            syncFifoStatus();
            reply_pending = false;
        }
    }
    void advance(uint64_t elapsed) { time_us += elapsed; events(); }
    void frontEnd(FrontEndMode next) override {
        assert(!ce_level);
        if (mode == FrontEndMode::Tx && tx_pending && next != FrontEndMode::Tx) {
            assert(powered_down && time_us >= powered_down + 1500);
            tx_pending = false;
        }
        mode = next;
        advance(next == FrontEndMode::Off ? 5 : 10);
    }
    void spiTxRx(const uint8_t* tx, uint8_t* rx, size_t n) override {
        advance(n * 8); // One MHz SPI, excluding unmodelled software overhead.
        FakeHal::spiTxRx(tx, rx, n);
        if (tx[0] == 0xE2) ++rx_flushes;
        if (n >= 2 && tx[0] == 0x20) {
            if (!(tx[1] & 2) && tx_pending) powered_down = time_us;
            if (corrupt_rx_config && (tx[1] & 3) == 3) regs[0] &= ~1u;
        }
    }
    void ce(bool high) override {
        events();
        const bool rising = high && !ce_level;
        const bool tx = !(regs[0] & 1);
        FakeHal::ce(high);
        if (rising && !tx) {
            assert(mode == FrontEndMode::Rx);
            rx_ready = time_us + 130;
        }
        if (rising && tx && !(regs[6] & 0x80)) {
            assert(mode == FrontEndMode::Tx);
            tx_ce = time_us;
            tx_end = time_us + 130 + 1280;
            powered_down = 0;
            tx_pending = true;
            regs[7] &= ~0x20u;
            tx_fifo.clear();
            syncFifoStatus();
            if (reply_enabled) {
                reply_start = tx_end + reply_delay;
                reply_end = reply_start + 1280;
                reply_pending = true;
                reply_captured = false;
            }
        }
    }
    bool irqAsserted() const override {
        const bool rx_irq = (regs[7] & 0x40) != 0;
        const bool tx_irq = (regs[7] & 0x30) != 0 && time_us >= tx_end + irq_delay;
        return irq_connected && (rx_irq || tx_irq);
    }
    void delayUs(uint32_t us) override { advance(us); }
    uint64_t nowUs() override { events(); return time_us; }
    void queue(uint8_t value) {
        rx_fifo_packets.push_back(std::vector<uint8_t>(32, value));
        regs[7] |= 0x40;
        syncFifoStatus();
    }
};

static void drain(Nrf24& radio, uint8_t expected) {
    uint8_t payload[32]{};
    size_t length = 0;
    assert(radio.readOnePacket(payload, sizeof(payload), length));
    assert(length == 32 && payload[0] == expected);
}

int main() {
    ExchangeHal hal;
    Nrf24 radio(hal);
    RadioManager manager(radio);
    assert(manager.boot(76));
    uint64_t before = hal.time_us;
    assert(manager.enterRx());
    const auto warm_us = hal.time_us - before;
    assert(warm_us < 500);
    const auto flushes = hal.rx_flushes;
    hal.queue(0x31);
    assert(manager.enterRx());
    assert(hal.rx_flushes == flushes);
    drain(radio, 0x31);
    assert(manager.leaveRx());
    hal.queue(0x32);
    assert(manager.enterRx());
    assert(hal.rx_fifo_packets.empty() && hal.rx_flushes == flushes + 1);
    assert(manager.powerDown());
    before = hal.time_us;
    assert(manager.enterRx());
    const auto cold_us = hal.time_us - before;
    assert(cold_us >= 4500 + 150);

    uint8_t payload[32]{};
    hal.reply_enabled = true;
    for (uint64_t irq_delay : {0u, 3000u}) {
        hal.irq_delay = irq_delay;
        assert(manager.sendPayloadAndListen(payload, sizeof(payload), false));
        assert(manager.status().state == RadioState::RxListening);
        assert(hal.mode == FrontEndMode::Rx && hal.rx_ready < hal.reply_start);
        const auto queued_flushes = hal.rx_flushes;
        hal.advance(hal.reply_end > hal.time_us ? hal.reply_end - hal.time_us : 0);
        assert(manager.enterRx()); // Must preserve an ACK that already arrived.
        assert(hal.rx_flushes == queued_flushes);
        drain(radio, 0xA5);
    }

    // Same receipt-based guard protects receiver ACKs and the sender's next DATA.
    hal.reply_enabled = false;
    for (const char* direction : {"ACK reply", "next DATA"}) {
        hal.queue(0x44);
        drain(radio, 0x44);
        const auto drained = hal.time_us;
        assert(manager.leaveRx());
        assert(manager.sendPayloadAndListen(payload, sizeof(payload), false));
        assert(hal.tx_ce - drained >= 5000);
        std::printf("%s drain-to-CE %llu us; RX ready %llu us after TX end\n",
            direction, static_cast<unsigned long long>(hal.tx_ce - drained),
            static_cast<unsigned long long>(hal.rx_ready - hal.tx_end));
    }
    hal.queue(0x45);
    drain(radio, 0x45);
    hal.advance(7000); // Existing storage/processing time already satisfies guard.
    before = hal.time_us;
    assert(manager.sendPayloadAndListen(payload, sizeof(payload), false));
    assert(hal.tx_ce - before < 1000);

    hal.queue(0x46);
    drain(radio, 0x46);
    before = hal.time_us;
    assert(manager.sendPayload(payload, sizeof(payload), false)); // No ACK/default path.
    assert(hal.tx_ce - before < 1000);
    assert(hal.mode == FrontEndMode::Off && !hal.ce_level);

    hal.corrupt_rx_config = true;
    const auto launches = hal.tx_trigger_count;
    assert(!manager.sendPayloadAndListen(payload, sizeof(payload), true));
    assert(hal.tx_trigger_count == launches + 1);
    assert(manager.status().state == RadioState::Fault && hal.mode == FrontEndMode::Off);
    hal.corrupt_rx_config = false;
    assert(manager.boot(76) && manager.enterRx());
    hal.regs[5] = 84;
    assert(!manager.enterRx());
    assert(hal.mode == FrontEndMode::Off && !hal.ce_level);
    assert(manager.boot(76) && manager.enterRx());
    hal.regs[0] &= ~2u;
    assert(!manager.enterRx());
    assert(hal.mode == FrontEndMode::Off);

    assert(manager.boot(76));
    assert(manager.startCw(76, 0));
    before = hal.time_us;
    assert(manager.stopCw());
    const auto stopped_cw = hal.time_us - before;
    assert(stopped_cw >= 1500 + 4500);
    assert(manager.enterRx() && hal.mode == FrontEndMode::Rx);
    assert(hal.time_us >= before + 1500 + 4500 + 150);

    std::printf("Warm arm %llu us; cold arm %llu us. PASS reliable reply arm, FIFO retention/stale reset, IRQ fallback, symmetric guard, processing credit, noACK/default bypass and rearm faults.\n",
        static_cast<unsigned long long>(warm_us), static_cast<unsigned long long>(cold_us));
}
