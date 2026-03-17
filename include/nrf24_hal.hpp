#pragma once
#include <cstddef>
#include <cstdint>

struct Nrf24Hal {
  virtual ~Nrf24Hal() = default;

  // SPI full-duplex transfer of n bytes.
  virtual void spiTxRx(const uint8_t* tx, uint8_t* rx, size_t n) = 0;

  // Control CE pin
  virtual void ce(bool level) = 0;

  // Small delay (for CE pulse / startup)
  virtual void delayUs(uint32_t us) = 0;
};