#pragma once

// Performance mode's view of the timeline, across the top: every track's clips in a lane,
// the playhead, the selected range, and what the take now recording will add (each deck's
// stretches, red, from where the take lands). Click to move the playhead; drag to select a
// range (the browser's Timeline folder then offers it for the decks); double-click clears
// the range.

#include "ProjectModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace stemcel::native
{
class TimelineActions;
struct LiveState;
} // namespace stemcel::native

namespace stemcel::native::perf
{

class TimelineStrip final : public juce::Component, private juce::Timer
{
  public:
    // A stretch the recording take will add: on deck `deck`'s lane, in timeline ms.
    struct Pending
    {
        int deck = 1;
        double startMs = 0.0, endMs = 0.0;
    };

    TimelineStrip(ProjectMirror& mirror, TimelineActions& actions, LiveState& live, std::function<void(double)> seek);
    ~TimelineStrip() override;

    std::function<std::vector<Pending>()> pending; // the take being recorded, if any
    std::function<void()> onSelection;              // after a range is set or cleared

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;

    // Smoke-test support: the range a drag from x0 to x1 (pixels) would select.
    void dragForTest(float x0, float x1);
    double msAt(float x) const;

  private:
    void timerCallback() override;
    double lengthMs() const;
    float xAt(double ms) const;
    juce::Rectangle<float> lanesArea() const;

    ProjectMirror& mirror;
    TimelineActions& actions;
    LiveState& live;
    std::function<void(double)> seek;
    float dragFrom = -1.0F, dragTo = -1.0F;
    bool selecting = false;
};

} // namespace stemcel::native::perf
