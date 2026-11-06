#pragma once
#include <atomic>
#include <mutex>
#include "battery_monitor.hpp"

// Board interface contract: ESP32-WROOM pad numbers are documentation only;
// ESP-IDF GPIO/ADC APIs use GPIO numbers. RF3 STATUS uses additive ASCII
// key=value fields / JSON integer millivolts over UART or Bluetooth SPP;
// this does not change the RF Protocol v2 wire format.
namespace DevboardPower {
constexpr int kPowerGoodGpio = 15;  // Module pad 23, healthy startup HIGH; latched cutoff LOW.
constexpr int kSenseGpio = 34;      // Module pad 6, ADC1 channel 6, input only.
constexpr int kNrfRailGpio = 21;    // Module pad 33, active-HIGH external rail request.
constexpr int kReservedLedGpio = 22;   // Module pad 36, left untouched.
constexpr int kReservedServoGpio = 25; // Module pad 10, left untouched.

struct Reading {
    int raw = -1;
    int millivolts = -1;
    bool calibrated = false; // eFuse calibration; otherwise nominal-reference estimate.
    bool saturated = false;
    bool noisy = false;
    int microvolts = -1;
};

struct Io {
    virtual ~Io() = default;
    virtual bool output(int gpio, bool high) = 0;
    virtual bool beginAdc(int gpio) = 0;
    virtual Reading readAdc() = 0;
    virtual bool loadCutoff(bool& latched) { latched=false; return true; }
    virtual bool storeCutoff(bool) { return true; }
};

class Controller {
public:
    Controller(Io& io, bool supported) : io_(io), supported_(supported) {}

    bool initialize()
    {
        if (!supported_) return true;
        // Load the trip BEFORE actively asserting the enable. Do not insert a
        // LOW pulse on a healthy battery keep-alive output. Reset/ROM GPIO
        // levels are a separate external-circuit contract, not guaranteed here.
        bool latched=false;
        if (!io_.loadCutoff(latched)) {
            (void)io_.output(kPowerGoodGpio,false);
            return false;
        }
        if (!io_.output(kPowerGoodGpio, !latched)) return false;
        power_good_ = !latched;
        if (!io_.output(kNrfRailGpio, !latched)) return false;
        nrf_rail_ = !latched;
        if (latched) monitor_.latch(true);
        adc_ready_ = io_.beginAdc(kSenseGpio);
        return true;
    }

    // STATUS reads cache only: ADC/protection service is independent of a GUI.
    Reading sample() const { std::lock_guard<std::mutex> lock(mutex_); return reading_; }
    BatteryMonitor::Snapshot battery(uint64_t now) const {
        std::lock_guard<std::mutex> lock(mutex_); return monitor_.snapshot(now);
    }
    bool poll(uint64_t now, bool quiet, uint64_t radio_activity_ms = 0,
              bool active_check = false, unsigned radio_context = 0)
    {
        if (!supported_) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!monitor_.due(now, quiet, radio_activity_ms, active_check, radio_context)) return false;
        const Reading value = adc_ready_ ? io_.readAdc() : Reading{};
        const bool valid = value.raw >= 0 && value.raw <= 4095 &&
            value.millivolts >= 0 && value.millivolts <= 3300 &&
            value.microvolts <= 3300000 && !value.saturated && !value.noisy;
        const int sense_uv = value.microvolts >= 0 ? value.microvolts :
            valid ? value.millivolts * 1000 : -1;
        const uint32_t serial = monitor_.readingSerial();
        const bool trip = monitor_.observe(now, sense_uv, value.raw, value.calibrated, valid);
        if (!valid) reading_ = value;
        else if (serial != monitor_.readingSerial())
            reading_ = {monitor_.raw(), monitor_.senseMv(), monitor_.calibrated(), false, false};
        return trip;
    }
    bool activeCheckDue(uint64_t now) const
    {
        if (!supported_) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        return monitor_.activeCheckDue(now);
    }
    void endActiveCheck(uint64_t now)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        monitor_.endActiveCheck(now);
    }
    uint32_t readingSerial() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return monitor_.readingSerial();
    }
    bool requestCutoff()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!supported_ || monitor_.cutoff()) return false;
        monitor_.latch(false);
        return true;
    }
    // Caller quiesces/releases radio signals BEFORE removing either rail.
    bool applyCutoff(uint64_t now=0) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!monitor_.cutoff()) monitor_.latch(false);
        if (!monitor_.persisted() && (!store_attempted_ || now-last_store_attempt_ms_>=1000)) {
            store_attempted_=true; last_store_attempt_ms_=now;
            monitor_.setPersisted(io_.storeCutoff(true));
        }
        if (io_.output(kNrfRailGpio,false)) nrf_rail_=false;
        if (io_.output(kPowerGoodGpio,false)) power_good_=false;
        return !power_good_ && !nrf_rail_;
    }
    bool resetCutoff(uint64_t now) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!monitor_.canReset(now) || !io_.storeCutoff(false)) return false;
        if (!io_.output(kPowerGoodGpio,true)) {
            monitor_.setPersisted(io_.storeCutoff(true)); return false;
        }
        power_good_=true; monitor_.reset();
        return true; // nRF rail deliberately stays OFF until requested.
    }
    bool allowRf(uint64_t now) const {
        if (!supported_) return true;
        std::lock_guard<std::mutex> lock(mutex_); return monitor_.allowRf(now);
    }
    bool supported() const { return supported_; }
    bool powerGood() const { return power_good_; }
    bool nrfRail() const { return !supported_ || nrf_rail_; }
    bool setNrfRail(bool enabled)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!supported_ || (enabled && (monitor_.cutoff() || !power_good_)) ||
            !io_.output(kNrfRailGpio, enabled)) return false;
        nrf_rail_ = enabled;
        return true;
    }

private:
    Io& io_;
    bool supported_ = false;
    std::atomic_bool power_good_{false};
    std::atomic_bool nrf_rail_{false};
    bool adc_ready_ = false;
    Reading reading_;
    BatteryMonitor::Monitor monitor_;
    mutable std::mutex mutex_;
    bool store_attempted_=false;
    uint64_t last_store_attempt_ms_=0;
};
} // namespace DevboardPower
