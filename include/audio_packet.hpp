#pragma once

#include <cstddef>
#include <cstdint>

namespace AudioPacket {
// Four bytes of metadata leave the remaining payload space for raw PCM audio.
constexpr size_t kPacketBytes = 32;
constexpr size_t kHeaderBytes = 4;
constexpr size_t kAudioBytesPerPacket = kPacketBytes - kHeaderBytes;

constexpr uint8_t kFirst = 0x01;
constexpr uint8_t kLast = 0x02;

struct Header {
    uint16_t sequence = 0;
    uint8_t audio_len = 0;
    uint8_t flags = 0;
};

// Pack one audio chunk into a single nRF24-sized payload.
bool encode(uint16_t sequence,
            const uint8_t* audio,
            size_t audio_len,
            bool is_first,
            bool is_last,
            uint8_t* out_packet,
            size_t& out_packet_len);

// Validate one packet and expose its decoded header plus audio span.
bool decode(const uint8_t* packet,
            size_t packet_len,
            Header& out_header,
            const uint8_t*& out_audio);
}  // namespace AudioPacket
