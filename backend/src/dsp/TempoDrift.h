#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <functional>
#include <vector>

namespace stemcel
{

// The BPM variability check: how a recording's tempo moves over its whole length (a live
// drummer pushing the chorus, a DJ edit that drifts, a track nudged by hand). The detector's
// one tempo describes the track; this follows it bar by bar, so a drifting recording can be
// warped back to the grid or the project's tempo can follow it.
struct TempoDriftResult
{
    bool ok = false;
    // The tempo most of the track sits at (the median bar).
    double mainBpm = 0.0;
    // How far the tempo spreads (5th to 95th percentile of the bars), as % of the main.
    double driftPercent = 0.0;
    // One point per bar, at the bar's middle: {seconds into the file, bpm}.
    struct Point
    {
        double sec = 0.0;
        double bpm = 0.0;
    };
    std::vector<Point> curve;
    // Where the tempo leaves the main by more than kSectionThreshold for 4 bars or more.
    struct Section
    {
        double startSec = 0.0, endSec = 0.0, bpm = 0.0;
    };
    std::vector<Section> sections;
    std::vector<double> beatsSec;

    static constexpr double kSectionThreshold = 0.015;
};

// Follows the beats through the whole recording near `bpmHint` (the detected tempo: drift is
// found around it, never at half or double it) by dynamic programming over the onset strength,
// then reads each bar's tempo from them. Worker threads only.
TempoDriftResult analyseTempoDrift(const juce::File& file, juce::AudioFormatManager& formatManager, double bpmHint,
                                   const std::function<bool()>& shouldAbort = {});
// The same on mono samples already at `sampleRate`.
TempoDriftResult analyseTempoDriftSamples(const std::vector<float>& mono, double sampleRate, double bpmHint);

} // namespace stemcel
