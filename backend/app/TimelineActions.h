#pragma once

#include "EngineClient.h"
#include "Automation.h"
#include "ProjectModel.h"

#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace stemcel::native
{

// Clip, track and transport edits, sent as the same bridge commands the renderer's project
// store sends (stores/projectClip*Actions.ts, projectTrackActions.ts), so behaviour and undo
// steps match the Electron app exactly.
class TimelineActions
{
  public:
    TimelineActions(EngineClient& engine, ProjectMirror& mirror);
    EngineClient& engineClient() { return engine; }

    // User-facing refusals (e.g. splitting a locked clip).
    std::function<void(const juce::String& message, bool isError)> onNotify;

    // ── clips ──────────────────────────────────────────────────────────────
    // Returns the new right-half clip id, or empty when refused.
    juce::String splitClipAt(const juce::String& clipId, double atMs);
    juce::String duplicateClip(const juce::String& clipId);
    void removeClips(const std::vector<juce::String>& clipIds);
    void moveClip(const juce::String& clipId, double positionMs, const juce::String& trackId);
    // Trim in timeline terms: new timeline start and end; source window follows via the
    // clip's tempo ratio.
    void trimClip(const juce::String& clipId, double newStartMs, double newEndMs);
    void setClipLocked(const juce::String& clipId, bool locked);
    // Master tempo (key lock) on stretches a warped clip at its own pitch; off plays it like a
    // turntable, its pitch moving with its tempo (the repitch warp mode).
    void setClipMasterTempo(const juce::String& clipId, bool on);
    // Shifts a clip's key by whole semitones at its current tempo (through its warp; an unwarped
    // clip gets a pinned 1.0 ratio, so only its pitch moves). 0 resets it.
    void setClipKeyShift(const juce::String& clipId, int semitones);
    void setClipColor(const juce::String& clipId, int colorIndex);
    void renameClip(const juce::String& clipId, const juce::String& name);
    void setClipReversed(const juce::String& clipId, bool reversed);
    void setClipEnvelope(const juce::String& clipId, const std::vector<EnvelopePoint>& points);
    void setClipBrake(const juce::String& clipId, bool on);
    void setClipBackspin(const juce::String& clipId, bool on);
    // Saves the clip's window as a library clip and binds the clip to it; returns its id.
    juce::String saveClipToLibrary(const juce::String& clipId);
    // Renders the clip (as heard) into a new sample in the library.
    void saveClipAsSample(const juce::String& clipId, const juce::String& audioType);
    // Cuts the clip's source window at every `beatsPerSlice` of its beat grid and saves
    // each piece as a sample; returns how many.
    int chopClipToGrid(const juce::String& clipId, double beatsPerSlice, const juce::String& audioType);
    // Saves the pieces between source-ms cut markers (plus head and tail) as samples.
    int sliceClipAtMarkers(const juce::String& clipId, std::vector<double> markersSourceMs, const juce::String& audioType);

    // ── crossfades & beat repeat ───────────────────────────────────────────
    void createTransition(const juce::String& trackId, const juce::String& leftClipId,
                          const juce::String& rightClipId, const juce::String& recipe = {});
    void deleteTransition(const juce::String& trackId, const juce::String& transitionId);
    void setTransitionRecipe(const juce::String& trackId, const juce::String& transitionId, const juce::String& recipe);
    // After a trim leaves the clip overlapping a neighbour tail-to-head, crossfade them
    // (projectTransitionActions.maybeCreateTransitionAfterTrim).
    void maybeCreateTransitionAfterTrim(const juce::String& clipId, bool rightEdge);
    // Automation curves (TRACK_SET_AUTOMATION; empty clears) and which lanes a track shows.
    void setAutomation(const juce::String& trackId, const juce::String& paramId,
                       const std::vector<automation::Point>& points, const juce::String& gestureId = {},
                       bool gestureEnd = true);
    void setAutomationLanes(const juce::String& trackId, const std::vector<automation::Lane>& lanes);
    // Shows or hides a track's (or the master's) automation lanes: the A button and key.
    // Hiding remembers which lanes were shown, to bring them back; a track opened for the
    // first time shows a lane for each parameter it has points for, else its gain.
    void toggleAutomation(const juce::String& trackId);
    // The parameters a track has automation points for.
    juce::StringArray automatedParams(const juce::String& trackId) const;
    void addBeatRepeat(const juce::String& trackId, double startBeat, double lengthBeats, const juce::String& division);
    void deleteBeatRepeat(const juce::String& trackId, const juce::String& regionId);

    // ── clipboard (projectClipClipboardActions) ────────────────────────────
    // Copies clips with their offsets from the group's first clip and top track.
    bool copyClips(const std::vector<juce::String>& clipIds);
    bool cutClips(const std::vector<juce::String>& clipIds);
    // Pastes at `positionMs` on `trackId` (lower tracks for a multi-track group), only into
    // free space. Returns the new clip ids (empty when refused).
    std::vector<juce::String> paste(const juce::String& trackId, double positionMs);
    bool hasClipboard() const { return !clipboard.empty(); }

    // Preferences > General: warp dropped audio to the project tempo.
    bool autoWarpOnDrop = true;

    // Places a library item on a track (drag from the library).
    juce::String addClip(const juce::String& trackId, const LibraryItemModel& libraryItem, double positionMs);

    // ── tracks ─────────────────────────────────────────────────────────────
    juce::String addTrack();
    void removeTrack(const juce::String& trackId);
    void renameTrack(const juce::String& trackId, const juce::String& name);
    void setTrackGain(const juce::String& trackId, double gain);
    void setTrackPan(const juce::String& trackId, double pan);
    void setTrackMuted(const juce::String& trackId, bool muted);
    void setTrackSoloed(const juce::String& trackId, bool soloed);
    void setTrackColor(const juce::String& trackId, int colorIndex);
    void setTrackHeight(const juce::String& trackId, double heightPx);
    void moveTrack(const juce::String& trackId, int newIndex);

    // ── transport & view ───────────────────────────────────────────────────
    void play();
    void pause();
    void stop();
    void seek(double positionMs);
    void addMarker(double positionMs);
    void removeMarker(const juce::String& markerId);
    void moveMarker(const juce::String& markerId, double positionMs);
    void setTimelineSelection(std::optional<std::pair<double, double>> range, bool loop);
    void setView(double pxPerSecond, double scrollXPx, const juce::String& selectedTrackId);
    void setSnapGrid(const juce::String& grid);
    void setMasterVolume(double gain);
    void setBpm(double bpm, bool autoWarp);
    // Replaces the tempo map (empty: the constant project tempo). One undo step.
    void setTempoMap(const std::vector<TempoPoint>& points);

    // The project's snap grid, with the renderer's default when unset.
    juce::String snapGrid() const;
    // Snaps a timeline time to the project grid (4/4 at the project BPM); "free" passes through.
    double snap(double ms) const;
    double snapStepMs() const;
    // Snap a placed clip onto the project beat grid by moving it: its first in-window beat
    // travels the shortest way to a project beat (projectClipPlacementActions.ts
    // alignClipToBarGrid). Skips samples, locked clips, tempo mismatches and moves that
    // would overlap a neighbour (reported as blocked).
    enum class Align { moved, blocked, skip };
    Align alignClipToBarGrid(const juce::String& clipId);
    // Every placed clip of an item that is not waiting to warp; the blocked count.
    int alignItemClipsToGrid(const juce::String& itemId);

  private:
    const ProjectModel& model() const { return mirror.model(); }
    void notify(const juce::String& message, bool isError);
    void replayClipProperties(const ClipModel& source, const juce::String& newClipId, bool includeLock);

    EngineClient& engine;
    ProjectMirror& mirror;
    std::map<juce::String, juce::String> duplicateTailBySource;
    std::map<juce::String, std::vector<automation::Lane>> hiddenLanes; // by track, while its automation is hidden
    struct ClipboardItem
    {
        ClipModel clip;
        double relStartMs = 0.0;
        int relTrackIndex = 0;
        // The clip's library item and those it derives from (source first), so a paste into
        // another project (another tab) can add what that project's library lacks.
        std::vector<juce::var> libraryItems;
    };
    std::vector<ClipboardItem> clipboard;
};

// Envelope helpers ported from lib/envelope.ts.
namespace envelope
{
std::vector<EnvelopePoint> sanitize(std::vector<EnvelopePoint> points);
double gainAt(const std::vector<EnvelopePoint>& points, double timeMs);
bool isFlatUnity(const std::vector<EnvelopePoint>& points);
// Splits a timeline-footprint envelope at `atMs`; an empty side means "no envelope".
std::pair<std::vector<EnvelopePoint>, std::vector<EnvelopePoint>> splitAt(const std::vector<EnvelopePoint>& points,
                                                                          double atMs);
} // namespace envelope

} // namespace stemcel::native
