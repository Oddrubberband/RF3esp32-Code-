#pragma once

#include <cstdint>

namespace RadioChannel {

constexpr uint8_t kMaximum = 125;

// Nominal nRF24 center frequency: F0 = 2400 + RF_CH [MHz].
// This calculation never accesses the radio; zero denotes an invalid channel.
constexpr uint16_t frequencyMHz(uint8_t channel)
{
    return channel <= kMaximum ? static_cast<uint16_t>(2400u + channel) : 0;
}

}  // namespace RadioChannel
