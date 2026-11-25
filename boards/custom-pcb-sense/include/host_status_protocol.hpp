#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

#include "host_connection_protocol.hpp"
#include "devboard_power.hpp"

// The same complete status contract is serialized for USB and Classic SPP.
// No live hardware or task primitives are used by this wire-format helper.
namespace HostStatusProtocol {
constexpr size_t kMaxResponseBytes = 2302;  // CRLF occupies the other two bytes.
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
    uint32_t work = 0;
    uint32_t attempt = 0;
    uint32_t crc32 = 0;
    bool peer_complete = false;
    bool no_ack = false;
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
    bool rx_no_ack = false;
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
    bool usb_sync = false;
    const char* fem_lna = nullptr;
    unsigned fem_antenna = 0;
    const char* fem_mode = nullptr;
    // Additive RF3 UART / Bluetooth SIG SPP board telemetry. Millivolts are
    // measured at the ADC pin plus estimated 6S capacity; RF Protocol v2 is unchanged.
    bool board_power = false;
    DevboardPower::Reading sense;
    BatteryMonitor::Snapshot battery;
    bool power_good = false;
    std::optional<bool> nrf_rail;
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
    out.reserve(kMaxResponseBytes);
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
    if (s.usb_sync) boolean("usb_sync", true);
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
    if (s.filesystem_ready) {
        boolean("file_guard", true);
        boolean("upload_create", true);
    }
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
    boolean("tx_no_ack", s.tx.no_ack);
    if (s.tx.no_ack) number("tx_work", s.tx.work);
    if (s.fem_mode) {
        const std::string_view mode(s.fem_mode);
        if (mode == "off" || mode == "rx" || mode == "tx") text("fem_mode", mode);
    }
    if (s.fem_antenna == 1 || s.fem_antenna == 2) number("fem_antenna", s.fem_antenna);
    if (s.fem_lna) {
        const std::string_view lna(s.fem_lna);
        if (lna == "gain" || lna == "bypass") text("fem_lna", lna);
    }
    boolean("rx_no_ack", s.rx_no_ack);
    boolean("no_ack_files", true);
    transfer("rx", s.rx);
    if (s.rx.attempt) number("rx_attempt", s.rx.attempt);
    if (s.board_power) {
        enabled("board_power", true);
        number("sense_raw", s.sense.raw);
        number("sense_mv", s.sense.millivolts);
        boolean("sense_calibrated", s.sense.calibrated);
        boolean("sense_saturated", s.sense.saturated);
        boolean("sense_noisy", s.sense.noisy);
        boolean("power_good", s.power_good);
        if (s.nrf_rail.has_value()) boolean("nrf_rail", *s.nrf_rail);
        text("battery_profile","soikon_6s_3300");
        number("battery_interval_mv",s.battery.interval_mv);
        number("battery_windows",s.battery.windows);
        number("battery_mv",s.battery.pack_mv);
        number("battery_estimated_mv",s.battery.estimated_mv);
        number("battery_percent",s.battery.percent);
        number("battery_remaining_mah",s.battery.remaining_mah);
        number("battery_age_ms",s.battery.age_ms);
        text("battery_state",s.battery.state);
        boolean("battery_cutoff",s.battery.cutoff);
        boolean("battery_persisted",s.battery.persisted);
    }
    out.push_back('}');
    // Overflow is a failed snapshot, never a truncated JSON success. The native
    // worst-case contract test must continue to fit before new fields are added.
    if (out.size() > kMaxResponseBytes)
        return "{\"state\":\"Unavailable\",\"status_error\":\"response_too_large\"}";
    return out;
}
}  // namespace HostStatusProtocol
