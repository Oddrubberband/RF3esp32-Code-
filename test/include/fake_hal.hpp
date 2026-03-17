#pragma once
#include "nrf24_hal.hpp"
#include <array>
#include <vector>
#include <cstring>

struct FakeHal : public Nrf24Hal {
  std::array<uint8_t, 0x20> regs{};          // nRF24 has 0x00..0x1F core regs
  std::vector<uint8_t> lastTx;

  void spiTxRx(const uint8_t* tx, uint8_t* rx, size_t n) override {
    lastTx.assign(tx, tx + n);
    std::memset(rx, 0, n);

    uint8_t cmd = tx[0];

    // R_REGISTER (0x00..0x1F)
    if ((cmd & 0xE0) == 0x00 && n >= 2) {
      uint8_t reg = cmd & 0x1F;
      rx[1] = regs[reg];
      return;
    }

    // W_REGISTER (0x20..0x3F)
    if ((cmd & 0xE0) == 0x20 && n >= 2) {
      uint8_t reg = cmd & 0x1F;
      regs[reg] = tx[1];
      return;
    }
  }

  void ce(bool) override {}
  void delayUs(uint32_t) override {}
};