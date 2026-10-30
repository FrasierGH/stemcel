#pragma once

#include <juce_core/juce_core.h>

namespace stemcel
{

class AudioEngine;
class ProjectState;

// TEMPO_MAP_SET {points: [{timeMs, bpm}]}: replaces the project's tempo map (an empty
// list returns the project to its constant tempo). Undoable.
bool applyTempoMapSet(const juce::var& payload, ProjectState& projectState);

// Brings the engine in line with the project's tempo map: publishes the curve and marks
// which clips follow it (warped, ratio not pinned, source tempo known). Cheap and
// idempotent, so the dispatcher runs it after every command; that one pass covers every
// way the answer can change (a map edit, undo, a load, a warp toggle, a pin, a re-analysis).
void syncTempoMap(AudioEngine& engine, const ProjectState& projectState);

// MIX_TRANSITION_SET {id, outClipId, inClipId, preset, startMs, endMs} and
// MIX_TRANSITION_REMOVE {id}: the record of a DJ transition preset applied between two
// clips (its sound is automation and clip settings, sent alongside in one undo group).
bool applyMixTransitionSet(const juce::var& payload, ProjectState& projectState);
bool applyMixTransitionRemove(const juce::var& payload, ProjectState& projectState);

} // namespace stemcel
