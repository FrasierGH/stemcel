#pragma once

// The decks' close-up waveforms one above the other, across the width of performance mode,
// as rekordbox and Serato can show them: all on the same time scale around the same centre
// line, so their beats line up when the decks are in sync. The decks' own screens keep the
// track, the time and the overview. Two lanes, or four with 4 DECKS.

#include "DeckDisplay.h"
#include "Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <utility>
#include <vector>

namespace stemcel::native::perf
{

class StackedWaveforms final : public juce::Component
{
  public:
    // (deck number, its display), top to bottom.
    void setDecks(std::vector<std::pair<int, DeckDisplay*>> list)
    {
        lanes = std::move(list);
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        g.setColour(colours::panel);
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0F);
        if (lanes.empty()) return;
        auto r = getLocalBounds().reduced(6);
        const int gap = 4;
        const int laneHeight = (r.getHeight() - gap * (static_cast<int>(lanes.size()) - 1)) / static_cast<int>(lanes.size());
        for (const auto& [number, display] : lanes)
        {
            auto area = r.removeFromTop(laneHeight);
            r.removeFromTop(gap);
            auto label = area.removeFromLeft(20);
            g.setColour(colours::accent);
            g.setFont(uiFont(15.0F, true));
            g.drawText(juce::String(number), label, juce::Justification::centred);
            area.removeFromLeft(2);
            display->paintCloseUpInto(g, area.toFloat());
        }
    }

  private:
    std::vector<std::pair<int, DeckDisplay*>> lanes;
};

} // namespace stemcel::native::perf
