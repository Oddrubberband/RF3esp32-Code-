#pragma once

#include <cstddef>
#include <cstdint>

// ValidationResult is a tiny "success plus reason" structure used by the host
// tools and tests.
struct ValidationResult {
    bool ok;
    const char* message;
};

// Validation centralizes the simple range checks shared across the project.
class Validation {
public:
    // nRF24 payloads are limited to 32 bytes.
    static ValidationResult payloadSize(size_t len);
    // Radio-frequency (RF) channels must stay within the chip's 0..125 range.
    static ValidationResult channel(uint8_t ch);
    // Morse timing must be positive.
    static ValidationResult dotTimeMs(uint32_t dot_ms);
    // Continuous-wave tests need a non-zero dwell time.
    static ValidationResult cwDurationMs(uint32_t duration_ms);
};
