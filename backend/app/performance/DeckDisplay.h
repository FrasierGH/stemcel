#pragma once

// A deck's screen: the track, its tempo and key, the time, the whole track's waveform with
// the cue, hot cues and loop, and the scrolling close-up with the beat grid, as a CDJ shows.

#include "PerfState.h"
#include "WaveformStore.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace stemcel::native::perf
{

class DeckDisplay final : public juce::Component
{
  public:
    DeckDisplay(int deckNumber, WaveformStore& store) : deck(deckNumber), waveforms(store) {}

    // Called every frame: `posSec` is the playhead extrapolated to now.
    void update(const DeckState& state, const DeckTrackInfo& info, double posSec, bool master);
    void setZoom(double secondsEachSide) { halfWindow = juce::jlimit(1.0, 16.0, secondsEachSide); }
    double zoom() const { return halfWindow; }
    std::function<void(double seconds)> onSeek; // a click on the overview

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;

  private:
    juce::Rectangle<int> overviewArea() const;
    juce::Rectangle<int> closeUpArea() const;
    void paintCloseUp(juce::Graphics&, juce::Rectangle<float> area);
    void paintOverview(juce::Graphics&, juce::Rectangle<float> area);

    const int deck;
    WaveformStore& waveforms;
    DeckState state;
    DeckTrackInfo info;
    double pos = 0.0;
    bool isMaster = false;
    double halfWindow = 4.0;
};

} // namespace stemcel::native::perf
