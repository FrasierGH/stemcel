#pragma once

#include "ProjectModel.h"
#include "Rekordbox.h"

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>

#include <array>
#include <limits>
#include <optional>
#include <vector>

// A track's arrangement as sections (intro, build, drop, breakdown, outro...), from rekordbox's
// phrase analysis when the track came from rekordbox, else from Stemcel's own: each bar's low,
// mid and high band energy on the item's beat grid, grouped into 8-bar phrases, phrases with
// the same character joined, and each section named by its energy and its kick against the
// track's. Vocals are marked where the item's Vocals stem (Separate Stems) has a voice; without
// one they are not guessed. The sections live on the library item in the Stemcel project; Stemcel's
// own results are also cached per audio file under ~/Stemcel/Analysis. Nothing here writes to
// rekordbox.
namespace stemcel::native::sections
{
struct Section
{
    double startSec = 0.0, endSec = 0.0; // source time
    juce::String label;                  // "Drop", "Breakdown", "CHORUS 1"...
    juce::String kind;                   // intro, verse, chorus, bridge, build, drop, breakdown, outro, groove
    double energy = 0.0;                 // 0..1 against the track
    bool kick = false;                   // the low band (kick and bass) is driving
    bool vocal = false;
    bool vocalEstimated = false;         // from the mid band, not a Vocals stem
    juce::String source;                 // "rekordbox" or "stemcel"
};

// One bar's features, each 0..1 against the track once normalised (vocal NaN when unknown).
struct Bar
{
    double low = 0.0, mid = 0.0, high = 0.0;
    double vocal = std::numeric_limits<double>::quiet_NaN();
};

// Splits bars into sections and names them. `firstBarSec` is where bar 0 starts.
std::vector<Section> segment(const std::vector<Bar>& bars, double barSec, double firstBarSec, double durationSec,
                             bool vocalFromStem);

// rekordbox's phrases (when it analysed the track), with energy and kick from its 3-band
// waveform; with no phrases, the 3-band waveform segmented as Stemcel's own analysis would.
std::vector<Section> fromRekordbox(const RekordboxTrackDetail& detail, double bpm, double beatAnchorSec, double durationSec);

// The bar grid an item's analysis uses: its tempo and first beat, else 4-second blocks.
std::pair<double, double> barGrid(double bpm, double beatAnchorSec);

// Stemcel's own analysis of an audio file (and its Vocals stem, when given): decodes it and
// measures each bar's bands. Slow (a second or two a track): run it off the message thread.
std::optional<std::vector<Section>> analyseFile(const juce::File& audio, double bpm, double beatAnchorSec,
                                                const juce::File& vocalStem = {});

juce::var toVar(const std::vector<Section>& list);
std::vector<Section> fromVar(const juce::var& value);
std::vector<Section> of(const LibraryItemModel& item);

// What a section shows: its label, with " + vocal" (or " + vocal?" when estimated).
juce::String caption(const Section& s);
juce::Colour colourFor(const juce::String& kind);

// The cache for Stemcel's own results: one JSON file per audio file, keyed by its name, size,
// modification time and first 64 KB, under ~/Stemcel/Analysis.
juce::File cacheFileFor(const juce::File& audio, double bpm, double beatAnchorSec, bool withVocalStem);
} // namespace stemcel::native::sections
