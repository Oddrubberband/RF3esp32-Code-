#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "audio_packet.hpp"

// FileSegmentation isolates the current file-to-packet chunking policy from
// ESP-IDF, SPIFFS, and the radio driver. The caller supplies a streaming read
// callback and remains responsible for encoding and transmitting each segment.
namespace FileSegmentation {

enum class ReadState {
    MoreDataMayFollow,
    EndOfFile,
    Error,
};

// bytes_read reports bytes successfully copied into the supplied buffer.
// state independently reports clean EOF or a read error, so a zero-byte result
// is never ambiguous.
struct ReadResult {
    size_t bytes_read = 0;
    ReadState state = ReadState::MoreDataMayFollow;

    ReadResult() = default;
    ReadResult(size_t successful_bytes, ReadState read_state)
        : bytes_read(successful_bytes), state(read_state)
    {
    }
};

using ReadCallback = ReadResult (*)(void* context, uint8_t* out, size_t capacity);

struct Segment {
    std::array<uint8_t, AudioPacket::kAudioBytesPerPacket> payload{};
    uint16_t sequence = 0;
    uint8_t payload_length = 0;
    bool first = false;
    bool last = false;
};

enum class NextResult {
    SegmentReady,
    EndOfFile,
    EmptyInput,
    ReadError,
    UnsupportedSize,
};

class Segmenter {
public:
    Segmenter(void* read_context, ReadCallback read_callback)
        : read_context_(read_context), read_callback_(read_callback)
    {
    }

    NextResult next(Segment& out)
    {
        out = Segment{};

        if (!read_callback_) {
            return NextResult::ReadError;
        }
        if (finished_) {
            return NextResult::EndOfFile;
        }
        if (next_sequence_ > static_cast<uint32_t>(UINT16_MAX)) {
            finished_ = true;
            return NextResult::UnsupportedSize;
        }

        ReadResult read;
        if (has_pending_) {
            read = ReadResult(pending_length_, pending_state_);
            for (size_t index = 0; index < pending_length_; ++index) {
                out.payload[index] = pending_payload_[index];
            }
            has_pending_ = false;
        } else {
            read = read_callback_(read_context_, out.payload.data(), out.payload.size());
        }

        if (read.bytes_read > out.payload.size() || read.state == ReadState::Error) {
            finished_ = true;
            return NextResult::ReadError;
        }

        if (read.bytes_read == 0) {
            finished_ = true;
            if (read.state != ReadState::EndOfFile) {
                return NextResult::ReadError;
            }
            return emitted_any_ ? NextResult::EndOfFile : NextResult::EmptyInput;
        }

        bool is_last = read.state == ReadState::EndOfFile;
        if (!is_last) {
            const ReadResult lookahead = read_callback_(read_context_,
                                                        pending_payload_.data(),
                                                        pending_payload_.size());
            if (lookahead.bytes_read > pending_payload_.size() ||
                lookahead.state == ReadState::Error) {
                finished_ = true;
                return NextResult::ReadError;
            }
            if (lookahead.bytes_read == 0) {
                if (lookahead.state != ReadState::EndOfFile) {
                    finished_ = true;
                    return NextResult::ReadError;
                }
                is_last = true;
            } else {
                pending_length_ = static_cast<uint8_t>(lookahead.bytes_read);
                pending_state_ = lookahead.state;
                has_pending_ = true;
            }
        }

        out.sequence = static_cast<uint16_t>(next_sequence_);
        out.payload_length = static_cast<uint8_t>(read.bytes_read);
        out.first = next_sequence_ == 0;
        out.last = is_last;

        emitted_any_ = true;
        ++next_sequence_;
        if (out.last) {
            finished_ = true;
        }
        return NextResult::SegmentReady;
    }

private:
    void* read_context_ = nullptr;
    ReadCallback read_callback_ = nullptr;
    std::array<uint8_t, AudioPacket::kAudioBytesPerPacket> pending_payload_{};
    uint32_t next_sequence_ = 0;
    uint8_t pending_length_ = 0;
    ReadState pending_state_ = ReadState::MoreDataMayFollow;
    bool has_pending_ = false;
    bool emitted_any_ = false;
    bool finished_ = false;
};

}  // namespace FileSegmentation
