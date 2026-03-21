#include "morse.hpp"

#include <cctype>

std::vector<KeyEvent> Morse::encode(const std::string& text, uint32_t dot_ms)
{
    std::vector<KeyEvent> events;

    if (dot_ms == 0) {
        return events;
    }

    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];

        if (c == ' ') {
            events.push_back({false, dot_ms * 7});
            continue;
        }

        const std::string symbols = symbolFor(c);
        if (symbols.empty()) {
            continue;
        }

        for (size_t j = 0; j < symbols.size(); ++j) {
            const uint32_t down_time = (symbols[j] == '-') ? (dot_ms * 3) : dot_ms;
            events.push_back({true, down_time});

            if (j + 1 < symbols.size()) {
                events.push_back({false, dot_ms});
            }
        }

        if (i + 1 < text.size() && text[i + 1] != ' ') {
            events.push_back({false, dot_ms * 3});
        }
    }

    return events;
}

std::string Morse::symbolFor(char c)
{
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    switch (c) {
        case 'A': return ".-";
        case 'B': return "-...";
        case 'C': return "-.-.";
        case 'D': return "-..";
        case 'E': return ".";
        case 'F': return "..-.";
        case 'G': return "--.";
        case 'H': return "....";
        case 'I': return "..";
        case 'J': return ".---";
        case 'K': return "-.-";
        case 'L': return ".-..";
        case 'M': return "--";
        case 'N': return "-.";
        case 'O': return "---";
        case 'P': return ".--.";
        case 'Q': return "--.-";
        case 'R': return ".-.";
        case 'S': return "...";
        case 'T': return "-";
        case 'U': return "..-";
        case 'V': return "...-";
        case 'W': return ".--";
        case 'X': return "-..-";
        case 'Y': return "-.--";
        case 'Z': return "--..";
        case '0': return "-----";
        case '1': return ".----";
        case '2': return "..---";
        case '3': return "...--";
        case '4': return "....-";
        case '5': return ".....";
        case '6': return "-....";
        case '7': return "--...";
        case '8': return "---..";
        case '9': return "----.";
        default:
            return "";
    }
}
