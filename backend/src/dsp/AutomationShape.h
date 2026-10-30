#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace stemcel
{

// The shape of an automation segment, from a point to the next one (the point's `curve`),
// the set DAWs share (Reaper's envelope shapes, FL Studio's curve modes): a straight line;
// hold until the next point (a step); smooth (an S-curve, slow at both ends); fast start
// (most of the move early, an exponential ease-out); slow start (most of it late, an
// ease-in); and stairs (four equal steps). `tension` (-1..1) bends fast/slow start further
// or back towards a line: 0 is the default bend.
enum class AutomationShape : std::uint8_t
{
    linear,
    hold,
    smooth,
    fastStart,
    slowStart,
    stairs
};

inline AutomationShape automationShapeFromString(const char* name) noexcept
{
    if (name == nullptr) return AutomationShape::linear;
    if (std::strcmp(name, "hold") == 0) return AutomationShape::hold;
    if (std::strcmp(name, "smooth") == 0) return AutomationShape::smooth;
    if (std::strcmp(name, "fastStart") == 0) return AutomationShape::fastStart;
    if (std::strcmp(name, "slowStart") == 0) return AutomationShape::slowStart;
    if (std::strcmp(name, "stairs") == 0) return AutomationShape::stairs;
    return AutomationShape::linear;
}

inline const char* automationShapeName(AutomationShape shape) noexcept
{
    switch (shape)
    {
        case AutomationShape::hold: return "hold";
        case AutomationShape::smooth: return "smooth";
        case AutomationShape::fastStart: return "fastStart";
        case AutomationShape::slowStart: return "slowStart";
        case AutomationShape::stairs: return "stairs";
        case AutomationShape::linear: break;
    }
    return "linear";
}

// How far along a segment's move the value is at `frac` (0..1) of its time.
inline double shapeFraction(AutomationShape shape, double tension, double frac) noexcept
{
    frac = std::clamp(frac, 0.0, 1.0);
    // The bend's exponent: 3 by default, from 1 (a line) to 8 with tension.
    const double power = std::clamp(3.0 + std::clamp(tension, -1.0, 1.0) * (tension < 0.0 ? 2.0 : 5.0), 1.0, 8.0);
    switch (shape)
    {
        case AutomationShape::hold: return 0.0;
        case AutomationShape::smooth: return 0.5 - 0.5 * std::cos(frac * 3.14159265358979323846);
        case AutomationShape::fastStart: return 1.0 - std::pow(1.0 - frac, power);
        case AutomationShape::slowStart: return std::pow(frac, power);
        case AutomationShape::stairs: return std::min(1.0, std::floor(frac * 4.0) / 3.0);
        case AutomationShape::linear: break;
    }
    return frac;
}

} // namespace stemcel
