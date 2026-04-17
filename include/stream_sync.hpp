#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "audio_packet.hpp"

namespace StreamSync {

constexpr uint8_t kControlStart = 0x81;
constexpr uint8_t kControlStop = 0x82;
constexpr uint8_t kControlRemoteCommand = 0x83;

constexpr uint8_t kMagic0 = 'R';
constexpr uint8_t kMagic1 = 'F';
constexpr uint8_t kMagic2 = '3';
constexpr uint8_t kVersion = 1;
constexpr size_t kRemoteCommandHeaderBytes = 6;
constexpr size_t kRemoteCommandMaxBytes = AudioPacket::kPacketBytes - kRemoteCommandHeaderBytes;

constexpr uint8_t kRecommendedStartRepeats = 3;
constexpr uint32_t kRecommendedStartGapMs = 10;
constexpr uint32_t kRecommendedPostStartGapMs = 20;
constexpr uint32_t kRecommendedSeq0DuplicateGapMs = 2;

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
            state_ = State::WaitingForSeq0;
            return Action::StartAccepted;
        }

        if (decodeStop(packet, packet_len, control)) {
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
            return Action::Ignore;
        }

        if (out_header) {
            *out_header = header;
        }
        if (out_audio) {
            *out_audio = audio;
        }

        if (is_last) {
            state_ = State::WaitingForStart;
        }

        return Action::AudioAccepted;
    }

private:
    State state_ = State::WaitingForStart;
    uint16_t current_stream_id_ = 0;
};

}  // namespace StreamSync
