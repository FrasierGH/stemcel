#pragma once

#include "ProjectModel.h"

#include <juce_core/juce_core.h>

#include <vector>

namespace stemcel::native
{

// Track automation (lib/automation/automationParams.ts): per-parameter native ranges and
// formatting, the lanes a track shows, and its breakpoint curves.
namespace automation
{
struct Param
{
    const char* id;
    const char* label;
    double min, max, defaultValue;
};

struct Point
{
    double timeMs = 0.0;
    double value = 0.0;
    // The shape of the segment from this point to the next (AutomationShape's names:
    // linear, hold, smooth, fastStart, slowStart, stairs) and its bend, -1..1.
    juce::String curve = "linear";
    double tension = 0.0;
};

// The segment shapes, as (id, label), for the curve menu.
const std::vector<std::pair<const char*, const char*>>& curveShapes();

// Patterns to write over a stretch of a lane, the presets DAWs ship as automation shapes
// and LFOs: ramps, fades, swells, sine and triangle waves, saws, trance gates, a sidechain
// pump, stairs and random steps, tied to the beat.
struct Pattern
{
    const char* id;
    const char* label;
};
const std::vector<Pattern>& patterns();
// `existing` with the pattern written over [startMs, endMs] (the curve either side kept and
// handed back to), for a parameter, with `beatMs` the length of a beat there.
std::vector<Point> applyPattern(const std::vector<Point>& existing, const Param& param, const juce::String& patternId,
                                double startMs, double endMs, double beatMs);

struct Lane
{
    juce::String paramId;
    int heightPx = 80;
};

constexpr int kDefaultLaneHeight = 80;
constexpr int kMinLaneHeight = 40;
constexpr int kMaxLaneHeight = 220;

// Every automatable track parameter, in the renderer's picker order.
const std::vector<Param>& params();
// The master bus's own: its level, and every knob of Project FX (reverb, echo, glue).
const std::vector<Param>& masterParams();
// A plugin parameter's lane id, "plugin:<slotId>:<index>", and the slot and index in one.
juce::String pluginParamId(const juce::String& slotId, int index);
bool parsePluginParamId(const juce::String& paramId, juce::String& slotId, int& index);
// Names a plugin parameter's lane (its plugin and parameter), for find() and the captions.
void namePluginParam(const juce::String& paramId, const juce::String& label);
const Param* find(const juce::String& paramId);
juce::String format(const juce::String& paramId, double value);
double toFraction(const Param& p, double value);
double fromFraction(const Param& p, double fraction);

std::vector<Lane> lanesOf(const TrackModel& track);
std::vector<Point> pointsOf(const TrackModel& track, const juce::String& paramId);
// The curve's value at `timeMs` (each segment along its shape, held at the ends; default when empty).
double valueAt(const std::vector<Point>& points, double timeMs, double fallback);

juce::var toVar(const std::vector<Point>& points);
juce::var toVar(const std::vector<Lane>& lanes);
} // namespace automation

} // namespace stemcel::native
