#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct FrameRecord {
    bool is_tx = false;
    uint64_t timestamp_us = 0;
    uint8_t channel = 0;
    std::vector<uint8_t> payload;
};

class FrameIO {
public:
    static std::string toLine(const FrameRecord& record);
    static bool fromLine(const std::string& line, FrameRecord& record);
    static bool appendRecord(const char* path, const FrameRecord& record);

    static std::string bytesToHex(const std::vector<uint8_t>& data);
    static bool hexToBytes(const std::string& hex, std::vector<uint8_t>& out);
};
