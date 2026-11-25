#pragma once
#include <array>
#include <mutex>
#include <string_view>
#include "battery_monitor.hpp"

// Custom 16 MB PCB. Only the sensing circuit drives the ADC pin.
namespace DevboardPower {
constexpr int kSenseGpio = 34;
constexpr int kPowerGoodGpio = 15;

struct Reading {
    int raw = -1;
    int millivolts = -1;
    bool calibrated = false;
    bool saturated = false;
    bool noisy = false;
    int microvolts = -1;
};

struct Io {
    virtual ~Io() = default;
    virtual bool output(int gpio, bool high) = 0;
    virtual bool beginAdc(int gpio) = 0;
    virtual Reading readAdc() = 0;
};

struct VoltageReport {
    Reading sense;
    BatteryMonitor::Snapshot battery;
};

class Controller {
public:
    explicit Controller(Io& io) : io_(io) {}

    bool initialize()
    {
        enabled_ = io_.output(kPowerGoodGpio, true);
        if (!enabled_) return false;
        adc_ready_ = io_.beginAdc(kSenseGpio);
        if (!adc_ready_) state_ = "sensor_error";
        return true;
    }
    bool powerGood() const { return enabled_; }
    bool radioAvailable() const { return enabled_; }

    // Called with the radio mutex held. tx_epoch also detects a complete TX
    // burst between polls. The radio can run normally between short ADC batches.
    void poll(uint64_t now, bool transmitting, uint32_t tx_epoch)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!adc_ready_) return;
        if (transmitting || (active_ && tx_epoch != epoch_)) {
            if (active_) finishAttempt(now);
            state_ = "held";
            return;
        }
        if (!active_) {
            if (now < next_start_) return;
            active_ = true;
            epoch_ = tx_epoch;
            start_ = now;
            batches_ = 0;
            sum_uv_ = sum_raw_ = 0;
            calibrated_ = true;
            state_ = "averaging";
        }
        const uint64_t slot = (now - start_) / kBatchPeriodMs;
        if (slot < batches_) return;
        if (batches_ == kBatches) {
            publish();
            last_result_ = start_ + kWindowMs;
            finishAttempt(last_result_);
            state_ = "fresh";
            return;
        }
        if (slot != batches_) { // Delayed worker: never catch up with a burst.
            finishAttempt(now);
            state_ = "held";
            return;
        }
        const Reading value = io_.readAdc();
        if (value.raw < 0 || value.raw > 4095 || value.microvolts < 0 ||
            value.microvolts > 3300000 || value.saturated || value.noisy) {
            finishAttempt(now);
            state_ = "sensor_error";
            return;
        }
        sum_uv_ += value.microvolts;
        sum_raw_ += value.raw;
        calibrated_ = calibrated_ && value.calibrated;
        ++batches_;
    }

    VoltageReport report(uint64_t now) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        VoltageReport result = cached_;
        auto& battery = result.battery;
        battery.state = state_;
        if (battery.windows) {
            battery.age_ms = static_cast<int>(std::min<uint64_t>(now >= last_result_ ? now - last_result_ : 0, INT32_MAX));
            if (battery.age_ms > 2 * static_cast<int>(kWindowMs+kRestMs) &&
                state_ == std::string_view("fresh")) battery.state = "stale";
        }
        return result;
    }
private:
    static constexpr unsigned kBatches = 20;
    static constexpr uint64_t kWindowMs = 1000, kRestMs = 10000;
    static constexpr uint64_t kBatchPeriodMs = kWindowMs / kBatches;
    void finishAttempt(uint64_t end)
    {
        active_ = false;
        next_start_ = end + kRestMs;
    }
    void publish()
    {
        const int mean_uv = (sum_uv_ + kBatches/2) / kBatches;
        cached_.sense = {static_cast<int>((sum_raw_ + kBatches/2) / kBatches),
                         (mean_uv+500)/1000, calibrated_, false, false, mean_uv};
        auto& b = cached_.battery;
        const int64_t pack_uv = BatteryMonitor::packUvForSenseUv(mean_uv);
        b.interval_mv = static_cast<int>((pack_uv+500)/1000);
        history_[next_] = pack_uv;
        next_ = (next_+1) % history_.size();
        if (b.windows < static_cast<int>(history_.size())) ++b.windows;
        if (b.windows != static_cast<int>(history_.size())) return;
        int64_t total = 0;
        for (const auto uv : history_) total += uv;
        b.pack_mv = static_cast<int>((total + 2000) / 4000);
        b.estimated_mv = std::max(0, b.pack_mv - 100);
        b.percent = BatteryMonitor::percentForCellMv((b.estimated_mv + 3)/6);
        b.remaining_mah = (b.percent * 3300 + 50)/100;
    }
    Io& io_;
    mutable std::mutex mutex_;
    bool enabled_ = false, adc_ready_ = false, active_ = false, calibrated_ = true;
    uint32_t epoch_ = 0;
    uint64_t start_ = 0, next_start_ = 0, last_result_ = 0;
    unsigned batches_ = 0;
    int sum_uv_ = 0, sum_raw_ = 0;
    std::array<int64_t,4> history_{};
    size_t next_ = 0;
    const char* state_ = "startup";
    VoltageReport cached_;
};
} // namespace DevboardPower
