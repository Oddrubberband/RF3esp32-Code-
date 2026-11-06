#include <unity.h>
#include "battery_monitor.hpp"

namespace {
bool batch(BatteryMonitor::Monitor& monitor, uint64_t time, int sense_uv,
           bool scheduled = false, int raw = 2000, bool calibrated = true)
{
    TEST_ASSERT_TRUE(monitor.due(time, true, 0, scheduled));
    return monitor.observe(time, sense_uv, raw, calibrated, true);
}

bool window(BatteryMonitor::Monitor& monitor, uint64_t start, int sense_uv,
            bool scheduled = false)
{
    for (uint64_t offset = 0; offset < 4000; offset += 250)
        TEST_ASSERT_FALSE(batch(monitor, start + offset, sense_uv, scheduled));
    return batch(monitor, start + 4000, sense_uv, scheduled);
}

void exact_knots_skip_missing_row_and_extrapolate()
{
    const auto& points = BatteryMonitor::kSenseCalibration;
    TEST_ASSERT_EQUAL(41, sizeof(points) / sizeof(points[0]));
    for (const auto& point : points) {
        TEST_ASSERT_NOT_EQUAL(24250, point.pack_mv);
        TEST_ASSERT_EQUAL_INT64(point.pack_mv * 1000, BatteryMonitor::packUvForSenseUv(point.sense_uv));
    }
    TEST_ASSERT_EQUAL_INT64(22000000, BatteryMonitor::packUvForSenseUv(2399999));
    TEST_ASSERT_EQUAL_INT64(24250000, BatteryMonitor::packUvForSenseUv(2645000));
    TEST_ASSERT_EQUAL_INT64(15000000, BatteryMonitor::packUvForSenseUv(1637500));
    TEST_ASSERT_EQUAL_INT64(25750000, BatteryMonitor::packUvForSenseUv(2808600));
    TEST_ASSERT_EQUAL_INT64(0, BatteryMonitor::packUvForSenseUv(0));
}

void startup_complete_average_and_estimate()
{
    BatteryMonitor::Monitor monitor;
    TEST_ASSERT_FALSE(monitor.due(9999, true));
    TEST_ASSERT_EQUAL_STRING("startup", monitor.snapshot(9999).state);
    for (uint64_t t = 10000; t < 14000; t += 250) {
        const int sign = t % 500 == 0 ? -1 : 1;
        TEST_ASSERT_FALSE(batch(monitor, t, 2512480 + sign * 1000, false,
                                2000 + sign * 8, t != 10750));
        TEST_ASSERT_EQUAL(0, monitor.readingSerial());
    }
    TEST_ASSERT_EQUAL_STRING("averaging", monitor.snapshot(13999).state);
    TEST_ASSERT_FALSE(batch(monitor, 14000, 2512480));
    TEST_ASSERT_EQUAL(1, monitor.readingSerial());
    TEST_ASSERT_EQUAL(17, monitor.windowSamples());
    TEST_ASSERT_EQUAL(2512, monitor.senseMv());
    TEST_ASSERT_EQUAL(2000, monitor.raw());
    TEST_ASSERT_FALSE(monitor.calibrated());
    const auto gauge = monitor.snapshot(14000);
    TEST_ASSERT_EQUAL(23040, gauge.pack_mv);
    TEST_ASSERT_EQUAL(22940, gauge.estimated_mv);
    TEST_ASSERT_EQUAL(46, gauge.percent);
    TEST_ASSERT_EQUAL(1518, gauge.remaining_mah);
    TEST_ASSERT_TRUE(monitor.allowRf(14000));
    TEST_ASSERT_FALSE(batch(monitor, 14250, 2000000));
    TEST_ASSERT_EQUAL_STRING("fresh", monitor.snapshot(14250).state);
    TEST_ASSERT_TRUE(monitor.allowRf(14250));
}

void five_consecutive_means_and_unrounded_protection_boundary()
{
    BatteryMonitor::Monitor monitor;
    TEST_ASSERT_EQUAL_INT64(20400000, BatteryMonitor::packUvForSenseUv(2225600));
    TEST_ASSERT_FALSE(window(monitor, 10000, 2225600));
    TEST_ASSERT_TRUE(monitor.allowRf(14000));
    for (unsigned i = 0; i < 5; ++i) {
        const uint64_t start = 14250 + 4250 * i;
        TEST_ASSERT_EQUAL(i == 4, window(monitor, start, 2225599));
        TEST_ASSERT_FALSE(monitor.allowRf(start + 4000));
        TEST_ASSERT_EQUAL(20400, monitor.snapshot(start + 4000).pack_mv);
    }
    monitor.latch(true);
    const auto trip = monitor.snapshot(35250);
    TEST_ASSERT_EQUAL_STRING("cutoff", trip.state);
    TEST_ASSERT_EQUAL(20400, trip.pack_mv);
    TEST_ASSERT_EQUAL(20300, trip.estimated_mv);
    TEST_ASSERT_EQUAL(0, trip.percent);
    TEST_ASSERT_EQUAL(0, trip.remaining_mah);
}

void rf_activity_mode_change_and_sample_gap_discard_partial_mean()
{
    BatteryMonitor::Monitor monitor;
    TEST_ASSERT_FALSE(batch(monitor, 10000, 1600000));
    TEST_ASSERT_FALSE(monitor.due(10100, false));
    TEST_ASSERT_FALSE(monitor.due(11999, true));
    TEST_ASSERT_TRUE(monitor.due(12100, true));
    TEST_ASSERT_FALSE(monitor.observe(12100, 2600000, 2000, true, true));
    TEST_ASSERT_FALSE(batch(monitor, 12500, 1900000, true)); // Different RF context.
    TEST_ASSERT_FALSE(window(monitor, 13250, 2399999, true)); // More than 500 ms missed.
    TEST_ASSERT_EQUAL(1, monitor.readingSerial());
    TEST_ASSERT_EQUAL(22000, monitor.snapshot(17250).pack_mv);
    TEST_ASSERT_FALSE(monitor.due(18000, true, 17800));
    TEST_ASSERT_EQUAL_STRING("held", monitor.snapshot(18000).state);
    TEST_ASSERT_EQUAL_STRING("stale", monitor.snapshot(27251).state);
    TEST_ASSERT_FALSE(monitor.allowRf(27251));
}

void quiet_radio_mode_change_starts_new_mean()
{
    BatteryMonitor::Monitor monitor;
    TEST_ASSERT_TRUE(monitor.due(10000, true, 0, false, 1));
    TEST_ASSERT_FALSE(monitor.observe(10000, 1600000, 2000, true, true));
    for (uint64_t t = 10250; t <= 14250; t += 250) {
        TEST_ASSERT_TRUE(monitor.due(t, true, 0, false, 2));
        TEST_ASSERT_FALSE(monitor.observe(t, 2399999, 2000, true, true));
        TEST_ASSERT_EQUAL(t == 14250 ? 1 : 0, monitor.readingSerial());
    }
    TEST_ASSERT_EQUAL(22000, monitor.snapshot(14250).pack_mv);
}

void invalid_batch_breaks_confirmation_and_recovery_needs_valid_mean()
{
    BatteryMonitor::Monitor monitor;
    for (unsigned i = 0; i < 4; ++i)
        TEST_ASSERT_FALSE(window(monitor, 10000 + i * 4250, 2200000));
    TEST_ASSERT_TRUE(monitor.due(27000, true));
    TEST_ASSERT_FALSE(monitor.observe(27000, -1, -1, false, false));
    TEST_ASSERT_EQUAL_STRING("sensor_error", monitor.snapshot(27000).state);
    TEST_ASSERT_FALSE(window(monitor, 27250, 2200000));
    monitor.latch(true);
    TEST_ASSERT_FALSE(window(monitor, 31500, 2291099));
    TEST_ASSERT_FALSE(monitor.canReset(35500));
    TEST_ASSERT_FALSE(window(monitor, 35750, 2291100));
    TEST_ASSERT_TRUE(monitor.canReset(39750));
    TEST_ASSERT_FALSE(monitor.canReset(49751));
    TEST_ASSERT_FALSE(monitor.due(39800, false));
    TEST_ASSERT_FALSE(monitor.canReset(39800));
}

void scheduled_checks_and_low_confirmation_allow_radio_progress()
{
    BatteryMonitor::Monitor monitor;
    TEST_ASSERT_FALSE(window(monitor, 10000, 2400000));
    TEST_ASSERT_FALSE(monitor.activeCheckDue(313999));
    TEST_ASSERT_TRUE(monitor.activeCheckDue(314000));
    TEST_ASSERT_FALSE(window(monitor, 316000, 2200000, true));
    monitor.endActiveCheck(320000);
    TEST_ASSERT_FALSE(monitor.activeCheckDue(321999));
    TEST_ASSERT_TRUE(monitor.activeCheckDue(322000));
    TEST_ASSERT_FALSE(monitor.due(322001, false));
    TEST_ASSERT_FALSE(window(monitor, 324001, 2200000, true));
    TEST_ASSERT_EQUAL_STRING("low_pending", monitor.snapshot(328001).state);
    TEST_ASSERT_FALSE(window(monitor, 330001, 2400000, true));
    TEST_ASSERT_FALSE(monitor.activeCheckDue(334002));
}
}

void runBatteryMonitorTests()
{
    RUN_TEST(exact_knots_skip_missing_row_and_extrapolate);
    RUN_TEST(startup_complete_average_and_estimate);
    RUN_TEST(five_consecutive_means_and_unrounded_protection_boundary);
    RUN_TEST(rf_activity_mode_change_and_sample_gap_discard_partial_mean);
    RUN_TEST(quiet_radio_mode_change_starts_new_mean);
    RUN_TEST(invalid_batch_breaks_confirmation_and_recovery_needs_valid_mean);
    RUN_TEST(scheduled_checks_and_low_confirmation_allow_radio_progress);
}
