#include "morse.hpp"

#include <cctype>

std::vector<KeyEvent> Morse::encode(const std::string& text, uint32_t dot_ms)
{
    (void)text;
    (void)dot_ms;

    // TODO:
    // Convert text into key-down / key-up timing events
    return {};
}

std::string Morse::symbolFor(char c)
{
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    // TODO:
    // Return ".", "-", etc. per character
    switch (c) {
        default:
            return "";
    }
}