#pragma once

// The auto mixer (docs/native-app.md, "Auto mix"): lays a set of tracks out as a DJ mix and
// picks a transition preset for every join. Tracks can be put in harmonic order first (the
// Camelot wheel, then tempo, then energy); each is placed on alternating decks so it comes in
// over the previous one's outro, the overlap landing on 8-bar phrases; each join gets the
// preset that suits the pair (keys, tempo gap, energy change) and the chosen style.

#include "EngineClient.h"
#include "ProjectModel.h"
#include "WaveformStore.h"

#include <juce_core/juce_core.h>

#include <optional>
#include <vector>

namespace stemcel::native
{
class TimelineActions;

namespace automix
{
// A key on the Camelot wheel: 1..12 and A (minor) or B (major). Empty for an unknown key.
struct Camelot
{
    int number = 0;
    bool major = false;
    bool valid() const { return number >= 1 && number <= 12; }
    juce::String text() const { return valid() ? juce::String(number) + (major ? "B" : "A") : juce::String("?"); }
};
Camelot camelot(const juce::String& key);
// 0 same key, 1 a step round the wheel or the relative major/minor (mixes in key), 2+ clashes.
int keyDistance(Camelot a, Camelot b);
// "A minor" moved by `semitones` ("B minor" for +2); empty for an unknown key.
juce::String transposeKey(const juce::String& key, int semitones);
// The key a clip sounds in: its source's key moved by its pitch shift.
juce::String clipKey(const ProjectModel& model, const ClipModel& clip);
// The smallest shift (0, then +-1, +-2, ...) up to `maxShift` semitones that puts `moving` in key
// with `target`; nullopt when none does or a key is unknown.
std::optional<int> keyMatchShift(const juce::String& moving, const juce::String& target, int maxShift);

// Overall level of a track from its waveform peaks, in dB (nullopt until they are loaded).
std::optional<double> energyDb(WaveformStore& waveforms, const LibraryItemModel& item);

// How the auto mixer leans: Smooth prefers long blends, Club EQ and filter moves, Party cuts
// and FX; Auto weighs them evenly.
juce::StringArray styles();

struct Choice
{
    juce::String presetId;
    juce::String why;
};
// The preset that suits joining `out` into `in`. `previous` (the last join's preset) is
// avoided, so a mix does not repeat one move all the way through.
// `outShift` / `inShift` are the clips' pitch shifts in semitones (their keys move with them).
Choice choose(const ProjectModel& model, WaveformStore& waveforms, const LibraryItemModel* out, const LibraryItemModel* in,
              const juce::String& previous, const juce::String& style, int outShift = 0, int inShift = 0);

// Orders `itemIds` harmonically: from the first, always on to the closest remaining track.
std::vector<juce::String> harmonicOrder(const ProjectModel& model, WaveformStore& waveforms, std::vector<juce::String> itemIds);

// Builds the mix after the end of the arrangement on two new decks. Returns a summary line
// (or the reason it could not).
// With `matchKeys`, an incoming track whose key clashes is pitch-shifted (up to 2 semitones,
// tempo untouched) into key with the outgoing one when that fixes it.
juce::String build(EngineClient& engine, ProjectMirror& mirror, TimelineActions& actions, WaveformStore& waveforms,
                   std::vector<juce::String> itemIds, bool reorder, const juce::String& style, bool matchKeys = false);

// Gives every overlapping pair among `clipIds` (all clips when empty) the preset that suits it.
// Returns how many joins it set.
int autoTransition(EngineClient& engine, ProjectMirror& mirror, TimelineActions& actions, WaveformStore& waveforms,
                   const std::vector<juce::String>& clipIds, const juce::String& style);
} // namespace automix
} // namespace stemcel::native
