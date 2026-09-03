#pragma once

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "radio_channel.hpp"

namespace CommandParsing {

inline std::string trimAscii(std::string value)
{
    const auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    while (!value.empty() && is_space(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && is_space(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

inline std::string uppercaseCopy(std::string_view text)
{
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return out;
}

inline std::vector<std::string> splitWords(const std::string& line)
{
    std::vector<std::string> words;
    std::string current;

    for (char ch : line) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!current.empty()) {
                words.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }

    if (!current.empty()) {
        words.push_back(current);
    }
    return words;
}

inline bool parseUint32Arg(std::string_view text,
                           uint32_t minimum,
                           uint32_t maximum,
                           uint32_t& out)
{
    if (text.empty() || minimum > maximum || text.front() == '-' || text.front() == '+') {
        return false;
    }

    const std::string value(text);
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (errno == ERANGE || !end || end == value.c_str() || *end != '\0' ||
        parsed > std::numeric_limits<uint32_t>::max() ||
        parsed < minimum || parsed > maximum) {
        return false;
    }

    out = static_cast<uint32_t>(parsed);
    return true;
}

inline bool parseUint8Arg(std::string_view text,
                          uint8_t minimum,
                          uint8_t maximum,
                          uint8_t& out)
{
    uint32_t parsed = 0;
    if (!parseUint32Arg(text, minimum, maximum, parsed)) {
        return false;
    }
    out = static_cast<uint8_t>(parsed);
    return true;
}

inline bool parseLoopCountToken(std::string_view text, bool& infinite, uint32_t& count)
{
    const std::string upper = uppercaseCopy(text);
    if (upper == "INF" || upper == "FOREVER") {
        infinite = true;
        count = 0;
        return true;
    }

    uint32_t parsed = 0;
    if (!parseUint32Arg(text, 1, UINT32_MAX, parsed)) {
        return false;
    }

    infinite = false;
    count = parsed;
    return true;
}

enum class ChannelAction { Select, Preview };

struct ChannelRequest {
    ChannelAction action = ChannelAction::Select;
    uint8_t channel = 0;
};

// Parse the entire request before allowing any radio or transfer side effects.
inline bool parseChannelCommand(const std::vector<std::string>& words, ChannelRequest& out)
{
    if (words.empty() || uppercaseCopy(words[0]) != "CHANNEL") {
        return false;
    }

    ChannelRequest request{};
    if (words.size() == 3 && uppercaseCopy(words[1]) == "PREVIEW") {
        request.action = ChannelAction::Preview;
    } else if (words.size() != 2) {
        return false;
    }

    if (!parseUint8Arg(words.back(), 0, RadioChannel::kMaximum, request.channel)) {
        return false;
    }
    out = request;
    return true;
}

}  // namespace CommandParsing
