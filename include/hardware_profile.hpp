#pragma once

#include <cstdint>

namespace HardwareProfile {

enum class Id : uint8_t {
    CustomPcb = 1,
    Esp32Devboard = 2,
};

struct Pins {
    int ce;
    int csn;
    int irq;
    int sck;
    int mosi;
    int miso;
};

constexpr Pins pinsFor(Id id)
{
    return id == Id::CustomPcb ? Pins{17, 5, 27, 18, 23, 19}
                              : Pins{27, 5, 26, 18, 23, 19};
}

constexpr const char* nameFor(Id id)
{
    return id == Id::CustomPcb ? "custom-pcb" : "esp32-devboard";
}

#if defined(RF3_HARDWARE_PROFILE_ID)
static_assert(RF3_HARDWARE_PROFILE_ID == 1 || RF3_HARDWARE_PROFILE_ID == 2,
              "RF3_HARDWARE_PROFILE_ID must select a supported profile");

#if !defined(NRF24_CE_PIN) || !defined(NRF24_CSN_PIN) || \
    !defined(NRF24_IRQ_PIN) || !defined(NRF24_SCK_PIN) || \
    !defined(NRF24_MOSI_PIN) || !defined(NRF24_MISO_PIN)
#error "The selected RF3 hardware profile must define every nRF24 pin"
#endif

inline constexpr Id kSelectedId = static_cast<Id>(RF3_HARDWARE_PROFILE_ID);
inline constexpr Pins kSelectedPins = pinsFor(kSelectedId);
inline constexpr const char* kSelectedName = nameFor(kSelectedId);
static_assert(NRF24_CE_PIN == kSelectedPins.ce &&
              NRF24_CSN_PIN == kSelectedPins.csn &&
              NRF24_IRQ_PIN == kSelectedPins.irq &&
              NRF24_SCK_PIN == kSelectedPins.sck &&
              NRF24_MOSI_PIN == kSelectedPins.mosi &&
              NRF24_MISO_PIN == kSelectedPins.miso,
              "nRF24 pin macros do not match the selected RF3 hardware profile");
#endif

}  // namespace HardwareProfile
