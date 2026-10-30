#pragma once

#include <juce_core/juce_core.h>

namespace stemcel::native
{

// The project's central media store, shared with the Electron app: per imported source
// file, `metadata/<mediaId>.json` ({version: 1, metadata: {title, artist, …}}) and an
// optional `covers/<mediaId>.<ext>`, beside the project file — or in the temp workspace
// while the project is unsaved, carried into the project folder on Save As.
namespace media
{
// The folder holding `metadata/` and `covers/` for a project at `projectFile` (empty =
// unsaved).
juce::File rootFor(const juce::String& projectFile);

bool isSafeId(const juce::String& mediaId);

// Writes the sidecar (and the cover, when `cover` exists) for `mediaId`. `metadata` is a
// DynamicObject in the renderer's AudioMetadata shape.
bool save(const juce::File& root, const juce::String& mediaId, const juce::var& metadata, const juce::File& cover);

// The stored metadata (void when absent) and cover file (non-existent when absent).
juce::var metadata(const juce::File& root, const juce::String& mediaId);
juce::File cover(const juce::File& root, const juce::String& mediaId);

// Copies every sidecar and cover from one store to another (Save As / first save).
void migrate(const juce::File& from, const juce::File& to);
} // namespace media

} // namespace stemcel::native
