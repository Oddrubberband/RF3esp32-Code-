#include "audio_packet.hpp"

// The implementation here mirrors the compact on-air packet format documented
// in the header: a tiny fixed header plus a variable-length pulse-code
// modulation (PCM) body.
namespace AudioPacket {
bool encode(uint16_t sequence,
            const uint8_t* audio,
            size_t audio_len,
            bool is_first,
            bool is_last,
            uint8_t* out_packet,
            size_t& out_packet_len)
{
    // Always initialize the output length so callers never observe stale values
    // after a rejected encode attempt.
    out_packet_len = 0;

    if (!audio || !out_packet || audio_len == 0 || audio_len > kAudioBytesPerPacket) {
        return false;
    }

    // Header layout is sequence (little-endian), audio length, then bit flags.
    out_packet[0] = static_cast<uint8_t>(sequence & 0xFF);
    out_packet[1] = static_cast<uint8_t>((sequence >> 8) & 0xFF);
    out_packet[2] = static_cast<uint8_t>(audio_len);
    out_packet[3] = 0;

    if (is_first) {
        out_packet[3] |= kFirst;
    }
    if (is_last) {
        out_packet[3] |= kLast;
    }

    for (size_t i = 0; i < audio_len; ++i) {
        out_packet[kHeaderBytes + i] = audio[i];
    }

    out_packet_len = kHeaderBytes + audio_len;
    return true;
}

bool decode(const uint8_t* packet,
            size_t packet_len,
            Header& out_header,
            const uint8_t*& out_audio)
{
    // Clear the outputs first so callers can safely inspect them only on
    // success.
    out_audio = nullptr;
    out_header = {};

    if (!packet || packet_len < kHeaderBytes) {
        return false;
    }

    // Reject malformed packets before exposing a payload pointer to the caller.
    out_header.sequence = static_cast<uint16_t>(packet[0]) |
                          static_cast<uint16_t>(packet[1] << 8);
    out_header.audio_len = packet[2];
    out_header.flags = packet[3];

    if (out_header.audio_len == 0 || out_header.audio_len > kAudioBytesPerPacket) {
        return false;
    }

    // The packet is only valid when the declared audio size matches the actual
    // number of bytes supplied by the transport.
    if (packet_len != kHeaderBytes + out_header.audio_len) {
        return false;
    }

    out_audio = packet + kHeaderBytes;
    return true;
}
}  // namespace AudioPacket
