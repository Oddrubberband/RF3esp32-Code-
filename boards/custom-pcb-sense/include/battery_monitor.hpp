#pragma once

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <limits>

namespace BatteryMonitor {
struct Snapshot {
    int pack_mv = -1;
    int estimated_mv = -1;
    int percent = -1;
    int remaining_mah = -1;
    int age_ms = -1;
    const char* state = "unavailable";
    bool cutoff = false;
    bool persisted = false;
    int interval_mv = -1;
    int windows = 0;
};

struct SensePoint { int sense_uv; int pack_mv; };

// Bench supply against unconnected sensing output, supplied 2026-11-05.
// Units are microvolts at the sense pin and millivolts at the bench supply.
// The 24.25 V row has no sense reading and is omitted.
inline constexpr SensePoint kSenseCalibration[] = {
    {1664700, 15250}, {1691900, 15500}, {1719000, 15750}, {1740000, 16000},
    {1773600, 16250}, {1800000, 16500}, {1828200, 16750}, {1855330, 17000},
    {1882240, 17250}, {1909500, 17500}, {1937000, 17750}, {1964000, 18000},
    {1991000, 18250}, {2018000, 18500}, {2045700, 18750}, {2073000, 19000},
    {2104000, 19250}, {2127600, 19500}, {2154000, 19750}, {2182200, 20000},
    {2210000, 20250}, {2236000, 20500}, {2263000, 20750}, {2291100, 21000},
    {2318000, 21250}, {2345000, 21500}, {2372800, 21750}, {2399999, 22000},
    {2427000, 22250}, {2454000, 22500}, {2481600, 22750}, {2508000, 23000},
    {2536000, 23250}, {2563400, 23500}, {2590000, 23750}, {2618000, 24000},
    {2672000, 24500}, {2699000, 24750}, {2727000, 25000}, {2754000, 25250},
    {2781300, 25500},
};

inline int64_t packUvForSenseUv(int sense_uv)
{
    const size_t count = sizeof(kSenseCalibration) / sizeof(kSenseCalibration[0]);
    size_t upper = 1;
    while (upper < count - 1 && sense_uv > kSenseCalibration[upper].sense_uv) ++upper;
    const auto& a = kSenseCalibration[upper - 1];
    const auto& b = kSenseCalibration[upper];
    const int64_t denominator = b.sense_uv - a.sense_uv;
    const int64_t numerator = (static_cast<int64_t>(sense_uv) - a.sense_uv) *
        (b.pack_mv - a.pack_mv) * 1000;
    const int64_t offset = (numerator + (numerator >= 0 ? denominator / 2 : -denominator / 2)) /
        denominator;
    return std::max<int64_t>(0, static_cast<int64_t>(a.pack_mv) * 1000 + offset);
}

inline int percentForCellMv(int mv)
{
    struct Point { int mv; int percent; };
    // Generic conventional 4.20-V/cell LiPo reference, linearly interpolated
    // BETWEEN these nonlinear knots; replace with characterized Soikon data.
    constexpr Point curve[] = {{3300,0},{3600,5},{3690,10},{3730,20},
        {3770,30},{3800,40},{3840,50},{3870,60},{3950,70},{4020,80},
        {4110,90},{4200,100}};
    if (mv <= curve[0].mv) return 0;
    for (size_t i = 1; i < sizeof(curve)/sizeof(curve[0]); ++i) {
        if (mv <= curve[i].mv) {
            const auto& a = curve[i-1]; const auto& b = curve[i];
            return a.percent + ((mv-a.mv)*(b.percent-a.percent)+(b.mv-a.mv)/2)/(b.mv-a.mv);
        }
    }
    return 100;
}

} // namespace BatteryMonitor
