#pragma once

#include "ProjectStateTypes.h"
#include "TempoCurve.h"

#include <memory>
#include <vector>

#include <juce_core/juce_core.h>

namespace stemcel
{

struct BeatRepeatRegionSamples
{
    juce::int64 startSample{0};
    juce::int64 endSample{0};
    int divisionSamples{0};
};

struct BeatRepeatSnapshot
{
    std::vector<BeatRepeatRegionSamples> regions;
};

// Regions are in beats: at `bpm`, or along `tempoCurve` when it has a tempo map (its beats,
// and each region's repeat size at the tempo where the region starts).
std::unique_ptr<BeatRepeatSnapshot> makeBeatRepeatSnapshot(
    const std::vector<BeatRepeatRegion>& regions, double sampleRate, double bpm,
    const TempoCurve* tempoCurve = nullptr);

} // namespace stemcel
