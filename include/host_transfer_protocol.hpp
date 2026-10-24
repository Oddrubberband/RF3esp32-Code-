#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace HostTransferProtocol {
// This negotiation changes only the PC-to-ESP transport. RF packet width and
// Protocol v2 framing remain unchanged.
constexpr size_t kLegacyChunkBytes = 64;
constexpr size_t kMaxChunkBytes = 256;
constexpr size_t kConsoleLineBytes = 640;
static_assert(kConsoleLineBytes > 24 + 2 * kMaxChunkBytes,
              "The complete largest UPLOAD DATA command must fit the console");

inline bool negotiateChunk(bool requested, std::string_view value, size_t& chunk)
{
    if (!requested) {
        chunk = kLegacyChunkBytes;
        return true;
    }
    if (value.empty()) return false;
    size_t parsed = 0;
    for (const char byte : value) {
        if (byte < '0' || byte > '9') return false;
        parsed = parsed * 10 + static_cast<size_t>(byte - '0');
        // Reject before an arbitrarily long token could overflow size_t.
        if (parsed > kMaxChunkBytes) return false;
    }
    if (parsed < kLegacyChunkBytes) return false;
    chunk = parsed;
    return true;
}

inline bool validChunkLength(size_t length, size_t chunk)
{
    return chunk >= kLegacyChunkBytes && chunk <= kMaxChunkBytes &&
           length > 0 && length <= chunk;
}

inline size_t downloadChunkSize(size_t chunk, size_t remaining)
{
    return chunk >= kLegacyChunkBytes && chunk <= kMaxChunkBytes
        ? std::min(chunk, remaining) : 0;
}

inline bool matchesPrevious(uint32_t sequence, uint32_t next_sequence,
                            const uint8_t* data, size_t length,
                            const uint8_t* previous, size_t previous_length)
{
    return data != nullptr && previous != nullptr && length > 0 && length <= kMaxChunkBytes &&
           next_sequence > 0 && sequence == next_sequence - 1 &&
           length == previous_length &&
           std::equal(data, data + length, previous);
}

inline void appendReadyChunk(std::string& response, bool requested, size_t chunk)
{
    if (requested) response += " " + std::to_string(chunk);
}
}  // namespace HostTransferProtocol
