#pragma once

#include "ProjectModel.h"

#include <juce_core/juce_core.h>

#include <vector>

namespace stemcel::native
{

// The tracklist written beside a mixdown: one line per source track, in the order the mix
// reaches it (`mm:ss  Artist - Title`). Artist and title come from the file's tags (or what
// rekordbox said at import, which the import stores the same way), and otherwise from the
// file name, cleaned of track numbers, video and quality noise, and split on its dash.
namespace tracklist
{

struct Guess
{
    juce::String artist; // empty when the name doesn't say
    juce::String title;
};

// A file name (with or without its extension) read as `Artist - Title`. With no separator the
// cleaned name is the title and the artist stays empty rather than guessed.
Guess fromFileName(const juce::String& fileName);

struct Entry
{
    double atMs = 0.0; // in the exported file
    juce::String artist, title;

    juce::String line() const;
};

// The tracks heard between project times `fromMs` and `toMs`, first appearance first; a
// source's stems count as the source. `leadInMs` is the silence before the export's first
// sample, so the times match the file.
std::vector<Entry> fromProject(const ProjectModel& project, double fromMs, double toMs, double leadInMs);

juce::String toText(const std::vector<Entry>& entries);

} // namespace tracklist
} // namespace stemcel::native
