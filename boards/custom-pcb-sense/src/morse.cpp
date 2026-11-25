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

std::string_view symbolFor(char c)
{
    // Lookup uses compact indexed tables for letters and digits.
    const unsigned char upper = static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(c)));
    if (upper >= 'A' && upper <= 'Z') {
        return kLetterCodes[upper - 'A'];
    }

    if (upper >= '0' && upper <= '9') {
        return kDigitCodes[upper - '0'];
    }

    return "";
}

template<typename Emit>
void forEachSymbol(std::string_view text, Emit emit)
{
    bool started = false;
    bool word_gap = false;
    for (char c : text) {
        if (c == ' ') {
            word_gap = started;
            continue;
        }
        const auto symbol = symbolFor(c);
        if (symbol.empty()) continue;
        emit(symbol, started ? (word_gap ? 7 : 3) : 0);
        started = true;
        word_gap = false;
    }
}
}  // namespace

std::vector<KeyEvent> Morse::encode(const std::string& text, uint32_t dot_ms)
{
    std::vector<KeyEvent> events;
    if (dot_ms == 0) return events;
    forEachSymbol(text, [&](std::string_view symbol, unsigned gap) {
        if (gap) events.push_back({false, dot_ms * gap});
        for (size_t i = 0; i < symbol.size(); ++i) {
            if (i) events.push_back({false, dot_ms});
            events.push_back({true, symbol[i] == '-' ? 3 * dot_ms : dot_ms});
        }
    });
    return events;
}

std::string Morse::render(const std::string& text)
{
    std::string out;
    forEachSymbol(text, [&](std::string_view symbol, unsigned gap) {
        if (gap) out += gap == 7 ? " / " : " ";
        out += symbol;
    });
    return out;
}
