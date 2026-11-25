#pragma once
#include <cstdint>
namespace TaskTiming {
constexpr uint32_t nonzeroTicks(uint32_t milliseconds, uint32_t tick_hz) {
    const uint64_t ticks = static_cast<uint64_t>(milliseconds) * tick_hz / 1000u;
    return ticks == 0 ? 1u : ticks > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(ticks);
}
}
