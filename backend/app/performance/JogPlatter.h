#pragma once

// A deck's spinning platter, as rekordbox and Serato draw one: a record turning at 33⅓ with
// the track (backwards when it goes back, still when it stops, scratched with the jog), a
// ring for how far through the track it is, the cue point on the ring, and a glow while the
// jog is touched.

#include "PerfState.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace stemcel::native::perf
{

class JogPlatter final : public juce::Component
{
  public:
    explicit JogPlatter(int deckNumber) : deck(deckNumber) { setInterceptsMouseClicks(false, false); }

    // The deck's state and where it is now (seconds, interpolated between states).
    void update(const DeckState& state, double positionSec)
    {
        const bool changed = std::abs(positionSec - position) > 1.0e-4 || state.loaded != s.loaded || state.touched != s.touched
                             || state.playing != s.playing || std::abs(state.cueSec - s.cueSec) > 1.0e-6;
        s = state;
        position = positionSec;
        if (changed) repaint();
    }

    void paint(juce::Graphics& g) override;

  private:
    int deck;
    DeckState s;
    double position = 0.0;
};

} // namespace stemcel::native::perf
