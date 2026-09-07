#include <cstddef>
#include <cstdint>

#include <unity.h>

#include "../include/fake_hal.hpp"
#include "nrf24.hpp"
#include "radio_manager.hpp"

namespace {

class FaultHal : public FakeHal {
public:
    bool floating = false;
    bool override_status = false;
    bool override_fifo = false;
    bool after_launch = false;
    uint8_t status_value = 0xFF;
    uint8_t fifo_value = 0xFF;

    void spiTxRx(const uint8_t* tx, uint8_t* rx, size_t n) override
    {
        FakeHal::spiTxRx(tx, rx, n);
        if (!rx || n == 0 || (after_launch && tx_trigger_count == 0)) {
            return;
        }
        if (floating) {
            for (size_t i = 0; i < n; ++i) {
                rx[i] = 0xFF;
            }
        }
        if (override_status) {
            rx[0] = status_value;
        }
        if (override_fifo && tx[0] == 0x17 && n >= 2) {
            rx[1] = fifo_value;
        }
    }
};

class SilentSuccessHal : public FakeHal {
public:
    void ce(bool level) override
    {
        FakeHal::ce(level);
        // Model the supported clone fallback: FIFO drains without a TX_DS latch.
        regs[0x07] &= static_cast<uint8_t>(~(1u << 5));
    }
};

void test_tx_floating_spi_before_launch_reports_communication_fault(void)
{
    FaultHal hal;
    Nrf24 radio(hal);
    RadioManager manager(radio);
    TEST_ASSERT_TRUE(manager.boot());
    hal.floating = true;
    const uint8_t payload[] = {0x12};

    TEST_ASSERT_FALSE(manager.sendPayload(payload, sizeof(payload)));
    const RadioStatus status = manager.status();
    TEST_ASSERT_EQUAL(static_cast<int>(RadioState::Fault), static_cast<int>(status.state));
    TEST_ASSERT_FALSE(status.last_tx_ok);
    TEST_ASSERT_FALSE(status.last_tx_timed_out);
    TEST_ASSERT_TRUE(radio.lastTxCommunicationFailed());
    TEST_ASSERT_EQUAL_INT(10, status.last_fault);
    TEST_ASSERT_EQUAL_HEX8(0xFF, status.last_status);
    TEST_ASSERT_EQUAL_HEX8(0xFF, status.last_fifo_status);
    TEST_ASSERT_EQUAL_INT(-1, status.power_level);
    TEST_ASSERT_EQUAL_INT(0, hal.tx_trigger_count);
    TEST_ASSERT_FALSE(hal.ce_level);
}

void test_tx_floating_spi_after_launch_does_not_rearm_or_report_success(void)
{
    for (const bool irq_connected : {false, true}) {
        FaultHal hal;
        hal.irq_connected = irq_connected;
        Nrf24 radio(hal);
        TEST_ASSERT_TRUE(radio.initDefaults());
        hal.floating = true;
        hal.after_launch = true;
        const uint8_t payload[] = {0x12};

        TEST_ASSERT_FALSE(radio.transmitOnce(payload, sizeof(payload)));
        TEST_ASSERT_TRUE(radio.lastTxCommunicationFailed());
        TEST_ASSERT_FALSE(radio.lastTxTimedOut());
        TEST_ASSERT_EQUAL_HEX8(0xFF, radio.lastTxStatus());
        TEST_ASSERT_EQUAL_INT(1, hal.tx_trigger_count);
        TEST_ASSERT_FALSE(hal.ce_level);
    }
}

void test_tx_invalid_final_snapshot_is_communication_fault_not_timeout(void)
{
    FaultHal hal;
    Nrf24 radio(hal);
    TEST_ASSERT_TRUE(radio.initDefaults());
    hal.floating = true;
    hal.after_launch = true;
    const uint8_t payload[] = {0x12};

    TEST_ASSERT_FALSE(radio.transmitOnce(payload, sizeof(payload), 0));
    TEST_ASSERT_TRUE(radio.lastTxCommunicationFailed());
    TEST_ASSERT_FALSE(radio.lastTxTimedOut());
    TEST_ASSERT_EQUAL_HEX8(0xFF, radio.lastTxStatus());
    TEST_ASSERT_EQUAL_INT(1, hal.tx_trigger_count);
    TEST_ASSERT_FALSE(hal.ce_level);
}

void test_tx_reserved_status_cannot_be_interpreted_as_success(void)
{
    const uint8_t invalid_statuses[] = {0xAE, 0x2C};
    for (const uint8_t invalid_status : invalid_statuses) {
        FaultHal hal;
        Nrf24 radio(hal);
        TEST_ASSERT_TRUE(radio.initDefaults());
        hal.override_status = true;
        hal.status_value = invalid_status;
        hal.after_launch = true;
        const uint8_t payload[] = {0x12};

        TEST_ASSERT_FALSE(radio.transmitOnce(payload, sizeof(payload)));
        TEST_ASSERT_TRUE(radio.lastTxCommunicationFailed());
        TEST_ASSERT_EQUAL_HEX8(invalid_status, radio.lastTxStatus());
        TEST_ASSERT_EQUAL_INT(1, hal.tx_trigger_count);
    }
}

void test_tx_invalid_fifo_rejects_irq_success_and_fifo_fallback(void)
{
    for (const uint8_t status : {uint8_t{0x2E}, uint8_t{0x0E}}) {
        FaultHal hal;
        Nrf24 radio(hal);
        TEST_ASSERT_TRUE(radio.initDefaults());
        hal.override_status = true;
        hal.status_value = status;
        hal.override_fifo = true;
        hal.after_launch = true;
        const uint8_t payload[] = {0x12};

        TEST_ASSERT_FALSE(radio.transmitOnce(payload, sizeof(payload)));
        TEST_ASSERT_TRUE(radio.lastTxCommunicationFailed());
        TEST_ASSERT_EQUAL_HEX8(0xFF, radio.lastTxFifoStatus());
        TEST_ASSERT_EQUAL_INT(1, hal.tx_trigger_count);
    }
}

void test_tx_valid_zero_status_preserves_no_ack_fifo_fallback(void)
{
    FaultHal hal;
    Nrf24 radio(hal);
    TEST_ASSERT_TRUE(radio.initDefaults());
    hal.override_status = true;
    hal.status_value = 0;
    const uint8_t payload[] = {0x12};

    TEST_ASSERT_TRUE(radio.transmitOnce(payload, sizeof(payload)));
    TEST_ASSERT_FALSE(radio.lastTxCommunicationFailed());
    TEST_ASSERT_EQUAL_HEX8(0, radio.lastTxStatus());
    TEST_ASSERT_EQUAL_INT(1, hal.tx_trigger_count);
}

void test_tx_valid_irq_success_clears_previous_communication_failure(void)
{
    FaultHal hal;
    Nrf24 radio(hal);
    RadioManager manager(radio);
    TEST_ASSERT_TRUE(manager.boot());
    const uint8_t payload[] = {0x12};
    hal.floating = true;
    TEST_ASSERT_FALSE(manager.sendPayload(payload, sizeof(payload)));

    hal.floating = false;
    TEST_ASSERT_TRUE(manager.sendPayload(payload, sizeof(payload)));
    TEST_ASSERT_FALSE(radio.lastTxCommunicationFailed());
    TEST_ASSERT_FALSE(radio.lastTxTimedOut());
    TEST_ASSERT_TRUE(manager.status().last_tx_ok);
    TEST_ASSERT_EQUAL_INT(0, manager.status().last_fault);
}

void test_tx_valid_max_rt_remains_transmit_failure(void)
{
    FakeHal hal;
    Nrf24 radio(hal);
    RadioManager manager(radio);
    TEST_ASSERT_TRUE(manager.boot());
    hal.next_tx_success = false;
    const uint8_t payload[] = {0x12};

    TEST_ASSERT_FALSE(manager.sendPayload(payload, sizeof(payload)));
    TEST_ASSERT_FALSE(radio.lastTxCommunicationFailed());
    TEST_ASSERT_FALSE(radio.lastTxTimedOut());
    TEST_ASSERT_EQUAL_INT(3, manager.status().last_fault);
    TEST_ASSERT_EQUAL_INT(2, hal.tx_trigger_count);
}

void test_tx_valid_no_ack_fifo_completion_remains_successful(void)
{
    SilentSuccessHal hal;
    Nrf24 radio(hal);
    TEST_ASSERT_TRUE(radio.initDefaults());
    const uint8_t payload[] = {0x12};

    TEST_ASSERT_TRUE(radio.transmitOnce(payload, sizeof(payload)));
    TEST_ASSERT_FALSE(radio.lastTxCommunicationFailed());
    TEST_ASSERT_FALSE(radio.lastTxTimedOut());
    TEST_ASSERT_EQUAL_INT(1, hal.tx_trigger_count);
}

void test_cw_floating_spi_cannot_report_carrier_started(void)
{
    FaultHal hal;
    Nrf24 radio(hal);
    TEST_ASSERT_TRUE(radio.initDefaults());
    hal.floating = true;

    TEST_ASSERT_FALSE(radio.startContinuousCarrier());
    TEST_ASSERT_FALSE(hal.ce_level);
}

void test_cw_payload_reuse_rejects_invalid_tx_success_status(void)
{
    FaultHal hal;
    Nrf24 radio(hal);
    TEST_ASSERT_TRUE(radio.initDefaults());
    hal.supports_cont_wave = false;
    hal.floating = true;
    hal.after_launch = true;

    TEST_ASSERT_FALSE(radio.startContinuousCarrier());
    TEST_ASSERT_FALSE(hal.ce_level);
    TEST_ASSERT_FALSE(hal.tx_reuse);
}

}  // namespace

void runRadioFaultTests()
{
    RUN_TEST(test_tx_floating_spi_before_launch_reports_communication_fault);
    RUN_TEST(test_tx_floating_spi_after_launch_does_not_rearm_or_report_success);
    RUN_TEST(test_tx_invalid_final_snapshot_is_communication_fault_not_timeout);
    RUN_TEST(test_tx_reserved_status_cannot_be_interpreted_as_success);
    RUN_TEST(test_tx_invalid_fifo_rejects_irq_success_and_fifo_fallback);
    RUN_TEST(test_tx_valid_zero_status_preserves_no_ack_fifo_fallback);
    RUN_TEST(test_tx_valid_irq_success_clears_previous_communication_failure);
    RUN_TEST(test_tx_valid_max_rt_remains_transmit_failure);
    RUN_TEST(test_tx_valid_no_ack_fifo_completion_remains_successful);
    RUN_TEST(test_cw_floating_spi_cannot_report_carrier_started);
    RUN_TEST(test_cw_payload_reuse_rejects_invalid_tx_success_status);
}
