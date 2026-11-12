#pragma once

// How the performance mixer's knobs and faders turn into gain, as rekordbox's defaults do.
// Shared by the live mixer and the take recorder, so what is written to the timeline is
// exactly what was heard.

#include <algorithm>
#include <cmath>

namespace stemcel::perf::curves
{

constexpr double kSilentDb = -60.0;  // the timeline's level floor
constexpr double kEqKillDb = -40.0;  // ToneEq's kill

inline double gainToDb(double gain) noexcept
{
    return gain <= 1.0e-3 ? kSilentDb : std::max(kSilentDb, 20.0 * std::log10(gain));
}

// A knob that is 0 dB at its centre: the left half falls away along its travel (−6 dB at a
// quarter) to `floorDb` at the end, the right half rises evenly to `maxDb`.
inline double centreKnobDb(double v, double maxDb, double floorDb) noexcept
{
    v = std::clamp(v, 0.0, 1.0);
    if (v >= 0.5) return maxDb * (v - 0.5) / 0.5;
    if (v <= 1.0e-4) return floorDb;
    return std::max(floorDb, 20.0 * std::log10(v / 0.5));
}

inline double trimDb(double v) noexcept { return centreKnobDb(v, 12.0, kSilentDb); }   // −∞ .. +12 dB
inline double eqDb(double v) noexcept { return centreKnobDb(v, 6.0, kEqKillDb); }      // kill .. +6 dB

// The FILTER knob as ToneEq's bipolar sweep (−1 low-pass .. +1 high-pass), with a dead zone at
// the centre detent.
inline double filterAmount(double v) noexcept
{
    const double x = (std::clamp(v, 0.0, 1.0) - 0.5) * 2.0;
    constexpr double kDead = 0.03;
    if (std::abs(x) <= kDead) return 0.0;
    return (x - std::copysign(kDead, x)) / (1.0 - kDead);
}

// Channel fader: a squared law (−12 dB at half travel), silent at the bottom.
inline double faderGain(double v) noexcept
{
    v = std::clamp(v, 0.0, 1.0);
    return v * v;
}

enum class CrossfaderCurve
{
    full,   // both full at the centre, each fading out over the other half (the default)
    smooth, // constant power: −3 dB each at the centre
    cut     // both full until the last few millimetres (scratch)
};

// Crossfader gain for a channel on side A (side = 0) or B (side = 1); THRU (side < 0) is 1.
inline double crossfaderGain(double x, int side, CrossfaderCurve curve) noexcept
{
    if (side < 0) return 1.0;
    x = std::clamp(x, 0.0, 1.0);
    const double towardOther = side == 0 ? x : 1.0 - x; // 0 = fully on this side
    if (curve == CrossfaderCurve::full) return std::clamp(2.0 * (1.0 - towardOther), 0.0, 1.0);
    if (curve == CrossfaderCurve::cut)
    {
        constexpr double kCutLag = 0.04;
        if (towardOther >= 1.0 - 1.0e-6) return 0.0;
        if (towardOther <= 1.0 - kCutLag) return 1.0;
        return (1.0 - towardOther) / kCutLag;
    }
    return std::cos(towardOther * 1.5707963267948966);
}

} // namespace stemcel::perf::curves
