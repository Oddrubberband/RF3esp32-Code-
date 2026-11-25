#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace RuntimeDiagnostics {
#if RF3_USB_EBYTE
inline constexpr const char* kBuild = "RF3-R28-EBYTE";
#elif RF3_AUTOMATIC_FEM
inline constexpr const char* kBuild = "RF3-NFEM-R4";
#else
inline constexpr const char* kBuild = "RF3-CBAT-R1-20261109";
#endif
inline constexpr size_t kJsonCapacity = 1280;

// All lifetime totals saturate at UINT32_MAX rather than wrapping. Snapshots
// load each independent atomic; fields are diagnostic aggregates, not one
// transactionally coherent instant. No radio/command mutex is required.
class Counter {
public:
    void add(uint64_t amount = 1) {
        uint32_t previous = value_.load(std::memory_order_relaxed);
        while (true) {
            const uint32_t next = amount > UINT32_MAX - previous
                ? UINT32_MAX : previous + static_cast<uint32_t>(amount);
            if (value_.compare_exchange_weak(previous, next, std::memory_order_relaxed)) return;
        }
    }
    void maximum(uint64_t value) {
        const uint32_t bounded = value > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(value);
        uint32_t previous = value_.load(std::memory_order_relaxed);
        while (previous < bounded &&
               !value_.compare_exchange_weak(previous, bounded, std::memory_order_relaxed)) {}
    }
    uint32_t value() const { return value_.load(std::memory_order_relaxed); }
private:
    std::atomic<uint32_t> value_{0};
};
struct Timing {
    Counter count, sum_us, max_us;
    void record(uint64_t microseconds) { count.add(); sum_us.add(microseconds); max_us.maximum(microseconds); }
};
struct Counters {
    Counter tx_ds, tx_fifo_only, tx_recoveries, tx_attempt_timeouts, tx_max_rt;
    Counter ack_timeouts, rejected_responses;
    Timing rx_poll, storage_write, ack_wait;
};
inline Counters& global() { static Counters counters; return counters; }

// Profile is one of the compile-time hardware-profile names, never user input.
inline std::string json(const Counters& d, const char* profile, uint32_t tick_hz) {
    char buffer[kJsonCapacity]{};
    const int length = std::snprintf(buffer, sizeof(buffer),
        "{\"type\":\"diag\",\"v\":1,\"build\":\"%s\",\"profile\":\"%s\",\"tick_hz\":%lu,\"rx_poll_ms\":2,"
        "\"tx_ds\":%lu,\"tx_fifo_only\":%lu,\"tx_recoveries\":%lu,\"tx_attempt_timeouts\":%lu,\"tx_max_rt\":%lu,"
        "\"ack_timeouts\":%lu,\"rejected_responses\":%lu,"
        "\"rx_poll_count\":%lu,\"rx_poll_us_sum\":%lu,\"rx_poll_us_max\":%lu,"
        "\"storage_write_count\":%lu,\"storage_write_us_sum\":%lu,\"storage_write_us_max\":%lu,"
        "\"ack_wait_count\":%lu,\"ack_wait_us_sum\":%lu,\"ack_wait_us_max\":%lu}",
        kBuild, profile, static_cast<unsigned long>(tick_hz),
        static_cast<unsigned long>(d.tx_ds.value()), static_cast<unsigned long>(d.tx_fifo_only.value()),
        static_cast<unsigned long>(d.tx_recoveries.value()), static_cast<unsigned long>(d.tx_attempt_timeouts.value()),
        static_cast<unsigned long>(d.tx_max_rt.value()), static_cast<unsigned long>(d.ack_timeouts.value()),
        static_cast<unsigned long>(d.rejected_responses.value()),
        static_cast<unsigned long>(d.rx_poll.count.value()), static_cast<unsigned long>(d.rx_poll.sum_us.value()),
        static_cast<unsigned long>(d.rx_poll.max_us.value()),
        static_cast<unsigned long>(d.storage_write.count.value()), static_cast<unsigned long>(d.storage_write.sum_us.value()),
        static_cast<unsigned long>(d.storage_write.max_us.value()),
        static_cast<unsigned long>(d.ack_wait.count.value()), static_cast<unsigned long>(d.ack_wait.sum_us.value()),
        static_cast<unsigned long>(d.ack_wait.max_us.value()));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(buffer))
        return "{\"type\":\"diag\",\"v\":1,\"error\":\"encoding_failed\"}";
    return std::string(buffer, static_cast<size_t>(length));
}
}  // namespace RuntimeDiagnostics
