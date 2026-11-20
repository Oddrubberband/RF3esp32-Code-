#include "fake_hal.hpp"
#include "nrf24.hpp"
#include <cassert>
#include <cstdio>

class TimedFemHal : public FakeHal {
public:
    FrontEndMode mode = FrontEndMode::Off;
    uint64_t launched = 0, completion = 0, powered_down = 0;
    bool pending = false, omit_done = false;
    uint8_t forced_status = 0;
    unsigned selections = 0, launch_count = 0;
    void frontEnd(FrontEndMode next) override {
        assert(!ce_level);
        if (mode == FrontEndMode::Tx && pending && next != FrontEndMode::Tx) {
            assert(powered_down && time_us >= powered_down + 1500);
            pending = false;
        }
        mode = next;
        ++selections;
        time_us += 5;
    }
    void spiTxRx(const uint8_t* tx, uint8_t* rx, size_t n) override {
        time_us += 3;
        if (pending && time_us >= completion && !omit_done) {
            pending = false;
            regs[7] |= 0x20;
        }
        FakeHal::spiTxRx(tx, rx, n);
        if (n >= 2 && tx[0] == 0x20 && !(tx[1] & 2) && pending) powered_down = time_us;
        if (tx[0] == 0xff && forced_status && pending) rx[0] = forced_status;
    }
    void ce(bool high) override {
        if (high) {
            assert(mode == ((regs[0] & 1) ? FrontEndMode::Rx : FrontEndMode::Tx));
            if (!(regs[0] & 1) && !(regs[6] & 0x80)) {
                assert(!pending);
                pending = true;
                launched = time_us;
                completion = launched + 1446;
                powered_down = 0;
                ++launch_count;
                FakeHal::ce(high);
                regs[7] &= ~0x20;
                tx_fifo.clear(); // FIFO dequeues long before the last on-air bit.
                syncFifoStatus();
                return;
            }
        }
        FakeHal::ce(high);
    }
};

int main() {
    TimedFemHal hal;
    Nrf24 radio(hal);
    assert(radio.initDefaults(76));
    assert(hal.mode == FrontEndMode::Off && !hal.ce_level);
    assert(!radio.initDefaults(84));
    assert(radio.startRx() && hal.mode == FrontEndMode::Rx && hal.ce_level);
    assert(radio.stopRx() && hal.mode == FrontEndMode::Off && !hal.ce_level);
    uint8_t data[32]{};
    assert(radio.transmitOnce(data,32,5000,false));
    assert(hal.time_us >= hal.completion);
    assert(hal.mode == FrontEndMode::Off && !hal.ce_level);
    assert(radio.startRx());
    assert(radio.transmitOnce(data,32,5000,false));
    assert(hal.mode == FrontEndMode::Off);
    assert(radio.startRx());
    assert(radio.powerDown() && hal.mode == FrontEndMode::Off && !hal.ce_level);
    hal.omit_done = true;
    assert(!radio.transmitOnce(data,32,3000,false));
    assert(hal.mode == FrontEndMode::Off && !hal.ce_level);
    hal.omit_done = false;
    for (auto status : {0xff,0x3e,0x1e}) {
        hal.forced_status = status;
        assert(!radio.transmitOnce(data,32,3000,false));
        assert(hal.mode == FrontEndMode::Off && !hal.ce_level);
    }
    hal.forced_status = 0;
    hal.omit_done = true;
    auto launches = hal.launch_count;
    assert(!radio.transmitOnce(data,32,3000,true));
    assert(hal.launch_count == launches+2 && hal.mode == FrontEndMode::Off);
    hal.omit_done = false;
    for (unsigned i = 0; i < 3; ++i) {
        assert(radio.startContinuousCarrier(76,6));
        assert(hal.mode == FrontEndMode::Tx && hal.ce_level && (hal.regs[6]&0x90)==0x90);
        radio.stopContinuousCarrier();
        assert(hal.mode == FrontEndMode::Off && !hal.ce_level && !(hal.regs[6]&0x90));
    }
    assert(radio.startContinuousCarrier(76,0));
    assert(radio.powerDown());
    assert(hal.mode == FrontEndMode::Off && !hal.ce_level);
    assert(radio.startContinuousCarrier(76,0));
    assert(radio.startRx());
    assert(hal.mode == FrontEndMode::Rx && hal.ce_level && !(hal.regs[6]&0x90));
    assert(radio.stopRx());
    hal.supports_cont_wave = false;
    assert(!radio.startContinuousCarrier(76,0));
    assert(hal.mode == FrontEndMode::Off && !hal.ce_level && !hal.tx_reuse);
    assert(!radio.startContinuousCarrier(84,0));
    std::puts("PASS automatic FEM startup/RX/TX/CW/stop/failure/recovery, TX_DS-only completion and carrier readback.");
}
