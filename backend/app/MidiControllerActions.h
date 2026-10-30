#pragma once

// MIDI controller actions: the engine maps controller input to named controls
// (MIDI_CONTROL) and this turns them into what they do in the arrange view — play,
// jog, markers, track and master dials, browsing tracks and clips. A port of the
// Electron renderer's midiControllerActions.ts, midiBrowseActions.ts and
// midiPlaybackHold.ts, with the same pacing, catch-up and hold rules.

#include "AppPreferences.h"
#include "EngineClient.h"
#include "LiveState.h"
#include "ProjectModel.h"
#include "TimelineActions.h"

#include <juce_events/juce_events.h>

#include <functional>
#include <map>
#include <optional>
#include <set>

namespace stemcel::native
{

class MidiControllerActions final : private juce::Timer
{
  public:
    struct Hooks
    {
        std::function<void()> togglePlay;
        std::function<void(double)> reveal;                  // scroll the timeline to a position
        std::function<void(const juce::String&)> revealTrack; // scroll a track into view
        std::function<void(double)> zoom;                    // zoom factor
        std::function<void()> showTrackFx;
        std::function<bool()> blocked;     // a modal dialog or editor owns input
        std::function<bool()> cueScratch;  // Cue during a scratch session; true when it was handled
    };

    MidiControllerActions(EngineClient& engine, ProjectMirror& mirror, TimelineActions& actions, TimelineViewState& view,
                          LiveState& live, AppPreferences& prefs, Hooks hooks);
    ~MidiControllerActions() override;

    void handleControl(const juce::var& payload);

  private:
    struct DialCatchUp
    {
        double from = 0.0, target = 0.0, startedAt = 0.0, lastOutput = 0.0;
        std::function<std::optional<double>()> read;
        std::function<void(double)> apply;
    };
    struct ClipBrowse
    {
        juce::String trackId, cursorClipId;
    };

    void timerCallback() override;
    void schedule();

    // Jog
    void queueJog(const juce::String& control, double delta, bool snapToGrid, const juce::String& device);
    void flushJog(double now);
    // Dials
    void handleDial(const juce::String& key, double hardware, std::function<std::optional<double>()> read,
                    std::function<void(double)> apply);
    void flushDials(double now);
    void applyAbsolute(const juce::var& payload);
    std::optional<double> readTrackControl(const juce::String& trackId, const juce::String& control) const;
    void applyTrackControl(const juce::String& trackId, const juce::String& control, double value);
    juce::String gestureFor(const juce::String& key);
    // Browse
    bool browseRotation(const juce::String& device, double delta, bool extendSelection);
    void browsePress(const juce::String& device);
    void browseTracks(double delta);
    std::vector<const ClipModel*> orderedClips(const juce::String& trackId) const;
    ClipBrowse* activeClipBrowse(const juce::String& device);
    // Jog-touch playback hold
    void jogTouch(const juce::String& device, int deck, bool pressed);
    bool endHold(const juce::String& key);
    bool endHoldsForDevice(const juce::String& device);
    void resumeAfterFinalRelease(bool finalRelease);
    // Markers
    void seekTo(double positionMs);
    void previousMarker();
    void nextMarker();
    void markerJump(int index);
    void toggleMarker();

    void devicesList(const juce::var& payload);
    void deckSelection(const juce::var& payload);
    void suspend();
    bool scrubAudioEnabled(const juce::String& device) const;
    const ProjectModel& model() const { return mirror.model(); }

    EngineClient& engine;
    ProjectMirror& mirror;
    TimelineActions& actions;
    TimelineViewState& view;
    LiveState& live;
    AppPreferences& prefs;
    Hooks hooks;
    Subscriptions subscriptions;

    // Jog accumulation, flushed once per frame.
    double pendingSeekDeltaMs = 0.0;
    double pendingBeatSteps = 0.0;
    double snappedJogUnits = 0.0;
    double lastSnappedSeekAt = -1.0e12;
    double pendingSnapMinIntervalMs = 20.0;
    juce::String pendingJogMode; // "", "snapped", "free"
    juce::String pendingJogDevice;
    bool jogPending = false;

    std::map<juce::String, double> lastDialValues;
    std::map<juce::String, DialCatchUp> dialCatchUps;
    std::map<juce::String, std::pair<juce::String, double>> dialGestures; // key -> (gesture id, last ms)

    std::map<juce::String, ClipBrowse> clipBrowse;
    std::set<juce::String> holds; // device:deck keys holding playback
    bool heldPlayback = false;     // a jog touch paused playback that the last release resumes
    bool syncPressed[3] = {false, false, false};
    bool wasBlocked = false;
    bool deckSelectionSynced = false;
    std::set<juce::String> enabledInputs;
};

} // namespace stemcel::native
