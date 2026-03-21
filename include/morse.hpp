#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct KeyEvent {
    bool key_down = false;
    uint32_t duration_ms = 0;
};

class Morse {
public:
    static std::vector<KeyEvent> encode(const std::string& text, uint32_t dot_ms);

private:
    static std::string symbolFor(char c);
};
