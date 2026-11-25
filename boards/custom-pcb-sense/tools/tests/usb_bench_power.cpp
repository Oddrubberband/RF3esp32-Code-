#include <cassert>
#include "devboard_power.hpp"

// Bench mode must never access the ADC, persisted cutoff, or battery switch
// via the automatic controller. app_main separately configures GPIO15 LOW.
struct NoBatteryIo final : DevboardPower::Io {
    bool output(int, bool) override { assert(false); return false; }
    bool beginAdc(int) override { assert(false); return false; }
    DevboardPower::Reading readAdc() override { assert(false); return {}; }
    bool loadCutoff(bool&) override { assert(false); return false; }
    bool storeCutoff(bool) override { assert(false); return false; }
};

int main() {
    static_assert(HardwareProfile::kSelectedId == HardwareProfile::Id::CustomPcb);
    static_assert(HardwareProfile::kSelectedBatteryPower == HardwareProfile::BatteryPower::None);
    static_assert(DevboardPower::kPowerGoodGpio == 15);
    NoBatteryIo io;
    DevboardPower::Controller power(io, HardwareProfile::kSelectedBatteryPower);
    assert(power.initialize());
    assert(!power.supported() && !power.hasNrfRailControl() && !power.powerGood());
    assert(power.allowRf(0) && power.radioAvailable());
    assert(!power.poll(20000, true));
}
