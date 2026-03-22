#pragma once

#include <cstdint>
#include <string>
#include <vector>

// FrameRecord is a simple serializable view of one radio event.
//
// It is used by helper utilities and native-side tests that want to log radio
// traffic as lines of text and later parse those lines back into structured
// packet metadata plus raw payload bytes.
struct FrameRecord {
    bool is_tx = false;              // True for transmitted frames, false for received frames.
    uint64_t timestamp_us = 0;       // Capture time in microseconds.
    uint8_t channel = 0;             // Radio-frequency (RF) channel associated with the frame.
    std::vector<uint8_t> payload;    // Raw payload bytes.
};

// FrameIO converts FrameRecord objects to and from a line-oriented text format.
class FrameIO {
public:
    // Produce a pipe-delimited line: DIRECTION|TIMESTAMP|CHANNEL|HEX_PAYLOAD.
    static std::string toLine(const FrameRecord& record);
    // Parse the line format produced by toLine.
    static bool fromLine(const std::string& line, FrameRecord& record);
    // Append one record to a log file in line-oriented form.
    static bool appendRecord(const char* path, const FrameRecord& record);

    // Hex helpers are public so tests and tools can reuse the same encoding.
    static std::string bytesToHex(const std::vector<uint8_t>& data);
    static bool hexToBytes(const std::string& hex, std::vector<uint8_t>& out);
};
