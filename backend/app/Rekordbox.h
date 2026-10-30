#pragma once

#include <juce_core/juce_core.h>

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace stemcel::native
{

struct RekordboxTrack
{
    juce::String id;
    juce::String title, artist, album, genre, label, key, comment, dateAdded;
    juce::String path, analysisPath, artworkPath, hotCues;
    double bpm = 0.0;
    double durationMs = 0.0;
    int rating = 0;
    int colour = 0;
    int playCount = 0;
    int sampleRate = 0;
    int bitrate = 0;
    int year = 0;
};

struct RekordboxPlaylist
{
    juce::String id, name, parentId;
    juce::String kind; // "playlist", "folder" or "smart"
    int seq = 0;
    juce::StringArray trackIds;
    bool isFolder() const { return kind == "folder"; }
};

struct RekordboxSnapshot
{
    bool ok = false;
    juce::String error;
    juce::String masterDb, shareRoot;
    std::vector<RekordboxTrack> tracks;
    std::vector<RekordboxPlaylist> playlists;

    const RekordboxTrack* findTrack(const juce::String& id) const;
    const RekordboxPlaylist* findPlaylist(const juce::String& id) const;
    // Children of `parentId` (empty = root) in rekordbox's order.
    std::vector<const RekordboxPlaylist*> childrenOf(const juce::String& parentId) const;

    void buildIndex();

  private:
    std::map<juce::String, size_t> trackIndex;
};

struct RekordboxCue
{
    double positionMs = 0.0;
    double endMs = -1.0; // loops only
    juce::String hot;    // "A".."P", empty for a memory cue
    bool memory = false;
    int colour = 0;
};

struct RekordboxBeat
{
    double timeMs = 0.0;
    int beatNumber = 1; // 1 = downbeat
    double bpm = 0.0;
};

struct RekordboxPhrase
{
    juce::String label;
    double startMs = -1.0;
    double endMs = -1.0;
};

struct RekordboxTrackDetail
{
    bool ok = false;
    juce::String error;
    std::vector<RekordboxCue> cues;
    std::vector<RekordboxBeat> beats;
    std::vector<RekordboxPhrase> phrases;
    juce::String mood;

    // The grid's tempo (its first beat's) and its first downbeat, when there is a grid.
    std::optional<double> gridBpm() const;
    std::optional<double> firstDownbeatSec() const;
};

// rekordbox library access through the `stemcel_rekordbox` shared library (Rust, over
// rbxport), loaded from beside the executable. Every call blocks and may take a while on
// a large library; call `open` and `trackDetail` off the message thread. Thread-safe.
class Rekordbox final
{
  public:
    Rekordbox();
    ~Rekordbox();

    bool isAvailable() const { return functions != nullptr; }
    juce::String unavailableReason() const { return loadError; }

    // Opens the installed library (or `masterDb` with `passphrase`, for tests) and reads
    // the whole snapshot. Replaces any library already open.
    RekordboxSnapshot open(const juce::File& masterDb = {}, const juce::String& passphrase = {});
    bool isOpen() const;
    RekordboxTrackDetail trackDetail(const juce::String& trackId);

    // Tags of any audio file via lofty (works without a library open). Void when the
    // reader is unavailable or the file has no readable tags.
    juce::var readTags(const juce::File& file);
    bool extractPicture(const juce::File& audio, const juce::File& out);
    // Decodes a format the engine can't read (m4a/aac/alac/ogg) to a float WAV.
    bool decodeToWav(const juce::File& source, const juce::File& wav);
    // Smoke-test support: builds a throwaway fixture library in `dir` whose first track is
    // `audio` (128 BPM grid, first downbeat at 120 ms). {ok, masterDb, passphrase, trackId}.
    juce::var buildFixture(const juce::File& dir, const juce::File& audio);

  private:
    struct Functions;
    juce::String callJson(const std::function<char*()>& call);

    juce::DynamicLibrary library;
    std::unique_ptr<Functions> functions;
    juce::String loadError;
    mutable std::mutex mutex;
    void* handle = nullptr;
};

// The app's one reader (the shared library is loaded once).
Rekordbox& sharedRekordbox();

} // namespace stemcel::native
