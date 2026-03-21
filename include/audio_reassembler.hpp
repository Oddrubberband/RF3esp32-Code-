#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "audio_packet.hpp"

enum class AudioReassemblyError {
    None,
    InvalidPacket,
    MissingStart,
    UnexpectedSequence,
    DuplicateStart,
    AlreadyComplete
};

// Rebuild a raw audio stream from ordered AudioPacket chunks.
class AudioReassembler {
public:
    bool acceptPacket(const uint8_t* packet, size_t packet_len);
    void reset();

    const std::vector<uint8_t>& audio() const;
    bool started() const;
    bool complete() const;
    uint16_t nextSequence() const;
    AudioReassemblyError lastError() const;

private:
    std::vector<uint8_t> audio_;
    uint16_t next_sequence_ = 0;
    bool started_ = false;
    bool complete_ = false;
    AudioReassemblyError last_error_ = AudioReassemblyError::None;
};
