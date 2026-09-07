#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "radio_channel.hpp"
#include "radio_manager.hpp"
#include "reliable_transfer_v2.hpp"

// Status readers only copy published values. The radio owner publishes while
// holding its own lock; this cache never accesses the radio or filesystem.
namespace AppStatus {

struct TransferReport {
    ReliableTransferV2::SenderState state = ReliableTransferV2::SenderState::Idle;
    ProtocolV2::ErrorCode error = ProtocolV2::ErrorCode::None;
    uint32_t transfer_id = 0;
    uint32_t bytes_transferred = 0;
    uint32_t total_bytes = 0;
    uint32_t current_sequence = 0;
    uint32_t total_packets = 0;
    uint32_t retry_count = 0;
    uint32_t crc32 = 0;
    uint64_t elapsed_ms = 0;
    uint32_t throughput_bps = 0; // Bytes per second; retained console field name.
    bool peer_complete = false;
    bool preparing = false;
};

struct RadioSnapshot {
    RadioStatus radio{};
    TransferReport tx{};
    uint64_t updated_ms = 0;
    bool rx_pending = false;
    uint32_t carrier_events = 0;
    uint32_t rx_stream = 0;
    uint32_t rx_raw = 0;
    uint32_t rx_missing = 0;
    uint32_t rx_duplicates = 0;
    uint32_t rx_drain_hits = 0;
    uint32_t rx_saved = 0;
    uint32_t rx_saved_bytes = 0;
    ReliableTransferV2::ReceiverState receiver_state = ReliableTransferV2::ReceiverState::Idle;
    ProtocolV2::ErrorCode receiver_error = ProtocolV2::ErrorCode::None;
    uint32_t receiver_id = 0;
    uint32_t receiver_bytes = 0;
    uint32_t receiver_total_bytes = 0;
    uint32_t receiver_sequence = 0;
    uint32_t receiver_packets = 0;
    uint32_t receiver_crc = 0;
};

struct Snapshot {
    RadioSnapshot status{};
    std::string selected_name;
    uint32_t selected_bytes = 0;
};

template <typename Mutex>
class SnapshotCache {
public:
    explicit SnapshotCache(Mutex& mutex) : mutex_(mutex) {}

    void publish(const RadioSnapshot& status)
    {
        Guard guard(mutex_);
        snapshot_.status = status;
    }

    void selectFile(std::string_view name, uint32_t bytes)
    {
        Guard guard(mutex_);
        snapshot_.selected_name.assign(name.data(), name.size());
        snapshot_.selected_bytes = bytes;
    }

    Snapshot capture() const
    {
        Guard guard(mutex_);
        return snapshot_;
    }

private:
    struct Guard {
        explicit Guard(Mutex& mutex) : mutex_(mutex) { mutex_.lock(); }
        ~Guard() { mutex_.unlock(); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        Mutex& mutex_;
    };
    Mutex& mutex_;
    Snapshot snapshot_{};
};

inline void appendJsonString(std::string& out, std::string_view value)
{
    constexpr char hex[] = "0123456789abcdef";
    out += '"';
    for (unsigned char ch : value) {
        if (ch == '"' || ch == '\\') {
            out += '\\';
            out += static_cast<char>(ch);
        } else if (ch < 0x20) {
            out += "\\u00";
            out += hex[ch >> 4];
            out += hex[ch & 0x0f];
        } else {
            out += static_cast<char>(ch);
        }
    }
    out += '"';
}

inline std::string buildJson(const Snapshot& snapshot,
                             std::string_view node_name,
                             std::string_view hostname)
{
    const RadioSnapshot& status = snapshot.status;
    const RadioStatus& radio = status.radio;
    const TransferReport& tx = status.tx;
    std::string json;
    json.reserve(1200);
    json += '{';
    const auto key = [&json](const char* name) {
        if (json.size() > 1) json += ',';
        appendJsonString(json, name);
        json += ':';
    };
    const auto text = [&json, &key](const char* name, std::string_view value) {
        key(name);
        appendJsonString(json, value);
    };
    const auto number = [&json, &key](const char* name, auto value) {
        key(name);
        json += std::to_string(value);
    };
    const auto boolean = [&json, &key](const char* name, bool value) {
        key(name);
        json += value ? "true" : "false";
    };
    text("node_name", node_name);
    text("hostname", hostname);
    text("state", RadioManager::stateName(radio.state));
    text("selected", snapshot.selected_name);
    number("selected_bytes", snapshot.selected_bytes);
    number("channel", radio.channel);
    number("frequency_mhz", RadioChannel::frequencyMHz(radio.channel));
    number("power", radio.power_level);
    boolean("tx_ok", radio.last_tx_ok);
    boolean("tx_timeout", radio.last_tx_timed_out);
    number("rx_packets", radio.rx_packets);
    number("rx_stream", status.rx_stream);
    number("rx_raw", status.rx_raw);
    number("rx_missing", status.rx_missing);
    number("rx_duplicates", status.rx_duplicates);
    number("rx_drain_hits", status.rx_drain_hits);
    number("rx_saved", status.rx_saved);
    number("rx_saved_bytes", status.rx_saved_bytes);
    number("last_fault", radio.last_fault);
    boolean("rx_pending", status.rx_pending);
    boolean("rpd", radio.carrier_detected);
    number("snapshot_updated_ms", status.updated_ms);
    number("protocol", ProtocolV2::kVersion);
    number("tx_id", tx.transfer_id);
    text("tx_state", ReliableTransferV2::senderStateName(tx.state));
    boolean("tx_preparing", tx.preparing);
    number("tx_bytes", tx.bytes_transferred);
    number("tx_total_bytes", tx.total_bytes);
    number("tx_seq", tx.current_sequence);
    number("tx_total_packets", tx.total_packets);
    number("tx_retries", tx.retry_count);
    text("tx_error", ProtocolV2::errorName(tx.error));
    number("tx_elapsed_ms", tx.elapsed_ms);
    number("tx_bytes_per_second", tx.throughput_bps);
    boolean("peer_complete", tx.peer_complete);
    number("rx_id", status.receiver_id);
    text("rx_state", ReliableTransferV2::receiverStateName(status.receiver_state));
    number("rx_bytes", status.receiver_bytes);
    number("rx_total_bytes", status.receiver_total_bytes);
    text("rx_error", ProtocolV2::errorName(status.receiver_error));
    json += '}';
    return json;
}

} // namespace AppStatus
