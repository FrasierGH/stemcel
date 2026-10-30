#pragma once

#include "EngineClient.h"
#include "ProjectModel.h"
#include "Rekordbox.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace stemcel::native
{

// What an importer knows about a file beyond its audio: rekordbox's own analysis.
struct ImportHints
{
    juce::String rekordboxId; // fetch beat grid + key from rekordbox for this track
    juce::String title, artist, key;
    double bpm = 0.0;
};

struct ImportRequest
{
    juce::File file;
    ImportHints hints;
};

// The native import pipeline, matching the renderer's `importAudioIntoLibrary`: probe the
// file (duration, rate, channels), read its tags, detect its key, make a WAV the engine
// can play when the format is one it can't read (m4a/aac/ogg), save tags + cover into the
// project media store, then LIBRARY_ADD — all files of one request as one undo step. A
// rekordbox track also gets its BPM and first downbeat as a manual tempo, so the engine's
// own detection (which a manual tempo supersedes) never fights rekordbox's grid.
class AudioImporter final
{
  public:
    using Done = std::function<void(const juce::StringArray& itemIds)>;

    AudioImporter(EngineClient& engine, ProjectMirror& mirror, Rekordbox& rekordbox);
    ~AudioImporter();

    void import(std::vector<ImportRequest> requests, Done done = {});
    void import(const juce::Array<juce::File>& files, Done done = {});

    int pendingCount() const { return pending; }
    std::function<void(const juce::String& message, bool isError)> onNotify;
    // Preferences > Audio, for a project that has not chosen its own rate.
    int defaultProjectSampleRate = 44100;
    // Pending count changed (for a progress indicator).
    std::function<void()> onProgress;

    static bool isAudioFile(const juce::File& file);
    static juce::String audioFileWildcard();

    // Shared with the Files browser's audition: a path the engine can play (the file
    // itself, or a cached WAV transcode), or empty when the file can't be decoded.
    juce::String playablePath(const juce::File& file);

  private:
    struct Prepared;
    Prepared prepare(const ImportRequest& request);
    // Asks first when the files' sample rate differs from the project's (keep the project
    // rate, switch the project to theirs, or cancel), then commits.
    void confirmRateThenCommit(std::vector<Prepared> prepared, Done done);
    void commit(std::vector<Prepared> prepared, const Done& done);
    juce::String nextItemId() const;

    EngineClient& engine;
    ProjectMirror& mirror;
    Rekordbox& rekordbox;
    juce::ThreadPool pool{2};
    int pending = 0;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
};

} // namespace stemcel::native
