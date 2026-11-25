#pragma once

#include <cstddef>
#include <cstdint>

namespace TxHelpers {

constexpr uint8_t kDefaultSendAttempts = 3;
constexpr uint32_t kDefaultRetryDelayMs = 2;
constexpr uint32_t kAudioByteUs = 125;

struct PacingDelay {
    uint32_t delay_ms = 0;
    uint32_t remainder_us = 0;
};

inline PacingDelay calculateAudioPacingDelay(size_t bytes_read, uint32_t previous_remainder_us)
{
    const uint32_t chunk_total_us =
        static_cast<uint32_t>(bytes_read) * kAudioByteUs + previous_remainder_us;

    return {
        chunk_total_us / 1000u,
        chunk_total_us % 1000u
    };
}

template <typename SendFn, typename DelayFn>
bool sendWithRetry(SendFn send,
                   DelayFn delay,
                   uint8_t max_attempts = kDefaultSendAttempts,
                   uint32_t retry_delay_ms = kDefaultRetryDelayMs)
{
    if (max_attempts == 0) {
        return false;
    }

    for (uint8_t attempt = 0; attempt < max_attempts; ++attempt) {
        if (send()) {
            return true;
        }

        if ((attempt + 1u) < max_attempts && retry_delay_ms > 0) {
            delay(retry_delay_ms);
        }
    }

    return false;
}

}  // namespace TxHelpers
