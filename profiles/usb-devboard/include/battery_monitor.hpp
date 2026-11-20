#pragma once

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <limits>

namespace BatteryMonitor {
struct Config {
    int cells = 6;
    int capacity_mah = 3300;
    int cutoff_mv = 20400;
    int reset_mv = 21000;
    int estimate_offset_mv = 100;
    uint32_t sample_period_ms = 250;
    uint32_t window_ms = 4000;
    uint32_t startup_ms = 10000;
    uint32_t active_check_ms = 300000;
    uint32_t active_retry_ms = 2000;
    uint32_t quiet_ms = 2000;
    uint32_t fresh_ms = 10000;
    unsigned low_samples = 5;
};

struct Snapshot {
    int pack_mv = -1;
    int estimated_mv = -1;
    int percent = -1;
    int remaining_mah = -1;
    int age_ms = -1;
    const char* state = "unavailable";
    bool cutoff = false;
    bool persisted = false;
};

struct SensePoint { int sense_uv; int pack_mv; };

// Bench supply against unconnected sensing output, supplied 2026-11-05.
// Units are microvolts at the sense pin and millivolts at the bench supply.
// The 24.25 V row has no sense reading and is omitted.
inline constexpr SensePoint kSenseCalibration[] = {
    {1664700, 15250}, {1691900, 15500}, {1719000, 15750}, {1740000, 16000},
    {1773600, 16250}, {1800000, 16500}, {1828200, 16750}, {1855330, 17000},
    {1882240, 17250}, {1909500, 17500}, {1937000, 17750}, {1964000, 18000},
    {1991000, 18250}, {2018000, 18500}, {2045700, 18750}, {2073000, 19000},
    {2104000, 19250}, {2127600, 19500}, {2154000, 19750}, {2182200, 20000},
    {2210000, 20250}, {2236000, 20500}, {2263000, 20750}, {2291100, 21000},
    {2318000, 21250}, {2345000, 21500}, {2372800, 21750}, {2399999, 22000},
    {2427000, 22250}, {2454000, 22500}, {2481600, 22750}, {2508000, 23000},
    {2536000, 23250}, {2563400, 23500}, {2590000, 23750}, {2618000, 24000},
    {2672000, 24500}, {2699000, 24750}, {2727000, 25000}, {2754000, 25250},
    {2781300, 25500},
};

inline int64_t packUvForSenseUv(int sense_uv)
{
    const size_t count = sizeof(kSenseCalibration) / sizeof(kSenseCalibration[0]);
    size_t upper = 1;
    while (upper < count - 1 && sense_uv > kSenseCalibration[upper].sense_uv) ++upper;
    const auto& a = kSenseCalibration[upper - 1];
    const auto& b = kSenseCalibration[upper];
    const int64_t denominator = b.sense_uv - a.sense_uv;
    const int64_t numerator = (static_cast<int64_t>(sense_uv) - a.sense_uv) *
        (b.pack_mv - a.pack_mv) * 1000;
    const int64_t offset = (numerator + (numerator >= 0 ? denominator / 2 : -denominator / 2)) /
        denominator;
    return std::max<int64_t>(0, static_cast<int64_t>(a.pack_mv) * 1000 + offset);
}

inline int percentForCellMv(int mv)
{
    struct Point { int mv; int percent; };
    // Generic conventional 4.20-V/cell LiPo reference, linearly interpolated
    // BETWEEN these nonlinear knots; replace with characterized Soikon data.
    constexpr Point curve[] = {{3300,0},{3600,5},{3690,10},{3730,20},
        {3770,30},{3800,40},{3840,50},{3870,60},{3950,70},{4020,80},
        {4110,90},{4200,100}};
    if (mv <= curve[0].mv) return 0;
    for (size_t i = 1; i < sizeof(curve)/sizeof(curve[0]); ++i) {
        if (mv <= curve[i].mv) {
            const auto& a = curve[i-1]; const auto& b = curve[i];
            return a.percent + ((mv-a.mv)*(b.percent-a.percent)+(b.mv-a.mv)/2)/(b.mv-a.mv);
        }
    }
    return 100;
}

class Monitor {
public:
    explicit Monitor(Config config = {}) : config_(config) {}

    bool due(uint64_t now, bool quiet, uint64_t radio_activity_ms = 0,
             bool active_check = false, unsigned radio_context = 0)
    {
        if (!quiet) last_busy_ms_ = now;
        const uint64_t boundary = std::max(last_busy_ms_, radio_activity_ms);
        held_ = !quiet || now < boundary || now - boundary < config_.quiet_ms;
        if (now < config_.startup_ms || held_) {
            clearWindow();
            return false;
        }
        // A scheduled check is a distinct sampling window even if CE does
        // not toggle when a file sender goes idle between packets.
        if (batches_ && (active_check != window_active_ || radio_context != window_context_ ||
                        now < last_attempt_ms_ ||
                        now - last_attempt_ms_ > 2u * config_.sample_period_ms ||
                        boundary > window_started_ms_)) {
            clearWindow();
        }
        window_active_ = active_check;
        window_context_ = radio_context;
        if (attempted_ && now >= last_attempt_ms_ &&
            now - last_attempt_ms_ < config_.sample_period_ms) return false;
        attempted_ = true;
        last_attempt_ms_ = now;
        return true;
    }

    bool observe(uint64_t now, int sense_uv, int raw, bool calibrated, bool valid)
    {
        if (!valid) {
            error_ = true;
            clearWindow();
            low_count_ = 0;
            return false;
        }
        if (!batches_) window_started_ms_ = now;
        ++batches_;
        sum_sense_uv_ += sense_uv;
        sum_raw_ += raw;
        all_calibrated_ = all_calibrated_ && calibrated;
        if (now - window_started_ms_ < config_.window_ms) return false;

        const int mean_uv = static_cast<int>((sum_sense_uv_ + batches_ / 2) / batches_);
        const int64_t pack_uv = packUvForSenseUv(mean_uv);
        if (pack_uv > 35000000) {
            error_ = true;
            low_count_ = 0;
            clearWindow();
            return false;
        }
        error_ = false;
        sense_uv_ = mean_uv;
        raw_ = static_cast<int>((sum_raw_ + batches_ / 2) / batches_);
        calibrated_ = all_calibrated_;
        window_samples_ = batches_;
        clearWindow();
        pack_uv_ = pack_uv;
        last_sample_ms_ = now;
        ++reading_serial_;

        if (cutoff_) return false;
        if (pack_uv_ < static_cast<int64_t>(config_.cutoff_mv) * 1000) ++low_count_;
        else low_count_ = 0;
        return low_count_ >= config_.low_samples;
    }

    bool activeCheckDue(uint64_t now) const
    {
        if (cutoff_ || now < config_.startup_ms) return false;
        const uint64_t boundary = std::max({last_active_check_ms_, last_sample_ms_,
                                           static_cast<uint64_t>(config_.startup_ms)});
        const uint32_t interval = low_count_ ? config_.active_retry_ms : config_.active_check_ms;
        return now >= boundary && now - boundary >= interval;
    }

    void endActiveCheck(uint64_t now)
    {
        last_active_check_ms_ = now;
        clearWindow();
    }

    uint32_t readingSerial() const { return reading_serial_; }
    unsigned windowSamples() const { return window_samples_; }
    int senseMv() const { return sense_uv_ < 0 ? -1 : (sense_uv_ + 500) / 1000; }
    int raw() const { return raw_; }
    bool calibrated() const { return calibrated_; }

    void latch(bool persisted)
    {
        cutoff_ = true;
        persisted_ = persisted;
        low_count_ = 0;
    }
    void setPersisted(bool value) { persisted_ = value; }

    bool canReset(uint64_t now) const
    {
        return cutoff_ && !held_ && !error_ && fresh(now) &&
            pack_uv_ >= static_cast<int64_t>(config_.reset_mv) * 1000;
    }
    void reset() { cutoff_ = false; persisted_ = false; low_count_ = 0; }
    bool cutoff() const { return cutoff_; }
    bool persisted() const { return persisted_; }

    bool allowRf(uint64_t now) const
    {
        return !cutoff_ && !error_ && low_count_ == 0 && fresh(now) &&
            pack_uv_ >= static_cast<int64_t>(config_.cutoff_mv) * 1000;
    }

    Snapshot snapshot(uint64_t now) const
    {
        Snapshot s;
        s.cutoff = cutoff_;
        s.persisted = persisted_;
        if (pack_uv_ >= 0) {
            s.pack_mv = static_cast<int>((pack_uv_ + 500) / 1000);
            s.estimated_mv = std::max(0, s.pack_mv - config_.estimate_offset_mv);
            const uint64_t age = now >= last_sample_ms_ ? now - last_sample_ms_ : 0;
            s.age_ms = static_cast<int>(std::min<uint64_t>(age, std::numeric_limits<int>::max()));
            s.percent = cutoff_ ? 0 : percentForCellMv((s.estimated_mv + config_.cells / 2) / config_.cells);
            s.remaining_mah = (s.percent * config_.capacity_mah + 50) / 100;
        } else if (cutoff_) {
            s.percent = s.remaining_mah = 0;
        }
        if (cutoff_) s.state = "cutoff";
        else if (error_) s.state = "sensor_error";
        else if (now < config_.startup_ms) s.state = "startup";
        else if (low_count_) s.state = "low_pending";
        else if (pack_uv_ < 0) s.state = batches_ ? "averaging" : held_ ? "settling" : "unavailable";
        else if (!fresh(now)) s.state = "stale";
        else s.state = held_ ? "held" : "fresh";
        return s;
    }

private:
    bool fresh(uint64_t now) const
    {
        return pack_uv_ >= 0 && now >= last_sample_ms_ &&
            now - last_sample_ms_ <= config_.fresh_ms;
    }

    void clearWindow()
    {
        batches_ = 0;
        sum_sense_uv_ = sum_raw_ = 0;
        all_calibrated_ = true;
    }

    Config config_;
    uint64_t last_sample_ms_ = 0, last_attempt_ms_ = 0, last_busy_ms_ = 0;
    uint64_t window_started_ms_ = 0, last_active_check_ms_ = 0;
    int64_t sum_sense_uv_ = 0, sum_raw_ = 0, pack_uv_ = -1;
    int sense_uv_ = -1, raw_ = -1;
    unsigned batches_ = 0, window_samples_ = 0, low_count_ = 0, window_context_ = 0;
    uint32_t reading_serial_ = 0;
    bool attempted_ = false, held_ = true, error_ = false;
    bool window_active_ = false, all_calibrated_ = true, calibrated_ = false;
    bool cutoff_ = false, persisted_ = false;
};
} // namespace BatteryMonitor
