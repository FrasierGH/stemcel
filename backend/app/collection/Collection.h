#pragma once

// Stemcel's DJ collection: the music library a DJ keeps across projects, in place of
// rekordbox's. Tracks (a file with its tags, beat grid, key, cues, rating, colour and My
// Tags), playlists in folders, and play histories read back from USB sticks. It is shared
// by every project tab and lives in ~/Stemcel/Collection/collection.json.
//
// Tracks are analysed in the background as they come in (Stemcel's BPM, beat grid and key
// detection, and loudness for auto gain); a grid set by hand or brought from rekordbox is
// never replaced by analysis unless the track is analysed again on purpose.
//
// Message thread only, except where noted.

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace stemcel::native
{
class Rekordbox;
struct RekordboxSource;
}

namespace stemcel::native::dj
{

// A hot cue (slot 0–7, A–H) or memory cue (slot -1), in seconds; a loop when `end > pos`.
struct Cue
{
    int slot = -1;
    double pos = 0.0;
    double end = -1.0;
    juce::String name;
    int colour = 0; // rekordbox's hot cue colour index, 0 = the default
    bool isLoop() const { return end > pos; }
    bool isHot() const { return slot >= 0; }
};

struct Track
{
    juce::int64 id = 0;
    juce::String path;
    juce::String title, artist, album, genre, label, remixer, comment;
    int year = 0;
    double durationSec = 0.0;
    int sampleRate = 0;
    int bitrate = 0;
    juce::int64 fileSize = 0;

    // The beat grid: a steady tempo from `anchorSec`, or `beats` (seconds) when the tempo
    // moves. `firstBeat` is the bar position (1–4) of the beat at the anchor (or of the
    // first of `beats`); 0 when not known, and the export finds the bars itself.
    double bpm = 0.0;
    double anchorSec = 0.0;
    std::vector<double> beats;
    int firstBeat = 0;
    bool gridLocked = false; // set by hand or brought from rekordbox: analysis leaves it

    juce::String key; // Stemcel's spelling: "A minor"
    int rating = 0;   // 0–5 stars
    int colour = 0;   // rekordbox's colour labels: 0 none, 1 pink … 8 purple
    juce::StringArray tags;
    juce::String dateAdded; // yyyy-mm-dd
    int playCount = 0;
    std::vector<Cue> cues;

    bool hasLoudness = false;
    double loudnessLufs = 0.0;

    juce::String rekordboxId; // where it came from, when imported from rekordbox
    juce::String artwork;     // a cover image in the collection folder

    enum class State
    {
        pending,
        analysing,
        ready,
        failed,
    };
    State state = State::pending;
    juce::String error;

    juce::String displayTitle() const;
    // The time of every beat in the track, from the grid.
    std::vector<double> beatTimes() const;
    // Beat number 1–4 of the beat at `index` in beatTimes(), 0 when the bars aren't known.
    int beatNumberAt(int index) const;
    const Cue* hotCue(int slot) const;
};

struct Playlist
{
    enum class Kind
    {
        playlist,
        folder,
        history,
    };
    juce::int64 id = 0;
    juce::int64 parent = 0; // 0 = the top
    juce::String name;
    Kind kind = Kind::playlist;
    std::vector<juce::int64> tracks;
    juce::String date;   // a history's day
    juce::String source; // a history's stick
    bool isFolder() const { return kind == Kind::folder; }
};

class Collection final : public juce::ChangeBroadcaster, private juce::Timer
{
  public:
    static Collection& shared();
    ~Collection() override;

    // Where it lives; the smoke test points it at its own folder before first use.
    static juce::File folder();
    static void setFolderForTest(const juce::File& folder);
    // Reads the file once (later calls do nothing); `reload` reads it again.
    void load();
    void reload();
    // Writes now (otherwise a change is written a second after it is made).
    void saveNow();

    const std::vector<Track>& tracks() const { return trackList; }
    const std::vector<Playlist>& playlists() const { return playlistList; }
    const Track* find(juce::int64 id) const;
    const Track* findByPath(const juce::String& path) const;
    const Playlist* findPlaylist(juce::int64 id) const;
    std::vector<const Playlist*> childrenOf(juce::int64 parent, bool histories) const;
    // Every track a playlist or folder holds (a folder's, of all its playlists, once each).
    std::vector<juce::int64> tracksIn(juce::int64 playlistOrFolder) const;

    // Adds audio files (those already in it are found, not added twice) and returns their
    // ids, in order; each new one is analysed in the background. With `playlist`, they
    // are put in it too.
    std::vector<juce::int64> addFiles(const juce::Array<juce::File>& files, juce::int64 playlist = 0);
    // Every audio file under `folder`, found off the message thread.
    void addFolder(const juce::File& folder, juce::int64 playlist = 0, std::function<void(int added)> done = {});
    void removeTracks(const std::vector<juce::int64>& ids);
    void update(juce::int64 id, const std::function<void(Track&)>& change);
    void relocate(juce::int64 id, const juce::File& file);
    // Analyses again, grid and key included (a grid set by hand is replaced).
    void reanalyse(const std::vector<juce::int64>& ids);
    int analysisPending() const { return queued; }
    void setPaused(bool pause); // the smoke test and exports hold analysis back
    // A track's loudness, measured where it was already in memory (a deck loading it).
    void setLoudness(const juce::String& path, double lufs);

    juce::int64 createPlaylist(const juce::String& name, juce::int64 parent, Playlist::Kind kind = Playlist::Kind::playlist);
    void renamePlaylist(juce::int64 id, const juce::String& name);
    void deletePlaylist(juce::int64 id); // a folder with all it holds
    void movePlaylist(juce::int64 id, juce::int64 newParent);
    void addToPlaylist(juce::int64 playlist, const std::vector<juce::int64>& ids, int insertAt = -1);
    void removeFromPlaylist(juce::int64 playlist, const std::vector<int>& positions);
    void reorderPlaylist(juce::int64 playlist, const std::vector<int>& positions, int insertAt);
    // A play history read from a stick: a playlist of kind history under "Histories".
    juce::int64 addHistory(const juce::String& name, const juce::String& date, const juce::String& source,
                           const std::vector<juce::int64>& ids);

    // Brings this computer's rekordbox library in: every track with its beat grid, key,
    // cues, rating, colour and comment, and every playlist (in a "rekordbox" folder).
    // Tracks already in the collection (the same file) get rekordbox's grid and cues only
    // where they have none. Runs on a thread; `progress` (message thread) gets done/total,
    // `done` the outcome.
    void importRekordbox(Rekordbox& rekordbox, std::function<void(int done, int total)> progress,
                         std::function<void(const juce::String& summary, bool ok)> done);
    // From another library (a drive's, or the smoke test's fixture with its passphrase).
    void importRekordbox(Rekordbox& rekordbox, const RekordboxSource& source, const juce::String& passphrase,
                         std::function<void(int done, int total)> progress,
                         std::function<void(const juce::String& summary, bool ok)> done);

    // The request the rekordbox module's export takes (`rbx_export_json`): these
    // playlists (folders with what they hold) and their tracks, with grids, cues and tags.
    juce::var exportRequest(const std::vector<juce::int64>& playlists, const juce::File& stick, bool sync) const;

    // Smoke-test support: true once nothing is waiting for analysis.
    bool idle() const { return queued == 0; }

  private:
    Collection();
    void timerCallback() override;
    void changed(bool save = true);
    void queueAnalysis(juce::int64 id, bool force);
    juce::int64 nextId() { return ++lastId; }
    void read(const juce::File& file);
    juce::var toJson() const;
    void reindex();
    Track* findMutable(juce::int64 id);
    Playlist* findPlaylistMutable(juce::int64 id);

    std::vector<Track> trackList;
    std::vector<Playlist> playlistList;
    std::map<juce::int64, size_t> byId;
    std::map<juce::String, size_t> byPath;
    juce::int64 lastId = 0;
    bool loaded = false;
    bool dirty = false;
    int queued = 0;
    bool paused = false;
    std::vector<std::pair<juce::int64, bool>> waiting; // queued while paused
    juce::ThreadPool pool{2};
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
};

// A file path as the collection compares them (slashes, and case where the system ignores it).
juce::String normalisedPath(const juce::String& path);

// A track's cues as performance mode keeps them on a project item ({hot: [null | {pos,
// end?}] x8, memory: [{pos, end?}]}, seconds), and back: `previous` keeps the names and
// colours of cues that didn't move.
juce::var toPerfCues(const Track& track);
std::vector<Cue> cuesFromPerf(const juce::var& perfCues, const std::vector<Cue>& previous);

} // namespace stemcel::native::dj
