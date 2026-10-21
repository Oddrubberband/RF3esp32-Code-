#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "audio_packet.hpp"

namespace StreamSync {

constexpr uint8_t kControlStart = 0x81;
constexpr uint8_t kControlStop = 0x82;
constexpr uint8_t kControlRemoteCommand = 0x83;
constexpr uint8_t kControlRemoteResponse = 0x84;
constexpr uint8_t kControlTextMessage = 0x85;

constexpr uint8_t kMagic0 = 'R';
constexpr uint8_t kMagic1 = 'F';
constexpr uint8_t kMagic2 = '3';
constexpr uint8_t kVersion = 1;
constexpr size_t kRemoteCommandHeaderBytes = 6;
constexpr size_t kRemoteCommandMaxBytes = AudioPacket::kPacketBytes - kRemoteCommandHeaderBytes;
constexpr size_t kTextMessageHeaderBytes = 10;
constexpr size_t kTextChunkMaxBytes = AudioPacket::kPacketBytes - kTextMessageHeaderBytes;
constexpr size_t kTextMessageMaxBytes = 154;
constexpr size_t kTextMessageMaxChunks =
    (kTextMessageMaxBytes + kTextChunkMaxBytes - 1) / kTextChunkMaxBytes;

constexpr uint8_t kRecommendedStartRepeats = 5;
constexpr uint32_t kRecommendedStartGapMs = 15;
constexpr uint32_t kRecommendedPostStartGapMs = 40;
constexpr uint32_t kRecommendedSeq0DuplicateGapMs = 8;
constexpr uint8_t kRecommendedStopRepeats = 3;
constexpr uint32_t kRecommendedStopGapMs = 8;
constexpr uint16_t kMaxToleratedSequenceGap = 8;

struct ControlFrame {
    uint16_t stream_id = 0;
};

inline void clearPacket(uint8_t* out_packet)
{
    if (!out_packet) {
        return;
    }

    for (size_t i = 0; i < AudioPacket::kPacketBytes; ++i) {
        out_packet[i] = 0;
    }
}

inline bool encodeControl(uint8_t control_type,
                          uint16_t stream_id,
                          uint8_t* out_packet,
                          size_t& out_packet_len)
{
    out_packet_len = 0;

    if (!out_packet) {
        return false;
    }

    clearPacket(out_packet);

    out_packet[0] = 0;
    out_packet[1] = 0;
    out_packet[2] = 0;
    out_packet[3] = control_type;

    out_packet[4] = kMagic0;
    out_packet[5] = kMagic1;
    out_packet[6] = kMagic2;
    out_packet[7] = kVersion;

    out_packet[8] = static_cast<uint8_t>((stream_id >> 8) & 0xFF);
    out_packet[9] = static_cast<uint8_t>(stream_id & 0xFF);

    out_packet_len = AudioPacket::kPacketBytes;
    return true;
}

inline bool encodeStart(uint16_t stream_id, uint8_t* out_packet, size_t& out_packet_len)
{
    return encodeControl(kControlStart, stream_id, out_packet, out_packet_len);
}

inline bool encodeStop(uint16_t stream_id, uint8_t* out_packet, size_t& out_packet_len)
{
    return encodeControl(kControlStop, stream_id, out_packet, out_packet_len);
}

inline bool decodeControl(uint8_t expected_type,
                          const uint8_t* packet,
                          size_t packet_len,
                          ControlFrame& out_frame)
{
    out_frame = {};

    if (!packet || packet_len != AudioPacket::kPacketBytes) {
        return false;
    }

    if (packet[2] != 0) {
        return false;
    }

    if (packet[3] != expected_type) {
        return false;
    }

    if (packet[4] != kMagic0 ||
        packet[5] != kMagic1 ||
        packet[6] != kMagic2 ||
        packet[7] != kVersion) {
        return false;
    }

    out_frame.stream_id =
        static_cast<uint16_t>(packet[8] << 8) |
        static_cast<uint16_t>(packet[9]);

    return true;
}

inline bool decodeStart(const uint8_t* packet, size_t packet_len, ControlFrame& out_frame)
{
    return decodeControl(kControlStart, packet, packet_len, out_frame);
}

inline bool decodeStop(const uint8_t* packet, size_t packet_len, ControlFrame& out_frame)
{
    return decodeControl(kControlStop, packet, packet_len, out_frame);
}

inline bool encodeRemoteCommand(const char* command,
                                size_t command_len,
                                uint8_t* out_packet,
                                size_t& out_packet_len)
{
    out_packet_len = 0;

    if (!command || !out_packet || command_len == 0 || command_len > kRemoteCommandMaxBytes) {
        return false;
    }

    clearPacket(out_packet);
    out_packet[0] = kMagic0;
    out_packet[1] = kMagic1;
    out_packet[2] = kMagic2;
    out_packet[3] = kControlRemoteCommand;
    out_packet[4] = kVersion;
    out_packet[5] = static_cast<uint8_t>(command_len);

    for (size_t i = 0; i < command_len; ++i) {
        const uint8_t ch = static_cast<uint8_t>(command[i]);
        if (ch < 0x20 || ch > 0x7E) {
            return false;
        }
        out_packet[kRemoteCommandHeaderBytes + i] = ch;
    }

    out_packet_len = AudioPacket::kPacketBytes;
    return true;
}

inline bool decodeRemoteCommand(const uint8_t* packet,
                                size_t packet_len,
                                std::string_view& out_command)
{
    out_command = {};

    if (!packet || packet_len != AudioPacket::kPacketBytes) {
        return false;
    }

    if (packet[0] != kMagic0 ||
        packet[1] != kMagic1 ||
        packet[2] != kMagic2 ||
        packet[3] != kControlRemoteCommand ||
        packet[4] != kVersion) {
        return false;
    }

    const size_t command_len = packet[5];
    if (command_len == 0 || command_len > kRemoteCommandMaxBytes) {
        return false;
    }

    for (size_t i = 0; i < command_len; ++i) {
        const uint8_t ch = packet[kRemoteCommandHeaderBytes + i];
        if (ch < 0x20 || ch > 0x7E) {
            return false;
        }
    }

    for (size_t i = kRemoteCommandHeaderBytes + command_len;
         i < AudioPacket::kPacketBytes;
         ++i) {
        if (packet[i] != 0) {
            return false;
        }
    }

    out_command = std::string_view(
        reinterpret_cast<const char*>(packet + kRemoteCommandHeaderBytes),
        command_len);
    return true;
}

inline bool encodeRemoteResponse(const char* text,
                                 size_t text_len,
                                 uint8_t* out_packet,
                                 size_t& out_packet_len)
{
    out_packet_len = 0;

    if (!text || !out_packet || text_len == 0 || text_len > kRemoteCommandMaxBytes) {
        return false;
    }

    clearPacket(out_packet);
    out_packet[0] = kMagic0;
    out_packet[1] = kMagic1;
    out_packet[2] = kMagic2;
    out_packet[3] = kControlRemoteResponse;
    out_packet[4] = kVersion;
    out_packet[5] = static_cast<uint8_t>(text_len);

    for (size_t i = 0; i < text_len; ++i) {
        const uint8_t ch = static_cast<uint8_t>(text[i]);
        if (!(ch == '\n' || ch == '\r' || ch == '\t' || (ch >= 0x20 && ch <= 0x7E))) {
            return false;
        }
        out_packet[kRemoteCommandHeaderBytes + i] = ch;
    }

    out_packet_len = AudioPacket::kPacketBytes;
    return true;
}

inline bool decodeRemoteResponse(const uint8_t* packet,
                                 size_t packet_len,
                                 std::string_view& out_text)
{
    out_text = {};

    if (!packet || packet_len != AudioPacket::kPacketBytes) {
        return false;
    }

    if (packet[0] != kMagic0 ||
        packet[1] != kMagic1 ||
        packet[2] != kMagic2 ||
        packet[3] != kControlRemoteResponse ||
        packet[4] != kVersion) {
        return false;
    }

    const size_t text_len = packet[5];
    if (text_len == 0 || text_len > kRemoteCommandMaxBytes) {
        return false;
    }

    for (size_t i = 0; i < text_len; ++i) {
        const uint8_t ch = packet[kRemoteCommandHeaderBytes + i];
        if (!(ch == '\n' || ch == '\r' || ch == '\t' || (ch >= 0x20 && ch <= 0x7E))) {
            return false;
        }
    }

    for (size_t i = kRemoteCommandHeaderBytes + text_len;
         i < AudioPacket::kPacketBytes;
         ++i) {
        if (packet[i] != 0) {
            return false;
        }
    }

    out_text = std::string_view(
        reinterpret_cast<const char*>(packet + kRemoteCommandHeaderBytes),
        text_len);
    return true;
}

struct TextChunk {
    uint16_t message_id = 0;
    uint8_t chunk_index = 0;
    uint8_t chunk_count = 0;
    std::string_view text{};
};

inline bool isTextMessagePacket(const uint8_t* packet, size_t packet_len)
{
    return packet &&
           packet_len == AudioPacket::kPacketBytes &&
           packet[0] == kMagic0 &&
           packet[1] == kMagic1 &&
           packet[2] == kMagic2 &&
           packet[3] == kControlTextMessage &&
           packet[4] == kVersion;
}

inline bool encodeTextChunk(uint16_t message_id,
                            uint8_t chunk_index,
                            uint8_t chunk_count,
                            const char* text,
                            size_t text_len,
                            uint8_t* out_packet,
                            size_t& out_packet_len)
{
    out_packet_len = 0;

    if (!text || !out_packet || message_id == 0 ||
        chunk_count == 0 || chunk_count > kTextMessageMaxChunks ||
        chunk_index >= chunk_count || text_len == 0 || text_len > kTextChunkMaxBytes ||
        (chunk_index + 1u < chunk_count && text_len != kTextChunkMaxBytes)) {
        return false;
    }

    clearPacket(out_packet);
    out_packet[0] = kMagic0;
    out_packet[1] = kMagic1;
    out_packet[2] = kMagic2;
    out_packet[3] = kControlTextMessage;
    out_packet[4] = kVersion;
    out_packet[5] = static_cast<uint8_t>((message_id >> 8) & 0xFF);
    out_packet[6] = static_cast<uint8_t>(message_id & 0xFF);
    out_packet[7] = chunk_index;
    out_packet[8] = chunk_count;
    out_packet[9] = static_cast<uint8_t>(text_len);

    for (size_t i = 0; i < text_len; ++i) {
        const uint8_t ch = static_cast<uint8_t>(text[i]);
        if (ch < 0x20 || ch > 0x7E) {
            out_packet_len = 0;
            return false;
        }
        out_packet[kTextMessageHeaderBytes + i] = ch;
    }

    out_packet_len = AudioPacket::kPacketBytes;
    return true;
}

inline bool decodeTextChunk(const uint8_t* packet,
                            size_t packet_len,
                            TextChunk& out_chunk)
{
    out_chunk = {};
    if (!isTextMessagePacket(packet, packet_len)) {
        return false;
    }

    const uint16_t message_id =
        static_cast<uint16_t>(packet[5] << 8) |
        static_cast<uint16_t>(packet[6]);
    const uint8_t chunk_index = packet[7];
    const uint8_t chunk_count = packet[8];
    const size_t text_len = packet[9];
    if (message_id == 0 || chunk_count == 0 || chunk_count > kTextMessageMaxChunks ||
        chunk_index >= chunk_count || text_len == 0 || text_len > kTextChunkMaxBytes ||
        (chunk_index + 1u < chunk_count && text_len != kTextChunkMaxBytes)) {
        return false;
    }

    for (size_t i = 0; i < text_len; ++i) {
        const uint8_t ch = packet[kTextMessageHeaderBytes + i];
        if (ch < 0x20 || ch > 0x7E) {
            return false;
        }
    }
    for (size_t i = kTextMessageHeaderBytes + text_len;
         i < AudioPacket::kPacketBytes;
         ++i) {
        if (packet[i] != 0) {
            return false;
        }
    }

    out_chunk.message_id = message_id;
    out_chunk.chunk_index = chunk_index;
    out_chunk.chunk_count = chunk_count;
    out_chunk.text = std::string_view(
        reinterpret_cast<const char*>(packet + kTextMessageHeaderBytes),
        text_len);
    return true;
}

class TextMessageAssembler {
public:
    enum class Action {
        NotText,
        Invalid,
        Ignored,
        InProgress,
        Complete
    };

    void reset()
    {
        active_ = false;
        message_id_ = 0;
        chunk_count_ = 0;
        next_chunk_ = 0;
        text_size_ = 0;
    }

    Action accept(const uint8_t* packet,
                  size_t packet_len,
                  std::string_view& out_message)
    {
        out_message = {};
        if (!isTextMessagePacket(packet, packet_len)) {
            return Action::NotText;
        }

        TextChunk chunk{};
        if (!decodeTextChunk(packet, packet_len, chunk)) {
            reset();
            return Action::Invalid;
        }

        if (chunk.chunk_index == 0) {
            reset();
            active_ = true;
            message_id_ = chunk.message_id;
            chunk_count_ = chunk.chunk_count;
        } else if (!active_) {
            return Action::Ignored;
        }

        if (chunk.message_id != message_id_ || chunk.chunk_count != chunk_count_) {
            reset();
            return Action::Invalid;
        }
        if (chunk.chunk_index < next_chunk_) {
            return Action::Ignored;
        }
        if (chunk.chunk_index != next_chunk_ ||
            text_size_ + chunk.text.size() > text_buffer_.size()) {
            reset();
            return Action::Invalid;
        }

        for (char ch : chunk.text) {
            text_buffer_[text_size_++] = ch;
        }
        ++next_chunk_;

        if (next_chunk_ < chunk_count_) {
            return Action::InProgress;
        }

        out_message = std::string_view(text_buffer_.data(), text_size_);
        active_ = false;
        return Action::Complete;
    }

private:
    std::array<char, kTextMessageMaxBytes> text_buffer_{};
    bool active_ = false;
    uint16_t message_id_ = 0;
    uint8_t chunk_count_ = 0;
    uint8_t next_chunk_ = 0;
    size_t text_size_ = 0;
};

class ReceiverGate {
public:
    enum class State {
        WaitingForStart,
        WaitingForSeq0,
        Streaming
    };

    enum class Action {
        Ignore,
        StartAccepted,
        StopAccepted,
        AudioAccepted,
        Invalid
    };

    void reset()
    {
        state_ = State::WaitingForStart;
        current_stream_id_ = 0;
        pending_stream_id_ = 0;
    }

    State state() const
    {
        return state_;
    }

    uint16_t currentStreamId() const
    {
        return current_stream_id_;
    }

    Action accept(const uint8_t* packet,
                  size_t packet_len,
                  AudioPacket::Header* out_header = nullptr,
                  const uint8_t** out_audio = nullptr)
    {
        if (out_header) {
            *out_header = {};
        }
        if (out_audio) {
            *out_audio = nullptr;
        }

        ControlFrame control{};
        if (decodeStart(packet, packet_len, control)) {
            current_stream_id_ = control.stream_id;
            pending_stream_id_ = 0;
            state_ = State::WaitingForSeq0;
            return Action::StartAccepted;
        }

        if (decodeStop(packet, packet_len, control)) {
            if (state_ == State::WaitingForSeq0 &&
                current_stream_id_ != 0 &&
                control.stream_id != current_stream_id_) {
                return Action::Ignore;
            }
            if (state_ == State::Streaming &&
                control.stream_id != current_stream_id_ &&
                control.stream_id != pending_stream_id_) {
                return Action::Ignore;
            }
            reset();
            return Action::StopAccepted;
        }

        AudioPacket::Header header{};
        const uint8_t* audio = nullptr;
        if (!AudioPacket::decode(packet, packet_len, header, audio)) {
            return Action::Invalid;
        }

        const bool is_first = (header.flags & AudioPacket::kFirst) != 0;
        const bool is_last = (header.flags & AudioPacket::kLast) != 0;

        if (state_ == State::WaitingForStart) {
            if (header.sequence == 0 && is_first) {
                pending_stream_id_ = 0;
                state_ = is_last ? State::WaitingForStart : State::Streaming;
                if (out_header) {
                    *out_header = header;
                }
                if (out_audio) {
                    *out_audio = audio;
                }
                return Action::AudioAccepted;
            }
            return Action::Ignore;
        }

        if (state_ == State::WaitingForSeq0) {
            if (header.sequence == 0 && is_first) {
                pending_stream_id_ = 0;
                state_ = is_last ? State::WaitingForStart : State::Streaming;
                if (out_header) {
                    *out_header = header;
                }
                if (out_audio) {
                    *out_audio = audio;
                }
                return Action::AudioAccepted;
            }
            return Action::Ignore;
        }

        if (header.sequence == 0 && is_first) {
            if (pending_stream_id_ != 0 && pending_stream_id_ != current_stream_id_) {
                current_stream_id_ = pending_stream_id_;
                pending_stream_id_ = 0;
                state_ = is_last ? State::WaitingForStart : State::Streaming;
                if (out_header) {
                    *out_header = header;
                }
                if (out_audio) {
                    *out_audio = audio;
                }
                return Action::AudioAccepted;
            }
            return Action::Ignore;
        }

        if (out_header) {
            *out_header = header;
        }
        if (out_audio) {
            *out_audio = audio;
        }

        if (is_last) {
            if (pending_stream_id_ != 0 && pending_stream_id_ != current_stream_id_) {
                current_stream_id_ = pending_stream_id_;
                pending_stream_id_ = 0;
                state_ = State::WaitingForSeq0;
            } else {
                pending_stream_id_ = 0;
                state_ = State::WaitingForStart;
            }
        }

        return Action::AudioAccepted;
    }

private:
    State state_ = State::WaitingForStart;
    uint16_t current_stream_id_ = 0;
    uint16_t pending_stream_id_ = 0;
};

}  // namespace StreamSync
