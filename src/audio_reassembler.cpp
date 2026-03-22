#include "audio_reassembler.hpp"

bool AudioReassembler::acceptPacket(const uint8_t* packet, size_t packet_len)
{
    // First validate the packet shape. Reassembly only deals with already
    // decoded stream semantics.
    AudioPacket::Header header;
    const uint8_t* audio = nullptr;
    if (!AudioPacket::decode(packet, packet_len, header, audio)) {
        last_error_ = AudioReassemblyError::InvalidPacket;
        return false;
    }

    const bool is_first = (header.flags & AudioPacket::kFirst) != 0;
    const bool is_last = (header.flags & AudioPacket::kLast) != 0;

    if (complete_) {
        last_error_ = AudioReassemblyError::AlreadyComplete;
        return false;
    }

    // The first packet must mark the start of a stream; after that sequence numbers must be exact.
    if (!started_) {
        if (!is_first || header.sequence != 0) {
            last_error_ = AudioReassemblyError::MissingStart;
            return false;
        }
        started_ = true;
    } else {
        if (is_first) {
            last_error_ = AudioReassemblyError::DuplicateStart;
            return false;
        }
        if (header.sequence != next_sequence_) {
            last_error_ = AudioReassemblyError::UnexpectedSequence;
            return false;
        }
    }

    // The original stream is recovered by appending each packet's audio bytes in
    // order. There is no further compression or framing at this layer.
    audio_.insert(audio_.end(), audio, audio + header.audio_len);
    next_sequence_ = static_cast<uint16_t>(header.sequence + 1);
    complete_ = is_last;
    last_error_ = AudioReassemblyError::None;
    return true;
}

void AudioReassembler::reset()
{
    // Reset returns the object to the same state as a fresh construction so the
    // next accepted packet must again be the first packet in a new stream.
    audio_.clear();
    next_sequence_ = 0;
    started_ = false;
    complete_ = false;
    last_error_ = AudioReassemblyError::None;
}

const std::vector<uint8_t>& AudioReassembler::audio() const
{
    return audio_;
}

// The remaining accessors intentionally expose the reassembler's state machine
// for tests and any future user-interface (UI) or debug tooling.
bool AudioReassembler::started() const
{
    return started_;
}

bool AudioReassembler::complete() const
{
    return complete_;
}

uint16_t AudioReassembler::nextSequence() const
{
    return next_sequence_;
}

AudioReassemblyError AudioReassembler::lastError() const
{
    return last_error_;
}
