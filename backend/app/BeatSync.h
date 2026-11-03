#pragma once

#include "ProjectModel.h"

#include <juce_core/juce_core.h>

#include <optional>
#include <vector>

// Beat sync while dragging a clip: where its beats fall on the timeline, and the placements
// that line them up with the clip it overlaps on another track (or with the project grid).
namespace stemcel::native::beatsync
{
// A clip's beats on the timeline (ms) as if it started at `offsetMs`: its item's detected
// beats (else its beat anchor and BPM), through its tempo and any tempo following, within its
// window. `downbeats` gets the index into the result of every bar's first beat.
std::vector<double> clipBeatsMs(const ProjectModel& model, const ClipModel& clip, double offsetMs,
                                std::vector<size_t>* downbeats = nullptr);

// Where the clip's sections of these kinds begin, on the timeline for the clip at `offsetMs`,
// in order (empty when its item has no sections).
std::vector<double> sectionStartsMs(const ProjectModel& model, const ClipModel& clip, double offsetMs, const juce::StringArray& kinds);

// The clip on another track the dragged clip (at `startMs`, on `trackId`) overlaps most.
const ClipModel* partnerFor(const ProjectModel& model, const ClipModel& dragged, double startMs, const juce::String& trackId);

struct Placement
{
    juce::String label;
    double startMs = 0.0;
};

// The smallest move that puts the dragged clip's beats on the partner's (the median offset
// over the overlapping beats, so one off transient doesn't win), else on the project's beats.
double nearestBeat(const ProjectModel& model, const ClipModel& dragged, double startMs, const juce::String& trackId);

// Up to six placements, nearest first where it matters: nearest beat, bar and phrase; its
// first drop on the partner's next drop; its first drop where the partner's breakdown or
// outro begins; its intro where the partner's outro begins. Each lands on the beat.
std::vector<Placement> placements(const ProjectModel& model, const ClipModel& dragged, double startMs, const juce::String& trackId);
} // namespace stemcel::native::beatsync
