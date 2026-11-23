#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "audio_packet.hpp"

// AudioReassemblyError captures why a candidate packet stream was rejected.
enum class AudioReassemblyError {
    None,               // The most recent packet was accepted.
    InvalidPacket,      // Packet framing or lengths were malformed.
    MissingStart,       // A stream began without the required first marker.
    UnexpectedSequence, // The next packet number did not match the expectation.
    DuplicateOrOld,     // The packet sequence was already accepted and was ignored.
    SequenceGapTooLarge,// Too many packets were missing to keep this stream.
    DuplicateStart,     // A fresh "first" packet arrived mid-stream.
    AlreadyComplete     // Additional packets arrived after the stream ended.
};

constexpr uint16_t kMaxToleratedSequenceGap = 8;

// AudioReassembler rebuilds the original byte stream from ordered packets.
//
// Packets must begin at sequence 0 with the "first" flag set. After that, small
// forward gaps are tolerated and counted so one missed RF packet does not abort
// the entire stream; duplicates and old packets are ignored.
class AudioReassembler {
public:
    // Accept one packet and append its payload bytes if the stream is still valid.
    bool acceptPacket(const uint8_t* packet, size_t packet_len);
    // Clear all accumulated state so a new stream can begin.
    void reset();

    // Return the reassembled payload bytes gathered so far.
    const std::vector<uint8_t>& audio() const;
    // Report whether a valid start packet has been seen.
    bool started() const;
    // Report whether the final packet in the stream has been accepted.
    bool complete() const;
    // Report the sequence number the next valid packet must carry.
    uint16_t nextSequence() const;
    // Count packets skipped across tolerated forward sequence gaps.
    uint32_t missingPackets() const;
    // Expose why the most recent packet was rejected.
    AudioReassemblyError lastError() const;

private:
    std::vector<uint8_t> audio_;  // Reassembled payload byte stream.
    uint16_t next_sequence_ = 0;  // Next packet number required for acceptance.
    uint32_t missing_packets_ = 0; // Number of sequence slots skipped by tolerated gaps.
    bool started_ = false;        // True once a valid first packet was accepted.
    bool complete_ = false;       // True once the stream has received its last packet.
    AudioReassemblyError last_error_ = AudioReassemblyError::None;
};
