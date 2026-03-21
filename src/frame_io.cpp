#include "frame_io.hpp"

#include <cstdio>
#include <sstream>

std::string FrameIO::toLine(const FrameRecord& record)
{
    const std::string direction = record.is_tx ? "TX" : "RX";
    const std::string payload_hex = bytesToHex(record.payload);

    return direction + "|" +
           std::to_string(record.timestamp_us) + "|" +
           std::to_string(record.channel) + "|" +
           payload_hex;
}

bool FrameIO::fromLine(const std::string& line, FrameRecord& record)
{
    std::stringstream ss(line);
    std::string direction;
    std::string timestamp_str;
    std::string channel_str;
    std::string payload_hex;

    if (!std::getline(ss, direction, '|')) {
        return false;
    }
    if (!std::getline(ss, timestamp_str, '|')) {
        return false;
    }
    if (!std::getline(ss, channel_str, '|')) {
        return false;
    }
    if (!std::getline(ss, payload_hex, '|')) {
        return false;
    }

    record.is_tx = (direction == "TX");

    try {
        record.timestamp_us = static_cast<uint64_t>(std::stoull(timestamp_str));
        record.channel = static_cast<uint8_t>(std::stoul(channel_str));
    } catch (...) {
        return false;
    }

    return hexToBytes(payload_hex, record.payload);
}

bool FrameIO::appendRecord(const char* path, const FrameRecord& record)
{
    const std::string line = toLine(record);
    if (line.empty()) {
        return false;
    }

    std::FILE* fp = std::fopen(path, "a");
    if (!fp) {
        return false;
    }

    const int written = std::fprintf(fp, "%s\n", line.c_str());
    std::fclose(fp);

    return written > 0;
}

std::string FrameIO::bytesToHex(const std::vector<uint8_t>& data)
{
    static const char* hex_digits = "0123456789ABCDEF";

    std::string hex;
    hex.reserve(data.size() * 2);

    for (uint8_t byte : data) {
        hex.push_back(hex_digits[(byte >> 4) & 0x0F]);
        hex.push_back(hex_digits[byte & 0x0F]);
    }

    return hex;
}

bool FrameIO::hexToBytes(const std::string& hex, std::vector<uint8_t>& out)
{
    out.clear();

    if ((hex.size() % 2) != 0) {
        return false;
    }

    auto hexValue = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };

    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = hexValue(hex[i]);
        const int lo = hexValue(hex[i + 1]);

        if (hi < 0 || lo < 0) {
            return false;
        }

        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }

    return true;
}
