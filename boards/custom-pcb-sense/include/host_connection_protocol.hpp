#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace HostConnectionProtocol {
// Host-link identity is separate from the RF file-transfer protocol version.
constexpr unsigned kVersion = 1;

inline void appendJsonString(std::string& out, std::string_view value)
{
    constexpr char hex[] = "0123456789abcdef";
    out.push_back('"');
    for (const unsigned char byte : value) {
        if (byte == '"' || byte == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(byte));
        } else if (byte < 0x20) {
            out += "\\u00";
            out.push_back(hex[byte >> 4]);
            out.push_back(hex[byte & 0x0f]);
        } else {
            out.push_back(static_cast<char>(byte));
        }
    }
    out.push_back('"');
}

// This response needs no radio, filesystem, mutable snapshot, or command lock.
inline std::string hello(std::string_view node_name,
                         std::string_view profile,
                         uint8_t radio_protocol_version,
                         bool diagnostics = false,
                         std::string_view build = {},
                         unsigned max_channel = 0,
                         bool usb_host_sync = false)
{
    std::string response = "{\"rf3_protocol\":" + std::to_string(kVersion) +
                           ",\"node_name\":";
    appendJsonString(response, node_name);
    response += ",\"profile\":";
    appendJsonString(response, profile);
    response += ",\"protocol\":" + std::to_string(radio_protocol_version);
    if (diagnostics) {
        response += ",\"diagnostics\":true,\"build\":";
        appendJsonString(response, build);
    }
    if (max_channel)
        response += ",\"max_channel\":" + std::to_string(max_channel);
    if (usb_host_sync)
        response += ",\"usb_host_sync\":true";
    response += "}";
    return response;
}
}  // namespace HostConnectionProtocol
