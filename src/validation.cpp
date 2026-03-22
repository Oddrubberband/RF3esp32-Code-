#include "validation.hpp"

ValidationResult Validation::payloadSize(size_t len)
{
    // nRF24 fixed payloads top out at 32 bytes.
    if (len == 0) {
        return {false, "payload is empty"};
    }
    if (len > 32) {
        return {false, "payload exceeds 32 bytes"};
    }
    return {true, "ok"};
}

ValidationResult Validation::channel(uint8_t ch)
{
    // nRF24 channels occupy the 0..125 range.
    if (ch > 125) {
        return {false, "channel must be 0..125"};
    }
    return {true, "ok"};
}

ValidationResult Validation::dotTimeMs(uint32_t dot_ms)
{
    // Zero-length timing would collapse the whole Morse schedule.
    if (dot_ms == 0) {
        return {false, "dot time must be > 0"};
    }
    return {true, "ok"};
}

ValidationResult Validation::cwDurationMs(uint32_t duration_ms)
{
    // A continuous-wave (CW) test with no dwell time is effectively a no-op
    // and usually an input error.
    if (duration_ms == 0) {
        return {false, "CW duration must be > 0"};
    }
    return {true, "ok"};
}
