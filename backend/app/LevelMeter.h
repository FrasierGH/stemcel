#pragma once

// The segmented level meter the transport bar's master meter draws, shared so every meter in
// the app reads the same: an L and an R lane from -60 to 0 dBFS in two-pixel segments (green,
// amber above -12, red above -3), the unlit segments faint like LEDs that are off, and each
// lane's peak hold as a bright tick. Horizontal (the transport bar) or vertical (a channel
// beside its fader, 0 dB at the top).

#include "Db.h"
#include "Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

namespace stemcel::native::meter
{

inline constexpr float kFloorDb = -60.0F;
inline constexpr std::array<int, 5> kMarks{-48, -24, -12, -6, 0};

inline juce::Colour zoneColour(float atDb)
{
    return atDb > -3.0F ? colours::red500 : atDb > -12.0F ? colours::amber400 : colours::emerald500;
}

// How far along the meter `dbValue` sits, 0 (the floor) to 1 (0 dBFS).
inline float proportion(float dbValue)
{
    return (juce::jlimit(kFloorDb, 0.0F, dbValue) - kFloorDb) / -kFloorDb;
}

// Peaks are linear (0..1+); a hold of 0 draws no tick.
inline void drawSegments(juce::Graphics& g, juce::Rectangle<float> area, std::array<float, 2> peaks, std::array<float, 2> holds,
                         bool vertical)
{
    drawWell(g, area.expanded(1.0F));
    const float length = vertical ? area.getHeight() : area.getWidth();
    for (size_t ch = 0; ch < 2; ++ch)
    {
        const auto lane = (vertical ? area.withWidth(area.getWidth() / 2.0F).withX(area.getX() + static_cast<float>(ch) * area.getWidth() / 2.0F)
                                          .reduced(0.5F, 0.0F)
                                    : area.withHeight(area.getHeight() / 2.0F).withY(area.getY() + static_cast<float>(ch) * area.getHeight() / 2.0F)
                                          .reduced(0.0F, 1.0F));
        const float level = proportion(static_cast<float>(db::linearToDb(peaks[ch]))) * length;
        for (float at = 0.0F; at < length; at += 3.0F)
        {
            const float atDb = kFloorDb + at / length * -kFloorDb;
            const auto colour = zoneColour(atDb);
            g.setColour(at < level ? colour : colour.withAlpha(0.12F));
            if (vertical) g.fillRect(lane.getX(), lane.getBottom() - at - 2.0F, lane.getWidth(), 2.0F);
            else g.fillRect(lane.getX() + at, lane.getY(), 2.0F, lane.getHeight());
        }
        if (holds[ch] > 0.0F)
        {
            const float at = proportion(static_cast<float>(db::linearToDb(holds[ch]))) * length;
            g.setColour(colours::zinc100);
            if (vertical) g.fillRect(lane.getX(), lane.getBottom() - at - 1.0F, lane.getWidth(), 2.0F);
            else g.fillRect(lane.getX() + at - 1.0F, lane.getY(), 2.0F, lane.getHeight());
        }
    }
}

// A peak that stays for 1.5 s, then falls to the live level.
struct PeakHold
{
    float held = 0.0F;
    double at = 0.0;
    void update(float peak, double nowMs)
    {
        if (peak >= held || nowMs - at > 1500.0)
        {
            held = peak;
            at = nowMs;
        }
    }
};

} // namespace stemcel::native::meter
