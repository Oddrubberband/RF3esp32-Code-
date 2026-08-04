#pragma once

#include <cstddef>
#include <cstdint>

namespace TransferRateLimiter {

// A rate of zero means unlimited. The result is intentionally generic bytes
// per second; no audio sample rate or media type participates in scheduling.
inline uint32_t delayMilliseconds(size_t bytes, uint32_t bytes_per_second)
{
    if (bytes == 0 || bytes_per_second == 0) {
        return 0;
    }
    const uint64_t numerator = static_cast<uint64_t>(bytes) * 1000u;
    return static_cast<uint32_t>((numerator + bytes_per_second - 1u) /
                                 bytes_per_second);
}

}  // namespace TransferRateLimiter
