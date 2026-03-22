#include "morse.hpp"

#include <array>
#include <cctype>
#include <string_view>

namespace {
constexpr std::array<std::string_view, 26> kLetterCodes = {
    ".-",   "-...", "-.-.", "-..",  ".",    "..-.", "--.",  "....", "..",
    ".---", "-.-",  ".-..", "--",   "-.",   "---",  ".--.", "--.-", ".-.",
    "...",  "-",    "..-",  "...-", ".--",  "-..-", "-.--", "--..",
};

constexpr std::array<std::string_view, 10> kDigitCodes = {
    "-----", ".----", "..---", "...--", "....-",
    ".....", "-....", "--...", "---..", "----.",
};

void appendGap(std::vector<KeyEvent>& events, uint32_t duration_ms)
{
    if (duration_ms == 0) {
        return;
    }

    if (!events.empty() && !events.back().key_down) {
        events.back().duration_ms += duration_ms;
        return;
    }

    events.push_back({false, duration_ms});
}

void appendMark(std::vector<KeyEvent>& events, uint32_t duration_ms)
{
    if (duration_ms == 0) {
        return;
    }

    events.push_back({true, duration_ms});
}
}  // namespace

std::vector<KeyEvent> Morse::encode(const std::string& text, uint32_t dot_ms)
{
    std::vector<KeyEvent> events;

    if (dot_ms == 0) {
        return events;
    }

    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];

        if (c == ' ') {
            // Standard Morse word gaps are seven dots wide.
            appendGap(events, dot_ms * 7);
            continue;
        }

        const std::string symbols = symbolFor(c);
        if (symbols.empty()) {
            // Unsupported characters are skipped rather than aborting the whole
            // message, which keeps the helper forgiving for user-interface (UI) input.
            continue;
        }

        for (size_t j = 0; j < symbols.size(); ++j) {
            // Dots are one unit, dashes are three units.
            const uint32_t down_time = symbols[j] == '-' ? (dot_ms * 3) : dot_ms;
            appendMark(events, down_time);

            if (j + 1 < symbols.size()) {
                // Symbols within one letter are separated by a one-dot gap.
                appendGap(events, dot_ms);
            }
        }

        if (i + 1 < text.size() && text[i + 1] != ' ') {
            // Distinct letters are separated by a three-dot gap.
            appendGap(events, dot_ms * 3);
        }
    }

    return events;
}

std::string Morse::symbolFor(char c)
{
    // Lookup uses compact indexed tables for letters and digits.
    const unsigned char upper = static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(c)));
    if (upper >= 'A' && upper <= 'Z') {
        return std::string(kLetterCodes[upper - 'A']);
    }

    if (upper >= '0' && upper <= '9') {
        return std::string(kDigitCodes[upper - '0']);
    }

    return "";
}
