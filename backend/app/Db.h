#pragma once

#include <juce_core/juce_core.h>

#include <cmath>
#include <limits>

namespace stemcel::native::db
{

// The renderer's fader taper (lib/audio/db.ts), so a fader position means the same level in
// both apps.
inline constexpr double kMinDisplayDb = -60.0;
inline constexpr double kMinInternalDb = -120.0;
inline constexpr double kMaxTrackDb = 6.0;
inline constexpr double kMaxMasterDb = 0.0;
inline constexpr double kTaperInfinityFraction = 0.02;
inline constexpr double kTaperExponent = 0.4;

inline double linearToDb(double linear)
{
    if (!std::isfinite(linear) || linear <= 0.0) return -std::numeric_limits<double>::infinity();
    return juce::jmax(kMinInternalDb, 20.0 * std::log10(linear));
}

inline double dbToLinear(double decibels)
{
    if (!std::isfinite(decibels) || decibels <= kMinInternalDb) return 0.0;
    return std::pow(10.0, decibels / 20.0);
}

inline double positionToDb(double pos, double maxDb)
{
    if (!std::isfinite(pos) || pos <= 0.0) return -std::numeric_limits<double>::infinity();
    if (pos <= kTaperInfinityFraction) return kMinDisplayDb;
    const double t = (juce::jmin(pos, 1.0) - kTaperInfinityFraction) / (1.0 - kTaperInfinityFraction);
    return kMinDisplayDb + std::pow(t, kTaperExponent) * (maxDb - kMinDisplayDb);
}

inline double dbToPosition(double decibels, double maxDb)
{
    if (!std::isfinite(decibels)) return 0.0;
    if (decibels <= kMinDisplayDb) return kTaperInfinityFraction;
    if (decibels >= maxDb) return 1.0;
    const double tapered = (decibels - kMinDisplayDb) / (maxDb - kMinDisplayDb);
    return kTaperInfinityFraction + std::pow(tapered, 1.0 / kTaperExponent) * (1.0 - kTaperInfinityFraction);
}

inline double linearToPosition(double linear, double maxDb)
{
    return dbToPosition(linearToDb(linear), maxDb);
}

inline double positionToLinear(double pos, double maxDb)
{
    return dbToLinear(positionToDb(pos, maxDb));
}

inline juce::String format(double decibels)
{
    if (!std::isfinite(decibels)) return juce::CharPointer_UTF8("-\xe2\x88\x9e dB");
    const double clamped = juce::jmax(kMinDisplayDb, decibels);
    return (clamped >= 0.0 ? "+" : "") + juce::String(clamped, 1) + " dB";
}

} // namespace stemcel::native::db
