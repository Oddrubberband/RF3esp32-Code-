#include <unity.h>
#include "nrf24.hpp"
#include "fake_hal.hpp"

void test_readReg_formats_spi_correctly() {
  FakeHal hal;
  hal.regs[0x07] = 0xAB; // STATUS

  Nrf24 radio(hal);
  uint8_t v = radio.readReg(0x07);

  TEST_ASSERT_EQUAL_UINT8(0xAB, v);
  TEST_ASSERT_EQUAL_UINT8(0x07, hal.lastTx[0]);   // R_REGISTER | 0x07
  TEST_ASSERT_EQUAL_UINT8(0xFF, hal.lastTx[1]);   // NOP
}

void test_powerUp_sets_PWR_UP_bit() {
  FakeHal hal;
  hal.regs[0x00] = 0x00; // CONFIG initially 0

  Nrf24 radio(hal);
  radio.powerUp();

  TEST_ASSERT_TRUE((hal.regs[0x00] & (1 << 1)) != 0);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_readReg_formats_spi_correctly);
  RUN_TEST(test_powerUp_sets_PWR_UP_bit);
  return UNITY_END();
}