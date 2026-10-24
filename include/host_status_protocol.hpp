#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "host_connection_protocol.hpp"

// The same complete status contract is serialized for USB and Classic SPP.
// No live hardware or task primitives are used by this wire-format helper.
namespace HostStatusProtocol {
constexpr size_t kMaxResponseBytes = 1534;  // CRLF occupies the other two bytes.
struct Transfer {
    const char* state_name = "Idle";
    const char* error_name = "none";
    std::string file_name;
    uint32_t id = 0;
    uint32_t bytes = 0;
    uint32_t total = 0;
    uint32_t sequence = 0;
    uint32_t packets = 0;
    uint32_t retries = 0;
    uint32_t crc32 = 0;
    bool peer_complete = false;
};
struct Snapshot {
    const char* node_name = "";
    const char* hostname = "";
    const char* profile = "";
    const char* state_name = "Boot";
    const char* loop_name = "None";
    bool supports_remote = false;
    bool remote_auto_rx = false;
    std::string selected_name;
    uint8_t channel = 76;
    int power_level = -1;
    bool tx_ok = false;
    bool test_mode = false;
    uint32_t test_tx_count = 0;
    uint32_t test_tx_bytes = 0;
    bool tx_timeout = false;
    bool rx_pending = false;
    bool rpd = false;
    uint32_t rx_packets = 0;
    uint32_t rx_stream = 0;
    uint32_t rx_raw = 0;
    uint32_t rx_missing = 0;
    uint32_t rx_saved = 0;
    uint32_t rx_saved_bytes = 0;
    std::string rx_last_name;
    std::string rx_last_path;
    uint32_t selected_bytes = 0;
    int last_fault = 0;
    bool filesystem_ready = false;
    bool test_mode_supported = false;
    unsigned host_chunk_max = 64;
    Transfer tx;
    Transfer rx;
};
inline std::string hex32(uint32_t value)
{
    char text[9]{};
    std::snprintf(text, sizeof(text), "%08lX", static_cast<unsigned long>(value));
    return text;
}
inline std::string json(const Snapshot& s)
{
    std::string out;
    out.reserve(1400);
    out.push_back('{');
    bool first = true;
    const auto key = [&](std::string_view name) {
        if (!first) out.push_back(',');
        first = false;
        HostConnectionProtocol::appendJsonString(out, name);
        out.push_back(':');
    };
    const auto text = [&](std::string_view name, std::string_view value) {
        key(name); HostConnectionProtocol::appendJsonString(out, value);
    };
    const auto number = [&](std::string_view name, auto value) {
        key(name); out += std::to_string(value);
    };
    const auto boolean = [&](std::string_view name, bool value) {
        key(name); out += value ? "true" : "false";
    };
    const auto enabled = [&](std::string_view name, bool value) {
        text(name, value ? "enabled" : "disabled");
    };
    text("node_name", s.node_name);
    text("hostname", s.hostname);
    text("profile", s.profile);
    text("state", s.state_name);
    text("selected", s.selected_name);
    number("selected_bytes", s.selected_bytes);
    number("channel", s.channel);
    // The -1 sentinel is retained on the wire for old hosts; new hosts render Unknown.
    number("power", s.power_level);
    boolean("tx_ok", s.tx_ok);
    boolean("tx_timeout", s.tx_timeout);
    number("rx_packets", s.rx_packets);
    number("rx_stream", s.rx_stream);
    number("rx_raw", s.rx_raw);
    number("rx_missing", s.rx_missing);
    number("rx_saved", s.rx_saved);
    number("rx_saved_bytes", s.rx_saved_bytes);
    text("rx_last", s.rx_last_name);
    text("rx_last_path", s.rx_last_path);
    number("last_fault", s.last_fault);
    boolean("rx_pending", s.rx_pending);
    boolean("rpd", s.rpd);
    boolean("test_mode", s.test_mode);
    boolean("test_mode_supported", s.test_mode_supported);
    number("test_tx_count", s.test_tx_count);
    number("test_tx_bytes", s.test_tx_bytes);
    text("test_tx_state", s.test_tx_count ? "Completed" : "Idle");
    enabled("text", true);
    enabled("host_upload", s.filesystem_ready);
    enabled("host_download", s.filesystem_ready);
    enabled("host_retry", true);
    enabled("host_storage", s.filesystem_ready);
    enabled("host_echo", s.test_mode_supported);
    boolean("host_stored_tx", s.filesystem_ready);
    number("host_chunk_max", s.host_chunk_max);
    number("protocol", 2);
    text("loop", s.loop_name);
    enabled("remote", s.supports_remote);
    boolean("remote_auto_rx", s.remote_auto_rx);
    const auto transfer = [&](std::string prefix, const Transfer& t) {
        text(prefix + "_id", hex32(t.id));
        text(prefix + "_state", t.state_name);
        text(prefix + "_bytes", std::to_string(t.bytes) + "/" + std::to_string(t.total));
        text(prefix + "_seq", std::to_string(t.sequence) + "/" + std::to_string(t.packets));
        text(prefix + "_error", t.error_name);
        text(prefix + "_crc32", hex32(t.crc32));
    };
    transfer("tx", s.tx);
    text("tx_file", s.tx.file_name);
    number("tx_retries", s.tx.retries);
    boolean("peer_complete", s.tx.peer_complete);
    transfer("rx", s.rx);
    out.push_back('}');
    // Overflow is a failed snapshot, never a truncated JSON success. The native
    // worst-case contract test must continue to fit before new fields are added.
    if (out.size() > kMaxResponseBytes)
        return "{\"state\":\"Unavailable\",\"status_error\":\"response_too_large\"}";
    return out;
}
}  // namespace HostStatusProtocol
