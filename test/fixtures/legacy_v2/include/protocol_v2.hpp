#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// ProtocolV2 defines the complete RF3 reliable-transfer wire codec. It has no
// dependency on ESP-IDF, FreeRTOS, a filesystem, or the radio driver.
namespace ProtocolV2 {

constexpr size_t kFrameSize = 32;
constexpr uint8_t kMagic0 = 'R';
constexpr uint8_t kMagic1 = 'F';
constexpr uint8_t kMagic2 = '2';
constexpr uint8_t kVersion = 2;
constexpr size_t kCommonHeaderBytes = 9;
constexpr size_t kDataHeaderBytes = 12;
constexpr size_t kDataPayloadCapacity = kFrameSize - kDataHeaderBytes;
constexpr uint32_t kMaxPacketCount = 65536u;
constexpr uint32_t kMaxFileSize = kMaxPacketCount * kDataPayloadCapacity;

constexpr uint32_t kControlResponseTimeoutMs = 500;
constexpr uint32_t kDataAckTimeoutMs = 250;
constexpr uint8_t kMaximumRetries = 5;
constexpr uint32_t kReceiverInactivityTimeoutMs = 10000;
constexpr uint32_t kSenderInactivityTimeoutMs = 10000;

static_assert(kFrameSize == 32, "Protocol v2 must use fixed 32-byte frames");
static_assert(kDataPayloadCapacity == 20, "Protocol v2 DATA capacity changed");
static_assert(kMaxFileSize == 1310720u, "Protocol v2 file-size limit changed");

using Frame = std::array<uint8_t, kFrameSize>;

enum class PacketType : uint8_t {
    Start = 1,
    Ready = 2,
    Data = 3,
    Ack = 4,
    Nack = 5,
    End = 6,
    Complete = 7,
    Error = 8,
    Cancel = 9,
};

enum class ErrorCode : uint8_t {
    None = 0,
    Busy = 1,
    UnsupportedVersion = 2,
    UnsupportedSize = 3,
    InvalidMetadata = 4,
    InvalidFrame = 5,
    WrongTransfer = 6,
    UnexpectedPacket = 7,
    UnexpectedSequence = 8,
    RetryExhausted = 9,
    Timeout = 10,
    Cancelled = 11,
    SourceRead = 12,
    SinkOpen = 13,
    SinkWrite = 14,
    SinkClose = 15,
    CrcMismatch = 16,
    PublicationFailed = 17,
    CleanupFailed = 18,
    PacketCountMismatch = 19,
    ByteCountMismatch = 20,
    TransportFailure = 21,
    StateViolation = 22,
};

inline bool isKnownErrorCode(ErrorCode error)
{
    return static_cast<uint8_t>(error) <=
           static_cast<uint8_t>(ErrorCode::StateViolation);
}

enum class DecodeStatus {
    Ok,
    NullArgument,
    WrongSize,
    InvalidMagic,
    UnsupportedVersion,
    UnknownPacketType,
    InvalidField,
    NonzeroReserved,
};

struct Packet {
    PacketType type = PacketType::Error;
    uint32_t transfer_id = 0;

    // START, END, and COMPLETE metadata.
    uint32_t total_size = 0;
    uint32_t total_packets = 0;
    uint32_t crc32 = 0;
    uint8_t flags = 0;

    // DATA and ACK sequence, and READY/NACK next expected sequence.
    uint16_t sequence = 0;
    uint32_t expected_sequence = 0;
    uint8_t payload_length = 0;
    std::array<uint8_t, kDataPayloadCapacity> payload{};

    // READY/COMPLETE status and NACK/ERROR/CANCEL reason.
    bool accepted = false;
    ErrorCode error = ErrorCode::None;
    uint32_t relevant_sequence = 0;
    uint8_t state = 0;
};

inline void storeLe16(uint8_t* out, uint16_t value)
{
    out[0] = static_cast<uint8_t>(value & 0xFFu);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}

inline void storeLe32(uint8_t* out, uint32_t value)
{
    out[0] = static_cast<uint8_t>(value & 0xFFu);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    out[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    out[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

inline uint16_t loadLe16(const uint8_t* in)
{
    return static_cast<uint16_t>(in[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(in[1]) << 8);
}

inline uint32_t loadLe32(const uint8_t* in)
{
    return static_cast<uint32_t>(in[0]) |
           (static_cast<uint32_t>(in[1]) << 8) |
           (static_cast<uint32_t>(in[2]) << 16) |
           (static_cast<uint32_t>(in[3]) << 24);
}

inline uint32_t packetCountForSize(uint32_t total_size)
{
    if (total_size == 0) {
        return 0;
    }
    return (total_size + static_cast<uint32_t>(kDataPayloadCapacity) - 1u) /
           static_cast<uint32_t>(kDataPayloadCapacity);
}

inline bool metadataSupported(uint32_t total_size, uint32_t total_packets)
{
    return total_size <= kMaxFileSize &&
           total_packets <= kMaxPacketCount &&
           total_packets == packetCountForSize(total_size);
}

inline size_t expectedPayloadLength(uint32_t total_size,
                                    uint32_t total_packets,
                                    uint32_t sequence)
{
    if (!metadataSupported(total_size, total_packets) ||
        total_packets == 0 || sequence >= total_packets) {
        return 0;
    }
    if (sequence + 1u < total_packets) {
        return kDataPayloadCapacity;
    }
    const uint32_t preceding =
        (total_packets - 1u) * static_cast<uint32_t>(kDataPayloadCapacity);
    return static_cast<size_t>(total_size - preceding);
}

inline bool hasMagic(const uint8_t* frame, size_t length)
{
    return frame && length == kFrameSize &&
           frame[0] == kMagic0 && frame[1] == kMagic1 && frame[2] == kMagic2;
}

inline bool peekTransferId(const uint8_t* frame, size_t length, uint32_t& transfer_id)
{
    transfer_id = 0;
    if (!hasMagic(frame, length)) {
        return false;
    }
    transfer_id = loadLe32(frame + 5);
    return transfer_id != 0;
}

inline bool isKnownPacketType(uint8_t raw)
{
    return raw >= static_cast<uint8_t>(PacketType::Start) &&
           raw <= static_cast<uint8_t>(PacketType::Cancel);
}

inline bool allZero(const uint8_t* frame, size_t begin, size_t end)
{
    for (size_t index = begin; index < end; ++index) {
        if (frame[index] != 0) {
            return false;
        }
    }
    return true;
}

inline bool writeCommon(const Packet& packet, Frame& out)
{
    out.fill(0);
    if (packet.transfer_id == 0 ||
        !isKnownPacketType(static_cast<uint8_t>(packet.type))) {
        return false;
    }
    out[0] = kMagic0;
    out[1] = kMagic1;
    out[2] = kMagic2;
    out[3] = kVersion;
    out[4] = static_cast<uint8_t>(packet.type);
    storeLe32(out.data() + 5, packet.transfer_id);
    return true;
}

inline bool encode(const Packet& packet, Frame& out)
{
    if (!writeCommon(packet, out)) {
        return false;
    }

    switch (packet.type) {
        case PacketType::Start:
            if (!metadataSupported(packet.total_size, packet.total_packets) ||
                packet.flags != 0) {
                return false;
            }
            storeLe32(out.data() + 9, packet.total_size);
            storeLe32(out.data() + 13, packet.total_packets);
            storeLe32(out.data() + 17, packet.crc32);
            out[21] = packet.flags;
            return true;

        case PacketType::Ready:
            if (packet.expected_sequence > kMaxPacketCount ||
                !isKnownErrorCode(packet.error) ||
                (packet.accepted && packet.error != ErrorCode::None) ||
                (!packet.accepted && packet.error == ErrorCode::None)) {
                return false;
            }
            storeLe32(out.data() + 9, packet.expected_sequence);
            out[13] = packet.accepted ? 1u : 0u;
            out[14] = static_cast<uint8_t>(packet.error);
            return true;

        case PacketType::Data:
            if (packet.payload_length == 0 ||
                packet.payload_length > kDataPayloadCapacity) {
                return false;
            }
            storeLe16(out.data() + 9, packet.sequence);
            out[11] = packet.payload_length;
            for (size_t index = 0; index < packet.payload_length; ++index) {
                out[kDataHeaderBytes + index] = packet.payload[index];
            }
            return true;

        case PacketType::Ack:
            storeLe16(out.data() + 9, packet.sequence);
            return true;

        case PacketType::Nack:
            if (packet.expected_sequence > kMaxPacketCount ||
                !isKnownErrorCode(packet.error) ||
                packet.error == ErrorCode::None) {
                return false;
            }
            storeLe32(out.data() + 9, packet.expected_sequence);
            out[13] = static_cast<uint8_t>(packet.error);
            return true;

        case PacketType::End:
            if (!metadataSupported(packet.total_size, packet.total_packets)) {
                return false;
            }
            storeLe32(out.data() + 9, packet.total_size);
            storeLe32(out.data() + 13, packet.total_packets);
            storeLe32(out.data() + 17, packet.crc32);
            return true;

        case PacketType::Complete:
            if (packet.total_size > kMaxFileSize ||
                !isKnownErrorCode(packet.error) ||
                packet.error != ErrorCode::None) {
                return false;
            }
            storeLe32(out.data() + 9, packet.total_size);
            storeLe32(out.data() + 13, packet.crc32);
            out[17] = static_cast<uint8_t>(packet.error);
            return true;

        case PacketType::Error:
            if (!isKnownErrorCode(packet.error) ||
                packet.error == ErrorCode::None) {
                return false;
            }
            out[9] = static_cast<uint8_t>(packet.error);
            storeLe32(out.data() + 10, packet.relevant_sequence);
            out[14] = packet.state;
            return true;

        case PacketType::Cancel:
            if (!isKnownErrorCode(packet.error) ||
                packet.error == ErrorCode::None) {
                return false;
            }
            out[9] = static_cast<uint8_t>(packet.error);
            return true;

        default:
            return false;
    }
}

inline DecodeStatus decode(const uint8_t* frame, size_t length, Packet& out)
{
    out = Packet{};
    if (!frame) {
        return DecodeStatus::NullArgument;
    }
    if (length != kFrameSize) {
        return DecodeStatus::WrongSize;
    }
    if (!hasMagic(frame, length)) {
        return DecodeStatus::InvalidMagic;
    }
    if (frame[3] != kVersion) {
        return DecodeStatus::UnsupportedVersion;
    }
    if (!isKnownPacketType(frame[4])) {
        return DecodeStatus::UnknownPacketType;
    }

    out.type = static_cast<PacketType>(frame[4]);
    out.transfer_id = loadLe32(frame + 5);
    if (out.transfer_id == 0) {
        return DecodeStatus::InvalidField;
    }

    switch (out.type) {
        case PacketType::Start:
            out.total_size = loadLe32(frame + 9);
            out.total_packets = loadLe32(frame + 13);
            out.crc32 = loadLe32(frame + 17);
            out.flags = frame[21];
            if (!metadataSupported(out.total_size, out.total_packets) ||
                out.flags != 0) {
                return DecodeStatus::InvalidField;
            }
            return allZero(frame, 22, kFrameSize)
                       ? DecodeStatus::Ok : DecodeStatus::NonzeroReserved;

        case PacketType::Ready:
            out.expected_sequence = loadLe32(frame + 9);
            if (frame[13] > 1u || out.expected_sequence > kMaxPacketCount) {
                return DecodeStatus::InvalidField;
            }
            out.accepted = frame[13] != 0;
            out.error = static_cast<ErrorCode>(frame[14]);
            if (!isKnownErrorCode(out.error) ||
                (out.accepted && out.error != ErrorCode::None) ||
                (!out.accepted && out.error == ErrorCode::None)) {
                return DecodeStatus::InvalidField;
            }
            return allZero(frame, 15, kFrameSize)
                       ? DecodeStatus::Ok : DecodeStatus::NonzeroReserved;

        case PacketType::Data:
            out.sequence = loadLe16(frame + 9);
            out.payload_length = frame[11];
            if (out.payload_length == 0 ||
                out.payload_length > kDataPayloadCapacity) {
                return DecodeStatus::InvalidField;
            }
            for (size_t index = 0; index < out.payload_length; ++index) {
                out.payload[index] = frame[kDataHeaderBytes + index];
            }
            return allZero(frame,
                           kDataHeaderBytes + out.payload_length,
                           kFrameSize)
                       ? DecodeStatus::Ok : DecodeStatus::NonzeroReserved;

        case PacketType::Ack:
            out.sequence = loadLe16(frame + 9);
            return allZero(frame, 11, kFrameSize)
                       ? DecodeStatus::Ok : DecodeStatus::NonzeroReserved;

        case PacketType::Nack:
            out.expected_sequence = loadLe32(frame + 9);
            out.error = static_cast<ErrorCode>(frame[13]);
            if (out.expected_sequence > kMaxPacketCount ||
                !isKnownErrorCode(out.error) ||
                out.error == ErrorCode::None) {
                return DecodeStatus::InvalidField;
            }
            return allZero(frame, 14, kFrameSize)
                       ? DecodeStatus::Ok : DecodeStatus::NonzeroReserved;

        case PacketType::End:
            out.total_size = loadLe32(frame + 9);
            out.total_packets = loadLe32(frame + 13);
            out.crc32 = loadLe32(frame + 17);
            if (!metadataSupported(out.total_size, out.total_packets)) {
                return DecodeStatus::InvalidField;
            }
            return allZero(frame, 21, kFrameSize)
                       ? DecodeStatus::Ok : DecodeStatus::NonzeroReserved;

        case PacketType::Complete:
            out.total_size = loadLe32(frame + 9);
            out.crc32 = loadLe32(frame + 13);
            out.error = static_cast<ErrorCode>(frame[17]);
            if (out.total_size > kMaxFileSize ||
                !isKnownErrorCode(out.error) ||
                out.error != ErrorCode::None) {
                return DecodeStatus::InvalidField;
            }
            return allZero(frame, 18, kFrameSize)
                       ? DecodeStatus::Ok : DecodeStatus::NonzeroReserved;

        case PacketType::Error:
            out.error = static_cast<ErrorCode>(frame[9]);
            out.relevant_sequence = loadLe32(frame + 10);
            out.state = frame[14];
            if (!isKnownErrorCode(out.error) ||
                out.error == ErrorCode::None) {
                return DecodeStatus::InvalidField;
            }
            return allZero(frame, 15, kFrameSize)
                       ? DecodeStatus::Ok : DecodeStatus::NonzeroReserved;

        case PacketType::Cancel:
            out.error = static_cast<ErrorCode>(frame[9]);
            if (!isKnownErrorCode(out.error) ||
                out.error == ErrorCode::None) {
                return DecodeStatus::InvalidField;
            }
            return allZero(frame, 10, kFrameSize)
                       ? DecodeStatus::Ok : DecodeStatus::NonzeroReserved;

        default:
            return DecodeStatus::UnknownPacketType;
    }
}

class Crc32 {
public:
    void reset() { state_ = 0xFFFFFFFFu; }

    void update(const uint8_t* data, size_t length)
    {
        if (!data) {
            return;
        }
        for (size_t index = 0; index < length; ++index) {
            state_ ^= data[index];
            for (uint8_t bit = 0; bit < 8; ++bit) {
                const uint32_t mask =
                    static_cast<uint32_t>(0u - static_cast<uint32_t>(state_ & 1u));
                state_ = (state_ >> 1) ^ (0xEDB88320u & mask);
            }
        }
    }

    uint32_t value() const { return state_ ^ 0xFFFFFFFFu; }

private:
    uint32_t state_ = 0xFFFFFFFFu;
};

inline uint32_t calculateCrc32(const uint8_t* data, size_t length)
{
    Crc32 crc;
    crc.update(data, length);
    return crc.value();
}

inline const char* packetTypeName(PacketType type)
{
    switch (type) {
        case PacketType::Start: return "START";
        case PacketType::Ready: return "READY";
        case PacketType::Data: return "DATA";
        case PacketType::Ack: return "ACK";
        case PacketType::Nack: return "NACK";
        case PacketType::End: return "END";
        case PacketType::Complete: return "COMPLETE";
        case PacketType::Error: return "ERROR";
        case PacketType::Cancel: return "CANCEL";
        default: return "UNKNOWN";
    }
}

inline const char* errorName(ErrorCode error)
{
    switch (error) {
        case ErrorCode::None: return "none";
        case ErrorCode::Busy: return "busy";
        case ErrorCode::UnsupportedVersion: return "unsupported-version";
        case ErrorCode::UnsupportedSize: return "unsupported-size";
        case ErrorCode::InvalidMetadata: return "invalid-metadata";
        case ErrorCode::InvalidFrame: return "invalid-frame";
        case ErrorCode::WrongTransfer: return "wrong-transfer";
        case ErrorCode::UnexpectedPacket: return "unexpected-packet";
        case ErrorCode::UnexpectedSequence: return "unexpected-sequence";
        case ErrorCode::RetryExhausted: return "retry-exhausted";
        case ErrorCode::Timeout: return "timeout";
        case ErrorCode::Cancelled: return "cancelled";
        case ErrorCode::SourceRead: return "source-read";
        case ErrorCode::SinkOpen: return "sink-open";
        case ErrorCode::SinkWrite: return "sink-write";
        case ErrorCode::SinkClose: return "sink-close";
        case ErrorCode::CrcMismatch: return "crc-mismatch";
        case ErrorCode::PublicationFailed: return "publication-failed";
        case ErrorCode::CleanupFailed: return "cleanup-failed";
        case ErrorCode::PacketCountMismatch: return "packet-count-mismatch";
        case ErrorCode::ByteCountMismatch: return "byte-count-mismatch";
        case ErrorCode::TransportFailure: return "transport-failure";
        case ErrorCode::StateViolation: return "state-violation";
        default: return "unknown";
    }
}

}  // namespace ProtocolV2
