#pragma once

#include <cstddef>
#include <cstdint>

// AudioPacket defines the on-air packet format used by the demo.
//
// Each nRF24 payload is capped at 32 bytes, so the project reserves the first
// 4 bytes for metadata and uses the remaining bytes for raw 8 kHz unsigned
// pulse-code modulation (PCM) audio. The header lets the receiver tell where a
// stream starts, where it ends, and what sequence number each chunk belongs to.
namespace AudioPacket {
// The radio's fixed payload limit is 32 bytes.
constexpr size_t kPacketBytes = 32;
// Every packet starts with sequence, audio length, and flags.
constexpr size_t kHeaderBytes = 4;
// The rest of the payload can carry audio data.
constexpr size_t kAudioBytesPerPacket = kPacketBytes - kHeaderBytes;

// These flags mark stream boundaries for the receiver.
constexpr uint8_t kFirst = 0x01;
constexpr uint8_t kLast = 0x02;

struct Header {
    uint16_t sequence = 0;  // Monotonic chunk index within one transmitted file.
    uint8_t audio_len = 0;  // Number of valid pulse-code modulation (PCM) bytes that follow the header.
    uint8_t flags = 0;      // Boundary markers such as kFirst and kLast.
};

// Encode one chunk of pulse-code modulation (PCM) data into the project's wire
// format.
//
// The caller provides one slice of audio and two framing booleans. The function
// writes the packed packet into out_packet and reports how many bytes are valid.
// The output length may be less than 32 because the final packet is allowed to
// carry a partial audio chunk.
bool encode(uint16_t sequence,
            const uint8_t* audio,
            size_t audio_len,
            bool is_first,
            bool is_last,
            uint8_t* out_packet,
            size_t& out_packet_len);

// Decode one packet and point the caller at the embedded audio bytes.
//
// This validates the packet shape before exposing the payload pointer so higher
// layers can safely append the recovered pulse-code modulation (PCM) bytes to a
// reassembly buffer.
bool decode(const uint8_t* packet,
            size_t packet_len,
            Header& out_header,
            const uint8_t*& out_audio);
}  // namespace AudioPacket
