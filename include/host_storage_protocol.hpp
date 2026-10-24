#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "host_connection_protocol.hpp"

namespace HostStorageProtocol {
constexpr size_t kFilenameMaxBytes = 30;
constexpr size_t kPageSize = 4;
constexpr size_t kMaxResponseBytes = 1534;  // Leave room for CRLF in the SPP message.

struct File {
    std::string name;
    size_t bytes = 0;
};

inline bool validUtf8(std::string_view text)
{
    size_t index = 0;
    while (index < text.size()) {
        const uint8_t lead = static_cast<uint8_t>(text[index++]);
        if (lead < 0x80) continue;
        size_t count = 0;
        uint32_t codepoint = 0;
        uint32_t minimum = 0;
        if (lead >= 0xc2 && lead <= 0xdf) {
            count = 1; codepoint = lead & 0x1f; minimum = 0x80;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            count = 2; codepoint = lead & 0x0f; minimum = 0x800;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            count = 3; codepoint = lead & 0x07; minimum = 0x10000;
        } else return false;
        if (count > text.size() - index) return false;
        while (count--) {
            const uint8_t tail = static_cast<uint8_t>(text[index++]);
            if ((tail & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (tail & 0x3f);
        }
        if (codepoint < minimum || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
    }
    return true;
}

inline bool protectedName(std::string_view name)
{
    if (name.empty() || name.front() == '.') return true;
    if (name.size() < 5) return false;
    const auto lower = [](char byte) {
        return byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte - 'A' + 'a') : byte;
    };
    const std::string_view suffix = name.substr(name.size() - 5);
    return suffix[0] == '.' && lower(suffix[1]) == 'p' && lower(suffix[2]) == 'a' &&
           lower(suffix[3]) == 'r' && lower(suffix[4]) == 't';
}

struct DeleteGuards {
    bool host_transfer_active = false;
    bool loop_active = false;
    bool radio_active = false;
    bool receive_active = false;
    bool recovery_pending = false;
};

inline const char* deleteError(const DeleteGuards& guards)
{
    if (guards.host_transfer_active) return "host_transfer_active";
    if (guards.loop_active || guards.radio_active || guards.receive_active) return "file_in_use_stop_radio_first";
    if (guards.recovery_pending) return "upload_recovery_pending";
    return nullptr;
}

// Both encoded operations reject active ownership conflicts. SELECT does not
// need a healthy RF chip; TX must have a known, ready radio before acceptance.
struct StoredGuards {
    bool host_transfer_active = false;
    bool loop_active = false;
    bool radio_active = false;
    bool receive_active = false;
    bool recovery_pending = false;
    bool radio_ready = false;
    bool test_mode = false;
};
inline const char* storedOperationError(const StoredGuards& guards, bool transmit)
{
    if (guards.host_transfer_active) return "host_transfer_active";
    if (guards.loop_active || guards.radio_active || guards.receive_active) return "file_in_use_stop_radio_first";
    if (guards.recovery_pending) return "upload_recovery_pending";
    if (transmit && guards.test_mode) return "test_mode_blocks_RF_TX";
    if (transmit && !guards.radio_ready) return "radio_not_ready";
    return nullptr;
}

inline bool validFilename(std::string_view name)
{
    if (name.size() > kFilenameMaxBytes || protectedName(name) ||
        name.find("..") != std::string_view::npos || !validUtf8(name)) return false;
    return std::none_of(name.begin(), name.end(), [](unsigned char byte) {
        return byte < 0x20 || byte == 0x7f || byte == '/' || byte == '\\';
    });
}

inline std::string encodeName(std::string_view name)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(name.size() * 2);
    for (const unsigned char byte : name) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0xf]);
    }
    return out;
}

inline bool decodeName(std::string_view hex, std::string& name)
{
    name.clear();
    if (hex.empty() || hex.size() % 2 || hex.size() > kFilenameMaxBytes * 2) return false;
    const auto nibble = [](char byte) -> int {
        if (byte >= '0' && byte <= '9') return byte - '0';
        if (byte >= 'a' && byte <= 'f') return byte - 'a' + 10;
        if (byte >= 'A' && byte <= 'F') return byte - 'A' + 10;
        return -1;
    };
    std::string decoded;
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        decoded.push_back(static_cast<char>((hi << 4) | lo));
    }
    if (!validFilename(decoded)) return false;
    name = std::move(decoded);
    return true;
}

inline std::string error(std::string_view message)
{
    std::string out = "{\"storage\":{\"op\":\"error\",\"message\":";
    HostConnectionProtocol::appendJsonString(out, message);
    out += "}}";
    return out;
}

inline std::string deleted(std::string_view name)
{
    std::string out = "{\"storage\":{\"op\":\"delete\",\"name\":";
    HostConnectionProtocol::appendJsonString(out, name);
    out += "}}";
    return out;
}

// The caller supplies a complete, sorted, visible-file snapshot. Pagination
// never serializes protected files and rejects unsupported names explicitly.
inline std::string listing(const std::vector<File>& files, size_t offset,
                           size_t used, size_t capacity)
{
    if (offset > files.size()) return error("invalid_offset");
    const size_t count = std::min(kPageSize, files.size() - offset);
    const size_t end = offset + count;
    std::string out = "{\"storage\":{\"op\":\"list\",\"offset\":" + std::to_string(offset) +
        ",\"next\":" + (end < files.size() ? std::to_string(end) : "null") +
        ",\"total\":" + std::to_string(files.size()) + ",\"used\":" + std::to_string(used) +
        ",\"capacity\":" + std::to_string(capacity) + ",\"files\":[";
    for (size_t i = offset; i < end; ++i) {
        if (!validFilename(files[i].name)) return error("unsupported_visible_filename");
        if (i != offset) out.push_back(',');
        out += "{\"name\":";
        HostConnectionProtocol::appendJsonString(out, files[i].name);
        out += ",\"size\":" + std::to_string(files[i].bytes) + "}";
    }
    out += "]}}";
    return out.size() <= kMaxResponseBytes ? out : error("response_too_large");
}
}  // namespace HostStorageProtocol
