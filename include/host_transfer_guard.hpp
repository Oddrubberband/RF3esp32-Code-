#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace HostTransferGuard {
inline bool alive(bool bluetooth, uint32_t epoch, int64_t touched, int64_t now,
                  bool connected, uint32_t current_epoch, int64_t timeout)
{
    return now - touched < timeout && (!bluetooth || (connected && epoch == current_epoch));
}

inline bool owns(uint8_t owner, uint32_t epoch, uint8_t origin, uint32_t command_epoch)
{
    return owner == origin && epoch == command_epoch;
}

// RF3 newline-delimited ASCII host protocol over UART or Bluetooth SIG SPP.
// Local echo is bounded and test-mode-only; it has no RF/flash side effects.
inline bool buildLocalEcho(bool devboard, bool test_mode, bool local_transport,
                           const std::string& text, size_t limit, std::string& response)
{
    response.clear();
    if (!devboard || !test_mode || !local_transport || text.empty() || text.size() > limit) {
        return false;
    }
    for (const unsigned char byte : text) {
        if (byte < 0x20 || byte > 0x7E) return false;
    }
    response = "ECHO TEXT: " + text;
    return true;
}
}  // namespace HostTransferGuard
