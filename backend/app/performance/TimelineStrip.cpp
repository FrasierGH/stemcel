#include "TimelineStrip.h"

#include "LiveState.h"
#include "Theme.h"
#include "TimelineActions.h"

#include <algorithm>
#include <cmath>

namespace stemcel::native::perf
{
namespace
{
constexpr float kRuler = 14.0F;

juce::String stamp(double ms)
{
    const int s = static_cast<int>(std::max(0.0, ms) / 1000.0);
    return juce::String(s / 60) + ":" + juce::String(s % 60).paddedLeft('0', 2);
}
} // namespace

TimelineStrip::TimelineStrip(ProjectMirror& m, TimelineActions& a, LiveState& l, std::function<void(double)> seekTo)
    : mirror(m), actions(a), live(l), seek(std::move(seekTo))
{
    setRepaintsOnMouseActivity(false);
    startTimerHz(30);
}

TimelineStrip::~TimelineStrip() = default;

void TimelineStrip::timerCallback()
{
    repaint();
}

double TimelineStrip::lengthMs() const
{
    double end = std::max(mirror.model().contentEndMs(), live.positionMs + 15000.0);
    if (pending)
        for (const auto& p : pending()) end = std::max(end, p.endMs + 15000.0);
    return std::max(30000.0, end);
}

juce::Rectangle<float> TimelineStrip::lanesArea() const
{
    return getLocalBounds().toFloat().reduced(1.0F).withTrimmedTop(kRuler);
}

float TimelineStrip::xAt(double ms) const
{
    const auto area = getLocalBounds().toFloat().reduced(1.0F);
    return area.getX() + static_cast<float>(ms / lengthMs()) * area.getWidth();
}

double TimelineStrip::msAt(float x) const
{
    const auto area = getLocalBounds().toFloat().reduced(1.0F);
    return juce::jlimit(0.0, lengthMs(), (x - area.getX()) / std::max(1.0F, area.getWidth()) * lengthMs());
}

void TimelineStrip::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour(colours::well);
    g.fillRoundedRectangle(bounds, 4.0F);
    const auto& model = mirror.model();
    const double length = lengthMs();

    // The ruler: a tick every so many seconds, at least 70 px apart.
    const auto area = bounds.reduced(1.0F);
    double step = 1000.0;
    for (const double s : {1000.0, 5000.0, 10000.0, 15000.0, 30000.0, 60000.0, 120000.0, 300000.0, 600000.0})
    {
        step = s;
        if (area.getWidth() * s / length >= 70.0) break;
    }
    g.setFont(uiFont(9.5F));
    for (double t = 0.0; t <= length; t += step)
    {
        const float x = xAt(t);
        g.setColour(colours::rule);
        g.drawVerticalLine(juce::roundToInt(x), area.getY(), area.getBottom());
        g.setColour(colours::textFaint);
        g.drawText(stamp(t), juce::Rectangle<float>(x + 3.0F, area.getY(), 50.0F, kRuler - 2.0F), juce::Justification::centredLeft);
    }

    // The tracks: one lane each, the clips in it.
    const auto lanes = lanesArea();
    const auto pendingNow = pending ? pending() : std::vector<Pending>{};
    const int recLanes = pendingNow.empty() ? 0 : 2;
    const int count = std::max(1, static_cast<int>(model.tracks.size()) + recLanes);
    const float laneH = std::clamp(lanes.getHeight() / static_cast<float>(count), 2.0F, 12.0F);
    float y = lanes.getY();
    for (const auto& track : model.tracks)
    {
        const auto colour = track.colorIndex >= 0 ? paletteSwatch(track.colorIndex) : colours::zinc500;
        for (const auto& clip : track.clips)
        {
            const float x0 = xAt(clip.offsetMs), x1 = xAt(clip.endMs());
            g.setColour(colour.withAlpha(track.muted ? 0.25F : 0.75F));
            g.fillRect(juce::Rectangle<float>(x0, y + 0.5F, std::max(1.0F, x1 - x0), laneH - 1.0F));
        }
        y += laneH;
        if (y > lanes.getBottom()) break;
    }
    // What the take now recording will add, a lane per deck.
    for (const auto& p : pendingNow)
    {
        const float laneY = y + static_cast<float>(p.deck - 1) * laneH;
        const float x0 = xAt(p.startMs), x1 = xAt(p.endMs);
        g.setColour(colours::red500.withAlpha(0.7F));
        g.fillRect(juce::Rectangle<float>(x0, laneY + 0.5F, std::max(1.0F, x1 - x0), laneH - 1.0F));
    }

    // The selected range (or the one being dragged).
    std::optional<std::pair<double, double>> range = model.timelineSelection;
    if (selecting && dragFrom >= 0.0F && std::abs(dragTo - dragFrom) > 3.0F)
        range = std::make_pair(msAt(std::min(dragFrom, dragTo)), msAt(std::max(dragFrom, dragTo)));
    if (range.has_value() && range->second > range->first)
    {
        const float x0 = xAt(range->first), x1 = xAt(range->second);
        g.setColour(colours::amber400.withAlpha(0.22F));
        g.fillRect(juce::Rectangle<float>(x0, area.getY(), x1 - x0, area.getHeight()));
        g.setColour(colours::amber400);
        g.drawVerticalLine(juce::roundToInt(x0), area.getY(), area.getBottom());
        g.drawVerticalLine(juce::roundToInt(x1), area.getY(), area.getBottom());
    }

    // The playhead.
    const float px = xAt(live.positionMs);
    g.setColour(juce::Colours::white);
    g.fillRect(juce::Rectangle<float>(px - 1.0F, area.getY(), 2.0F, area.getHeight()));
    g.setColour(colours::text);
    g.setFont(uiFont(10.0F, true));
    g.drawText(stamp(live.positionMs), juce::Rectangle<float>(px + 4.0F, area.getY(), 50.0F, kRuler - 2.0F), juce::Justification::centredLeft);
}

void TimelineStrip::mouseDown(const juce::MouseEvent& e)
{
    dragFrom = dragTo = e.position.x;
    // The ruler moves the playhead; the lanes select (a click without a drag moves it too).
    selecting = e.position.y > kRuler;
    if (!selecting && seek) seek(msAt(e.position.x));
}

void TimelineStrip::mouseDrag(const juce::MouseEvent& e)
{
    dragTo = e.position.x;
    if (!selecting && seek) seek(msAt(e.position.x));
}

void TimelineStrip::mouseUp(const juce::MouseEvent& e)
{
    if (selecting)
    {
        if (std::abs(e.position.x - dragFrom) > 3.0F)
            dragForTest(dragFrom, e.position.x);
        else if (seek)
            seek(msAt(e.position.x));
    }
    selecting = false;
    dragFrom = dragTo = -1.0F;
}

void TimelineStrip::mouseDoubleClick(const juce::MouseEvent&)
{
    actions.setTimelineSelection(std::nullopt, false);
    if (onSelection) onSelection();
}

void TimelineStrip::dragForTest(float x0, float x1)
{
    const double from = msAt(std::min(x0, x1)), to = msAt(std::max(x0, x1));
    if (to - from < 50.0) return;
    actions.setTimelineSelection(std::make_pair(from, to), false);
    if (onSelection) onSelection();
}

} // namespace stemcel::native::perf
