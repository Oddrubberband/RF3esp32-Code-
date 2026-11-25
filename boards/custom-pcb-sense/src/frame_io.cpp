#include "frame_io.hpp"

#include <cstdio>
#include <charconv>
#include <string_view>
#include <utility>

std::string FrameIO::toLine(const FrameRecord& record)
{
    // Keep the textual format simple and grep-friendly so recorded frames can
    // be inspected by hand or parsed by tests without extra dependencies.
    const std::string direction = record.is_tx ? "TX" : "RX";
    const std::string payload_hex = bytesToHex(record.payload);

    return direction + "|" +
           std::to_string(record.timestamp_us) + "|" +
           std::to_string(record.channel) + "|" +
           payload_hex;
}

bool FrameIO::fromLine(const std::string& line, FrameRecord& record)
{
    std::string_view fields(line);
    if (!fields.empty() && fields.back() == '\n') fields.remove_suffix(1);
    if (!fields.empty() && fields.back() == '\r') fields.remove_suffix(1);
    const auto field = [&](std::string_view& value) {
        const auto end = fields.find('|');
        if (end == std::string_view::npos) return false;
        value = fields.substr(0, end);
        fields.remove_prefix(end + 1);
        return true;
    };
    const auto decimal = [](std::string_view value, auto& output) {
        if (value.empty()) return false;
        const char* end = value.data() + value.size();
        const auto result = std::from_chars(value.data(), end, output);
        return result.ec == std::errc{} && result.ptr == end;
    };
    FrameRecord parsed;
    std::string_view direction, timestamp, channel;
    if (!field(direction) || !field(timestamp) || !field(channel) ||
        (direction != "TX" && direction != "RX") ||
        !decimal(timestamp, parsed.timestamp_us) || !decimal(channel, parsed.channel) ||
        !hexToBytes(std::string(fields), parsed.payload)) return false;
    parsed.is_tx = direction == "TX";
    record = std::move(parsed);
    return true;
}

bool FrameIO::appendRecord(const char* path, const FrameRecord& record)
{
    // Persisting frames as newline-delimited records keeps appends cheap and
    // avoids having to maintain a more complex binary log format.
    if (!path || !*path) return false;
    const std::string line = toLine(record);
    if (line.empty()) {
        return false;
    }

    std::FILE* fp = std::fopen(path, "a");
    if (!fp) {
        return false;
    }

    const int written = std::fprintf(fp, "%s\n", line.c_str());
    const int closed = std::fclose(fp);
    return written > 0 && closed == 0;
}

std::string FrameIO::bytesToHex(const std::vector<uint8_t>& data)
{
    static const char* hex_digits = "0123456789ABCDEF";

    std::string hex;
    hex.reserve(data.size() * 2);

    // Each byte becomes two American Standard Code for Information Interchange
    // (ASCII) hex digits so the payload can safely travel through plain-text
    // logs and files.
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

    // Accept either upper- or lower-case hex so logs stay easy to edit or
    // generate from multiple tools.
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
