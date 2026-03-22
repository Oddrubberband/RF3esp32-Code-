#pragma once

#include <cstdint>
#include <string>
#include <vector>

// KeyEvent models one contiguous key-up or key-down span for Morse timing.
struct KeyEvent {
    bool key_down = false;      // True while the transmitter should be keyed.
    uint32_t duration_ms = 0;   // Duration of that state.
};

// Morse turns American Standard Code for Information Interchange (ASCII) text
// into timed key-up/key-down events.
//
// The output is useful for any feature that wants to turn a text string into a
// sequence of transmit bursts with standard Morse spacing rules.
class Morse {
public:
    // Encode text using the supplied dot length in milliseconds.
    static std::vector<KeyEvent> encode(const std::string& text, uint32_t dot_ms);

private:
    // Look up the dit/dah pattern for one character.
    static std::string symbolFor(char c);
};
