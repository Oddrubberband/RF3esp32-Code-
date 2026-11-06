#include <unity.h>
#include <utility>
#include <vector>
#include "devboard_power.hpp"

namespace {
struct FakePowerIo : DevboardPower::Io {
    std::vector<std::pair<int, bool>> outputs;
    int adc_gpio = -1;
    int reads = 0;
    bool output_ok = true;
    bool adc_ok = true;
    bool persisted=false, load_ok=true, store_ok=true;
    unsigned stores=0;
    DevboardPower::Reading reading{2048, 2400, true, false};
    bool loadCutoff(bool& value) override {value=persisted;return load_ok;}
    bool storeCutoff(bool value) override {++stores;if(!store_ok)return false;persisted=value;return true;}
    bool output(int gpio, bool high) override {
        outputs.emplace_back(gpio, high);
        return output_ok;
    }
    bool beginAdc(int gpio) override { adc_gpio = gpio; return adc_ok; }
    DevboardPower::Reading readAdc() override { ++reads; return reading; }
};

bool powerWindow(DevboardPower::Controller& power, uint64_t start)
{
    for (uint64_t offset = 0; offset < 4000; offset += 250)
        TEST_ASSERT_FALSE(power.poll(start + offset, true));
    return power.poll(start + 4000, true);
}

void boot_asserts_battery_before_radio_and_configures_input() {
    FakePowerIo io;
    DevboardPower::Controller power(io, true);
    TEST_ASSERT_TRUE(power.initialize());
    TEST_ASSERT_EQUAL(2, io.outputs.size());
    TEST_ASSERT_EQUAL(15, io.outputs[0].first);
    TEST_ASSERT_TRUE(io.outputs[0].second);
    TEST_ASSERT_EQUAL(21, io.outputs[1].first);
    TEST_ASSERT_TRUE(io.outputs[1].second);
    TEST_ASSERT_EQUAL(34, io.adc_gpio);
    TEST_ASSERT_TRUE(power.powerGood());
}

void adc_remains_live_when_radio_rail_is_off() {
    FakePowerIo io;
    DevboardPower::Controller power(io, true);
    TEST_ASSERT_TRUE(power.initialize());
    TEST_ASSERT_TRUE(power.setNrfRail(false));
    TEST_ASSERT_FALSE(power.nrfRail());
    TEST_ASSERT_FALSE(power.poll(9999, true));
    TEST_ASSERT_EQUAL(0, io.reads);
    TEST_ASSERT_FALSE(powerWindow(power, 10000));
    TEST_ASSERT_EQUAL(2400, power.sample().millivolts);
    io.reading.millivolts = 800;
    TEST_ASSERT_FALSE(power.poll(14250, true));
    TEST_ASSERT_EQUAL(2400, power.sample().millivolts);
    for (uint64_t t = 14500; t <= 18250; t += 250)
        TEST_ASSERT_FALSE(power.poll(t, true));
    TEST_ASSERT_EQUAL(800, power.sample().millivolts);
    TEST_ASSERT_EQUAL(34, io.reads);
    TEST_ASSERT_TRUE(power.powerGood());
    TEST_ASSERT_EQUAL(21, io.outputs.back().first);
}

void custom_profile_does_not_touch_reserved_pins() {
    FakePowerIo io;
    DevboardPower::Controller power(io, false);
    TEST_ASSERT_TRUE(power.initialize());
    TEST_ASSERT_EQUAL(-1, power.sample().millivolts);
    TEST_ASSERT_FALSE(power.setNrfRail(false));
    TEST_ASSERT_TRUE(io.outputs.empty());
    TEST_ASSERT_EQUAL(-1, io.adc_gpio);
    TEST_ASSERT_EQUAL(0, io.reads);
    TEST_ASSERT_TRUE(power.nrfRail());
}

void adc_failure_does_not_cut_battery_power_or_report_zero() {
    FakePowerIo io;
    io.adc_ok = false;
    DevboardPower::Controller power(io, true);
    TEST_ASSERT_TRUE(power.initialize());
    TEST_ASSERT_TRUE(power.powerGood());
    TEST_ASSERT_EQUAL(-1, power.sample().millivolts);
    TEST_ASSERT_EQUAL(0, io.reads);
}

void failed_output_does_not_claim_rail_change() {
    FakePowerIo io;
    DevboardPower::Controller power(io, true);
    TEST_ASSERT_TRUE(power.initialize());
    io.output_ok = false;
    TEST_ASSERT_FALSE(power.setNrfRail(false));
    TEST_ASSERT_TRUE(power.nrfRail());
    TEST_ASSERT_TRUE(power.powerGood());
}

void failed_boot_output_stops_before_adc_or_radio_enable() {
    FakePowerIo io;
    io.output_ok = false;
    DevboardPower::Controller power(io, true);
    TEST_ASSERT_FALSE(power.initialize());
    TEST_ASSERT_EQUAL(1, io.outputs.size());
    TEST_ASSERT_FALSE(power.powerGood());
    TEST_ASSERT_EQUAL(-1, io.adc_gpio);
}
void status_reads_cache_and_never_samples_during_rf()
{
    FakePowerIo io;
    DevboardPower::Controller power(io, true);
    TEST_ASSERT_TRUE(power.initialize());
    for (int i = 0; i < 100; ++i) TEST_ASSERT_EQUAL(-1, power.sample().millivolts);
    TEST_ASSERT_EQUAL(0, io.reads);
    TEST_ASSERT_FALSE(powerWindow(power, 10000));
    const unsigned reads = io.reads;
    for (uint64_t t = 14250; t <= 20000; t += 250) TEST_ASSERT_FALSE(power.poll(t, false));
    TEST_ASSERT_EQUAL(reads, io.reads);
    TEST_ASSERT_EQUAL(2400, power.sample().millivolts);
    TEST_ASSERT_FALSE(power.poll(21999, true));
    TEST_ASSERT_EQUAL(reads, io.reads);
    TEST_ASSERT_FALSE(power.poll(22000, true));
    TEST_ASSERT_EQUAL(reads + 1, io.reads);
}

void sustained_low_latches_both_outputs_and_survives_reboot()
{
    FakePowerIo io;
    DevboardPower::Controller power(io, true);
    TEST_ASSERT_TRUE(power.initialize());
    io.reading.millivolts = 2200;
    for (unsigned i = 0; i < 5; ++i)
        TEST_ASSERT_EQUAL(i == 4, powerWindow(power, 10000 + 4250 * i));
    TEST_ASSERT_TRUE(power.requestCutoff());
    TEST_ASSERT_FALSE(power.requestCutoff());
    TEST_ASSERT_TRUE(power.powerGood());
    TEST_ASSERT_TRUE(power.nrfRail());
    TEST_ASSERT_FALSE(power.setNrfRail(true));
    TEST_ASSERT_FALSE(io.persisted);
    TEST_ASSERT_TRUE(power.applyCutoff(31000));
    TEST_ASSERT_FALSE(power.powerGood());
    TEST_ASSERT_FALSE(power.nrfRail());
    TEST_ASSERT_TRUE(io.persisted);
    TEST_ASSERT_EQUAL(0, power.battery(31000).percent);
    TEST_ASSERT_FALSE(power.setNrfRail(true));
    io.outputs.clear();
    DevboardPower::Controller reboot(io, true);
    TEST_ASSERT_TRUE(reboot.initialize());
    for (const auto& output : io.outputs) TEST_ASSERT_FALSE(output.second);
    TEST_ASSERT_TRUE(reboot.battery(0).cutoff);
    TEST_ASSERT_EQUAL(0, reboot.battery(0).percent);
}

void rebound_does_not_reset_and_manual_reset_requires_fresh_recovery()
{
    FakePowerIo io;
    DevboardPower::Controller power(io, true);
    TEST_ASSERT_TRUE(power.initialize());
    io.reading.millivolts = 2200;
    for (unsigned i = 0; i < 5; ++i) (void)powerWindow(power, 10000 + 4250 * i);
    TEST_ASSERT_TRUE(power.applyCutoff(31000));
    TEST_ASSERT_FALSE(power.resetCutoff(31000));
    io.reading.millivolts = 2600;
    TEST_ASSERT_FALSE(powerWindow(power, 31250));
    TEST_ASSERT_FALSE(power.powerGood());
    TEST_ASSERT_TRUE(power.battery(35250).cutoff);
    TEST_ASSERT_FALSE(power.poll(35500, false));
    TEST_ASSERT_FALSE(power.resetCutoff(35500));
    TEST_ASSERT_FALSE(power.poll(37500, true));
    TEST_ASSERT_TRUE(power.resetCutoff(37500));
    TEST_ASSERT_TRUE(power.powerGood());
    TEST_ASSERT_FALSE(power.nrfRail());
    TEST_ASSERT_FALSE(io.persisted);
}

void noise_saturation_and_failed_persistence_do_not_invent_valid_readings()
{
    FakePowerIo io;
    DevboardPower::Controller power(io, true);
    TEST_ASSERT_TRUE(power.initialize());
    io.reading.millivolts = 2200;
    io.reading.noisy = true;
    for (uint64_t t = 10000; t <= 11750; t += 250) TEST_ASSERT_FALSE(power.poll(t, true));
    TEST_ASSERT_EQUAL(-1, power.battery(11750).pack_mv);
    TEST_ASSERT_FALSE(power.allowRf(11750));
    TEST_ASSERT_TRUE(power.powerGood());
    TEST_ASSERT_TRUE(power.sample().noisy);
    io.reading.noisy = false;
    io.reading.saturated = true;
    TEST_ASSERT_FALSE(power.poll(12000, true));
    TEST_ASSERT_EQUAL(-1, power.battery(12000).pack_mv);
    TEST_ASSERT_TRUE(power.sample().saturated);
    io.reading.saturated = false;
    for (unsigned i = 0; i < 5; ++i) (void)powerWindow(power, 12250 + 4250 * i);
    io.store_ok = false;
    TEST_ASSERT_TRUE(power.applyCutoff(33250));
    TEST_ASSERT_FALSE(power.powerGood());
    TEST_ASSERT_FALSE(power.battery(33250).persisted);
    const unsigned attempts = io.stores;
    TEST_ASSERT_TRUE(power.applyCutoff(33350));
    TEST_ASSERT_EQUAL(attempts, io.stores);
    io.store_ok = true;
    TEST_ASSERT_TRUE(power.applyCutoff(34250));
    TEST_ASSERT_EQUAL(attempts + 1, io.stores);
    TEST_ASSERT_TRUE(power.battery(34250).persisted);
}

void unreadable_persistent_latch_fails_closed() {
    FakePowerIo io;io.load_ok=false;DevboardPower::Controller power(io,true);
    TEST_ASSERT_FALSE(power.initialize());TEST_ASSERT_FALSE(power.powerGood());
    TEST_ASSERT_EQUAL(1,io.outputs.size());TEST_ASSERT_FALSE(io.outputs[0].second);
}
}

void runDevboardPowerTests() {
    RUN_TEST(boot_asserts_battery_before_radio_and_configures_input);
    RUN_TEST(adc_remains_live_when_radio_rail_is_off);
    RUN_TEST(custom_profile_does_not_touch_reserved_pins);
    RUN_TEST(adc_failure_does_not_cut_battery_power_or_report_zero);
    RUN_TEST(failed_output_does_not_claim_rail_change);
    RUN_TEST(failed_boot_output_stops_before_adc_or_radio_enable);
    RUN_TEST(status_reads_cache_and_never_samples_during_rf);
    RUN_TEST(sustained_low_latches_both_outputs_and_survives_reboot);
    RUN_TEST(rebound_does_not_reset_and_manual_reset_requires_fresh_recovery);
    RUN_TEST(noise_saturation_and_failed_persistence_do_not_invent_valid_readings);
    RUN_TEST(unreadable_persistent_latch_fails_closed);
}
