#pragma once

#include "ProjectModel.h"
#include "TimelineActions.h"

#include <juce_core/juce_core.h>

#include <vector>

namespace stemcel::native::stutter
{

// Beat stutter: the beat-synced repeats of a slice of the track (the Beat Repeat engine,
// tempo-locked in playback and export), laid out as the classic moves: a straight stutter
// at one division, a build-up whose repeats get shorter (1/4, 1/8, 1/16, 1/32), a fill on a
// bar's last beat, and a gated stutter (repeats chopped by a 1/16 volume gate).
struct Preset
{
    const char* id;
    const char* label;
};
const std::vector<Preset>& presets();

// Applies a preset on a track from `startBeat` (for the preset's own length), or across
// [startBeat, endBeat) when the range is given, replacing beat repeats already there. One
// undo step.
void apply(TimelineActions& actions, const ProjectModel& model, const juce::String& trackId, const juce::String& presetId,
           double startBeat, double endBeat = -1.0);

} // namespace stemcel::native::stutter
