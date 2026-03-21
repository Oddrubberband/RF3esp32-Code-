#pragma once

#include <cstddef>
#include <cstdint>

struct ValidationResult {
    bool ok;
    const char* message;
};

class Validation {
public:
    static ValidationResult payloadSize(size_t len);
    static ValidationResult channel(uint8_t ch);
    static ValidationResult dotTimeMs(uint32_t dot_ms);
    static ValidationResult cwDurationMs(uint32_t duration_ms);
};
