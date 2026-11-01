#pragma once

#include "Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace stemcel::native
{

// A thin bar between two panes that drags to resize them: a 1 px rule that lights up as a
// grip under the pointer. `onDrag` gets the distance from where the drag began (along the
// bar's axis), so the owner works out the new size from the size it had then.
class ResizeBar final : public juce::Component, public juce::SettableTooltipClient
{
  public:
    enum class Axis
    {
        rows,   // a horizontal bar: drags up and down
        columns // a vertical bar: drags left and right
    };

    explicit ResizeBar(Axis a) : axis(a)
    {
        setMouseCursor(axis == Axis::rows ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::LeftRightResizeCursor);
        setRepaintsOnMouseActivity(true);
    }

    std::function<void()> onDragStart;
    std::function<void(int delta)> onDrag;
    std::function<void()> onDragEnd;

    void paint(juce::Graphics& g) override
    {
        g.fillAll(colours::background);
        const auto b = getLocalBounds().toFloat();
        const bool live = isMouseOverOrDragging();
        g.setColour(live ? colours::sky500 : colours::border);
        if (axis == Axis::rows)
            g.fillRect(b.withSizeKeepingCentre(b.getWidth(), live ? 3.0f : 1.0f));
        else
            g.fillRect(b.withSizeKeepingCentre(live ? 3.0f : 1.0f, b.getHeight()));
    }

    void mouseDown(const juce::MouseEvent&) override
    {
        if (onDragStart) onDragStart();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (onDrag) onDrag(axis == Axis::rows ? e.getDistanceFromDragStartY() : e.getDistanceFromDragStartX());
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (onDragEnd) onDragEnd();
    }

  private:
    Axis axis;
};

} // namespace stemcel::native
