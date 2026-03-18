#include "frame_io.hpp"

#include <cstdio>

std::string FrameIO::toLine(const FrameRecord& record)
{
    (void)record;

    // TODO:
    // Turn one FrameRecord into one text line.
    // Example idea:
    // direction|timestamp|channel|payloadhex
    return {};
}

bool FrameIO::fromLine(const std::string& line, FrameRecord& record)
{
    (void)line;
    (void)record;

    // TODO:
    // Parse one text line back into a FrameRecord.
    return false;
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
    (void)data;

    // TODO:
    // Convert payload bytes into a hex string.
    return {};
}

bool FrameIO::hexToBytes(const std::string& hex, std::vector<uint8_t>& out)
{
    (void)hex;
    (void)out;

    // TODO:
    // Convert hex string back into bytes.
    return false;
}