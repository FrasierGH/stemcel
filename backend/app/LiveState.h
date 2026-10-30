#pragma once

#include "EngineClient.h"

#include <juce_events/juce_events.h>

#include <map>
#include <set>

namespace stemcel::native
{

// High-rate engine state: playhead and meters, updated from the 60 Hz PLAYHEAD_UPDATE,
// MASTER_LEVEL and TRACK_LEVELS broadcasts. Components read it from their own repaint
// timers rather than being notified per update.
class LiveState
{
  public:
    explicit LiveState(EngineClient& engine);

    double positionMs = 0.0;
    bool playing = false;
    float masterPeakL = 0.0f;
    float masterPeakR = 0.0f;
    std::map<juce::String, std::pair<float, float>> trackPeaks;

  private:
    Subscriptions subscriptions;
};

// Timeline view state shared by the track headers and the timeline canvas.
class TimelineViewState final : public juce::ChangeBroadcaster
{
  public:
    static constexpr double kDefaultPxPerSecond = 100.0;
    static constexpr double kMinPxPerSecond = 10.0;
    static constexpr double kMaxPxPerSecond = 800.0;
    static constexpr int kRulerHeight = 28;
    static constexpr int kTempoLaneHeight = 56;
    static constexpr int kDefaultTrackHeight = 120;
    static constexpr int kMinTrackHeight = 120;
    static constexpr int kMaxTrackHeight = 400;
    static constexpr int kTrackGap = 2;

    double pxPerSecond = kDefaultPxPerSecond;
    double scrollXPx = 0.0; // horizontal scroll in pixels at the current zoom
    double scrollY = 0.0;
    juce::String selectedTrackId;
    std::set<juce::String> selectedClipIds;
    bool followPlayback = true;
    // Clicks on clips edit their volume envelope instead of selecting / moving them.
    bool volumeMode = false;
    // Preferences > Timeline: left/right waveforms (else the summary) and what the
    // transport's skip buttons jump to (markers, else the timeline's ends).
    bool stereoWaveforms = true;
    bool skipToMarkers = true;
    // The tempo lane under the ruler (the tempo map's curve and points).
    bool tempoLaneVisible = false;
    // Where the tracks start: below the ruler and, when shown, the tempo lane.
    int headerHeight() const { return kRulerHeight + (tempoLaneVisible ? kTempoLaneHeight : 0); }

    double msToX(double ms) const { return ms / 1000.0 * pxPerSecond - scrollXPx; }
    double xToMs(double x) const { return (x + scrollXPx) / pxPerSecond * 1000.0; }

    void changed() { sendChangeMessage(); }
};

} // namespace stemcel::native
