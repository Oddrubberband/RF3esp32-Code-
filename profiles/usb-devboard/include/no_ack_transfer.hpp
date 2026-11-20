#pragma once

#include <algorithm>
#include "reliable_transfer_v2.hpp"

// One-way frames have a distinct magic. A reliable receiver ignores them;
// a silent receiver accepts only these frames and discards all reply frames.
namespace NoAckTransfer {
constexpr uint32_t kStartGuardMs = 250;
constexpr uint32_t kDataGapMs = 10;

inline bool decode(const uint8_t* bytes, size_t length,
                   ProtocolV2::Packet& packet, ProtocolV2::Frame& reliable)
{
    if (!bytes || length != ProtocolV2::kFrameSize || bytes[0] != 'R' ||
        bytes[1] != 'F' || bytes[2] != 'N') return false;
    for (size_t i = 0; i < length; ++i) reliable[i] = bytes[i];
    reliable[2] = ProtocolV2::kMagic2;
    if (ProtocolV2::decode(reliable.data(), reliable.size(), packet) !=
        ProtocolV2::DecodeStatus::Ok) return false;
    return packet.type == ProtocolV2::PacketType::Start ||
           packet.type == ProtocolV2::PacketType::Data ||
           packet.type == ProtocolV2::PacketType::End ||
           packet.type == ProtocolV2::PacketType::Cancel;
}

inline bool encode(const ProtocolV2::Packet& packet, ProtocolV2::Frame& frame)
{
    if (packet.type != ProtocolV2::PacketType::Start &&
        packet.type != ProtocolV2::PacketType::Data &&
        packet.type != ProtocolV2::PacketType::End &&
        packet.type != ProtocolV2::PacketType::Cancel) return false;
    if (!ProtocolV2::encode(packet, frame)) return false;
    frame[2] = 'N';
    return true;
}

class Sender {
public:
    Sender(void* context, ReliableTransferV2::SourceCallbacks source)
        : context_(context), source_(source) {}

    bool begin(uint32_t id, std::string_view name)
    {
        metadata_ = {};
        sent_bytes_ = 0;
        sequence_ = 0;
        done_ = false;
        error_ = ProtocolV2::ErrorCode::None;
        id_ = id;
        if (!id || !FileTypeMetadata::derive(name, metadata_.extension))
            return fail(ProtocolV2::ErrorCode::InvalidMetadata);
        const auto extension = metadata_.extension;
        const auto result = ReliableTransferV2::inspectSource(context_, source_, metadata_);
        metadata_.extension = extension;
        if (result != ReliableTransferV2::InspectResult::Ready)
            return fail(result == ReliableTransferV2::InspectResult::UnsupportedSize
                            ? ProtocolV2::ErrorCode::UnsupportedSize
                            : ProtocolV2::ErrorCode::SourceRead);
        packet_ = {};
        packet_.type = ProtocolV2::PacketType::Start;
        setMetadata();
        return true;
    }

    bool frame(ProtocolV2::Frame& out) const
    {
        return !done_ && error_ == ProtocolV2::ErrorCode::None && NoAckTransfer::encode(packet_, out);
    }

    bool noteSent()
    {
        if (done_ || error_ != ProtocolV2::ErrorCode::None) return false;
        if (packet_.type == ProtocolV2::PacketType::End) { done_ = true; return true; }
        if (packet_.type == ProtocolV2::PacketType::Data) {
            sent_bytes_ += packet_.payload_length;
            ++sequence_;
        }
        packet_ = {};
        if (sent_bytes_ == metadata_.total_size) {
            packet_.type = ProtocolV2::PacketType::End;
            setMetadata();
            return true;
        }
        packet_.type = ProtocolV2::PacketType::Data;
        packet_.transfer_id = id_;
        packet_.sequence = static_cast<uint16_t>(sequence_);
        const size_t wanted = std::min<size_t>(ProtocolV2::kDataPayloadCapacity,
                                              metadata_.total_size - sent_bytes_);
        size_t read = 0;
        while (read < wanted) {
            const auto next = source_.read(context_, packet_.payload.data() + read, wanted - read);
            if (next.state == ReliableTransferV2::ReadState::Error || !next.bytes_read ||
                next.bytes_read > wanted - read) return fail(ProtocolV2::ErrorCode::SourceRead);
            read += next.bytes_read;
        }
        packet_.payload_length = static_cast<uint8_t>(read);
        return true;
    }

    bool done() const { return done_; }
    ProtocolV2::ErrorCode error() const { return error_; }
    uint32_t sentBytes() const { return sent_bytes_; }
    uint32_t sequence() const { return sequence_; }
    const ReliableTransferV2::Metadata& metadata() const { return metadata_; }

private:
    void setMetadata()
    {
        packet_.transfer_id = id_;
        packet_.total_size = metadata_.total_size;
        packet_.total_packets = metadata_.total_packets;
        packet_.crc32 = metadata_.crc32;
        if (packet_.type == ProtocolV2::PacketType::Start) {
            packet_.extension = metadata_.extension;
            packet_.flags = FileTypeMetadata::empty(metadata_.extension)
                                ? 0 : FileTypeMetadata::kStartExtensionFlag;
        }
    }
    bool fail(ProtocolV2::ErrorCode error) { error_ = error; return false; }
    void* context_;
    ReliableTransferV2::SourceCallbacks source_;
    ReliableTransferV2::Metadata metadata_{};
    ProtocolV2::Packet packet_{};
    uint32_t id_ = 0, sent_bytes_ = 0, sequence_ = 0;
    bool done_ = false;
    ProtocolV2::ErrorCode error_ = ProtocolV2::ErrorCode::None;
};
} // namespace NoAckTransfer
