#pragma once

// The decks' close-up waveforms one above the other, across the width of performance mode,
// as rekordbox and Serato can show them: both on the same time scale around the same
// centre line, so their beats line up when the decks are in sync. The decks' own screens
// keep the track, the time and the overview.

#include "DeckDisplay.h"
#include "Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace stemcel::native::perf
{

class StackedWaveforms final : public juce::Component
{
  public:
    StackedWaveforms(DeckDisplay& deck1, DeckDisplay& deck2) : top(deck1), bottom(deck2) {}

    void paint(juce::Graphics& g) override
    {
        g.setColour(colours::panel);
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0F);
        auto r = getLocalBounds().reduced(6);
        const int half = (r.getHeight() - 4) / 2;
        auto upper = r.removeFromTop(half);
        r.removeFromTop(4);
        auto lower = r;
        int number = 1;
        for (auto [display, area] : {std::pair<DeckDisplay*, juce::Rectangle<int>>{&top, upper}, {&bottom, lower}})
        {
            auto label = area.removeFromLeft(20);
            g.setColour(colours::accent);
            g.setFont(uiFont(15.0F, true));
            g.drawText(juce::String(number++), label, juce::Justification::centred);
            area.removeFromLeft(2);
            display->paintCloseUpInto(g, area.toFloat());
        }
    }

  private:
    DeckDisplay& top;
    DeckDisplay& bottom;
};

} // namespace stemcel::native::perf
