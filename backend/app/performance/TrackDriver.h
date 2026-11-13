#pragma once

// A performance channel driving a timeline track: the channel's knobs, filter and effects
// act on the track as the timeline plays (live values over its automation), its PLAY, CUE and
// jog work the timeline transport, and with REC on, what is moved while the timeline plays
// is written into the track's automation over the stretch it was moved for. A moved control
// holds where it was left (as a knob does) until the timeline stops; that is when the stretch
// is written, as one undo step, and the track goes back to its curves.

#include "EngineClient.h"
#include "LiveState.h"
#include "PerfState.h"
#include "ProjectModel.h"
#include "TimelineActions.h"

#include <array>
#include <map>

namespace stemcel::native::perf
{

class TrackDriver
{
  public:
    TrackDriver(EngineClient& engine, ProjectMirror& mirror, TimelineActions& actions, LiveState& live);

    // `trackId` empty: the deck again.
    void bind(int deck, const juce::String& trackId);
    const juce::String& boundTrack(int deck) const { return bindings[static_cast<size_t>(deck - 1)].trackId; }
    bool anyBound() const;

    // Each PERF_STATE.
    void update(const State& state);
    // PLAY, CUE and the jog of a bound channel.
    void transport(const juce::String& control, double value);
    // Writes what is held (when it was recording) and lets every track go.
    void finish();

    // The channel's values as track parameters (also for tests).
    static std::map<juce::String, double> valuesFor(const State& state, int deck, double bpm);

  private:
    struct Touch
    {
        std::vector<std::pair<double, double>> points; // (timeline ms, value)
        bool writing = false;
    };
    struct Binding
    {
        juce::String trackId;
        std::map<juce::String, double> last;
        std::map<juce::String, Touch> held; // controls moved since the timeline started (or ever, stopped)
        bool primed = false;
    };
    void commit(bool write);
    void sendLive(const juce::String& trackId, const std::map<juce::String, double>& values);
    void clearLive(const juce::String& trackId);

    EngineClient& engine;
    ProjectMirror& mirror;
    TimelineActions& actions;
    LiveState& live;
    std::array<Binding, kDecks> bindings;
    bool wasPlaying = false;
    bool recording = false;
    double playStartMs = 0.0;
};

} // namespace stemcel::native::perf
