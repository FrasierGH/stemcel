#pragma once

#include "TempoCurve.h"

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

#include <optional>
#include <vector>

namespace stemcel
{
class ProjectState;
struct ProjectSession;
} // namespace stemcel

namespace stemcel::native
{

// The master bus's id wherever a track id is taken (plugins, automation): the engine's
// plugins::kMasterChainId.
inline const juce::String kMasterTrackId{"__master__"};


class EngineClient;

struct EnvelopePoint
{
    double timeMs = 0.0;
    double gain = 1.0;
};

struct ClipModel
{
    juce::String id;
    juce::String trackId;
    juce::String libraryItemId;
    juce::String name;
    double offsetMs = 0.0;       // timeline start
    double durationMs = 0.0;     // source-time length of the window
    double inMs = 0.0;           // source-time trim offset
    double effectiveDurationMs = 0.0; // timeline length (after warp)
    // The source tempo when the clip follows the tempo map (its waveform is drawn along it); 0 when not.
    double tempoFollowBpm = 0.0;
    double effectiveTempoRatio = 1.0; // source ms per timeline ms
    bool effectiveWarpActive = false;
    int colorIndex = -1;
    bool locked = false;
    bool reversed = false;
    bool brake = false;
    bool backspin = false;
    bool unresolved = false;
    bool warpEnabled = false;
    juce::String warpMode;
    double tempoRatio = 1.0;
    double semitones = 0.0;
    double cents = 0.0;
    double beatOffsetMs = 0.0;
    juce::String scratchPatternId;
    bool pendingAutoWarp = false;
    std::vector<EnvelopePoint> envelope;
    double gainDb = 0.0; // the clip's own gain
    juce::var raw; // the full clip object (e.g. to tell a pinned tempoRatio from an absent one)

    double endMs() const { return offsetMs + effectiveDurationMs; }
};

struct TransitionModel
{
    juce::String id;
    juce::String leftClipId;
    juce::String rightClipId;
    juce::String recipe;
};

struct TrackModel
{
    juce::String id;
    juce::String name;
    double gain = 1.0;
    double pan = 0.0;
    bool muted = false;
    bool soloed = false;
    double heightPx = 0.0; // 0 = default
    int colorIndex = -1;
    std::vector<ClipModel> clips;
    std::vector<TransitionModel> transitions;
    juce::var raw; // the full track object, for panels that read FX/automation fields
};

struct LibraryItemModel
{
    juce::String id;
    juce::String filePath;
    juce::String playbackFilePath;
    juce::String kind; // source | stem | sample | clip
    juce::String name;
    juce::String fileName;
    double durationMs = 0.0;
    double sampleRate = 0.0;
    int channelCount = 0;
    juce::String key;
    double bpm = 0.0;
    double beatAnchorSec = 0.0;
    std::vector<double> beats;
    juce::String sourceItemId;
    juce::String mediaId;
    bool unresolved = false;
    juce::var raw;

    juce::String displayName() const
    {
        if (name.isNotEmpty()) return name;
        if (fileName.isNotEmpty()) return fileName;
        return juce::File(filePath).getFileNameWithoutExtension();
    }
    // The file whose audio the waveform shows: the decoded cache when present.
    juce::String audioPath() const { return playbackFilePath.isNotEmpty() ? playbackFilePath : filePath; }
};

struct MarkerModel
{
    juce::String id;
    double positionMs = 0.0;
};

struct ProjectModel
{
    juce::String filePath;
    juce::String name;
    bool dirty = false;
    double bpm = 120.0;
    bool bpmSeeded = false;
    double projectLengthMs = 0.0;
    double pxPerSecond = 0.0;
    double scrollX = 0.0;
    juce::String selectedTrackId;
    juce::String snapGrid;
    std::optional<std::pair<double, double>> timelineSelection;
    bool loopSelection = false;
    double masterVolume = 1.0;
    std::vector<TrackModel> tracks;
    // The master bus as a row of its own: its automation lanes and inserts (id kMasterTrackId;
    // empty id until the engine reports it). Not one of `tracks`.
    TrackModel master;
    std::vector<LibraryItemModel> library;
    std::vector<MarkerModel> markers;
    // The tempo map (empty: the constant `bpm`), and the tempo over time it describes.
    std::vector<TempoPoint> tempoMap;
    TempoCurve tempoCurve() const { return TempoCurve(tempoMap, bpm); }
    juce::var raw;

    static ProjectModel fromState(const juce::var& state);

    const TrackModel* findTrack(const juce::String& id) const;
    const ClipModel* findClip(const juce::String& id) const;
    const LibraryItemModel* findLibraryItem(const juce::String& id) const;
    // End of the last clip on any track.
    double contentEndMs() const;
};

// The UI's read-only mirror of the engine's project. Rebuilt from the authoritative
// ProjectState (the same snapshot the bridge sends as PROJECT_STATE) whenever the project's
// ValueTree changes, coalesced to one rebuild per message-loop turn. In-process, there is
// no optimistic local state to reconcile: what the UI shows is what the engine holds.
class ProjectMirror final : public juce::ChangeBroadcaster,
                            private juce::ValueTree::Listener,
                            private juce::AsyncUpdater
{
  public:
    explicit ProjectMirror(EngineClient& engine);
    ~ProjectMirror() override;

    const ProjectModel& model() const { return current; }

    // Rebuilds now (e.g. right after a command whose effect the caller needs to read).
    void refreshNow();

  private:
    void handleAsyncUpdate() override;
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override { triggerAsyncUpdate(); }
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override { triggerAsyncUpdate(); }
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override { triggerAsyncUpdate(); }
    void valueTreeChildOrderChanged(juce::ValueTree&, int, int) override { triggerAsyncUpdate(); }
    void valueTreeRedirected(juce::ValueTree&) override { triggerAsyncUpdate(); }

    EngineClient& engine;
    juce::ValueTree tree;
    ProjectModel current;
    int subscription = 0;
};

} // namespace stemcel::native
