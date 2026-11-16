#pragma once

// The timeline's horizontal scrollbar, which also zooms: drag the thumb's middle to scroll,
// or either end to show more or less of the arrangement (the zoom follows, and with it the
// transport bar's zoom slider). A double-click shows everything.

#include "Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace stemcel::native
{

class ZoomScrollBar final : public juce::Component
{
  public:
    // What the bar spans (0..totalMs) and what the view shows (startMs..endMs).
    void setRange(double newTotalMs, double newStartMs, double newEndMs)
    {
        if (drag != Part::none) return; // the drag owns the range until it ends
        totalMs = juce::jmax(1.0, juce::jmax(newTotalMs, newEndMs));
        startMs = newStartMs;
        endMs = newEndMs;
        repaint();
    }

    // The new visible range, from a drag of the thumb or one of its ends.
    std::function<void(double startMs, double endMs)> onChange;
    std::function<void()> onShowAll;

    double visibleStartMs() const { return startMs; }
    double visibleEndMs() const { return endMs; }

    // For the smoke test: a drag of the right end by dx pixels.
    void dragEndForTest(float dx)
    {
        const auto thumb = thumbBounds();
        const juce::Point<float> from{thumb.getRight() - 2.0F, thumb.getCentreY()};
        beginDrag(from);
        continueDrag(from.x + dx);
        drag = Part::none;
    }

    void paint(juce::Graphics& g) override
    {
        const auto track = getLocalBounds().toFloat();
        g.setColour(colours::zinc950.withAlpha(0.6F));
        g.fillRoundedRectangle(track, track.getHeight() * 0.5F);
        const auto thumb = thumbBounds();
        const bool active = drag != Part::none || isMouseOver(true);
        g.setColour(colours::textDim.withAlpha(active ? 0.55F : 0.38F));
        g.fillRoundedRectangle(thumb, thumb.getHeight() * 0.5F);
        // The grips at each end, which zoom.
        g.setColour(colours::text.withAlpha(active ? 0.9F : 0.6F));
        for (const float x : {thumb.getX() + kGrip * 0.5F, thumb.getRight() - kGrip * 0.5F})
        {
            g.fillRect(x - 1.5F, thumb.getY() + 2.5F, 1.0F, thumb.getHeight() - 5.0F);
            g.fillRect(x + 0.5F, thumb.getY() + 2.5F, 1.0F, thumb.getHeight() - 5.0F);
        }
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        const auto part = partAt(e.position);
        setMouseCursor(part == Part::start || part == Part::end ? juce::MouseCursor::LeftRightResizeCursor
                                                                : juce::MouseCursor::NormalCursor);
        repaint();
    }
    void mouseExit(const juce::MouseEvent&) override { repaint(); }
    void mouseDown(const juce::MouseEvent& e) override { beginDrag(e.position); }
    void mouseDrag(const juce::MouseEvent& e) override { continueDrag(e.position.x); }
    void mouseUp(const juce::MouseEvent&) override
    {
        drag = Part::none;
        repaint();
    }
    void mouseDoubleClick(const juce::MouseEvent&) override
    {
        if (onShowAll) onShowAll();
    }

  private:
    enum class Part
    {
        none,
        start,
        end,
        middle
    };
    static constexpr float kGrip = 8.0F;    // each end's grab zone
    static constexpr float kMinThumb = 24.0F; // two grips and some middle

    double msPerPx() const { return totalMs / juce::jmax(1.0, static_cast<double>(getWidth())); }

    juce::Rectangle<float> thumbBounds() const
    {
        const auto w = static_cast<double>(getWidth());
        float x0 = static_cast<float>(startMs / totalMs * w);
        float x1 = static_cast<float>(endMs / totalMs * w);
        // Never too thin to grab: grow it about its centre.
        if (x1 - x0 < kMinThumb)
        {
            const float c = juce::jlimit(kMinThumb * 0.5F, static_cast<float>(w) - kMinThumb * 0.5F, (x0 + x1) * 0.5F);
            x0 = c - kMinThumb * 0.5F;
            x1 = c + kMinThumb * 0.5F;
        }
        return {x0, 1.0F, x1 - x0, static_cast<float>(getHeight()) - 2.0F};
    }

    Part partAt(juce::Point<float> p) const
    {
        const auto thumb = thumbBounds();
        if (p.x >= thumb.getX() - 2.0F && p.x <= thumb.getX() + kGrip) return Part::start;
        if (p.x <= thumb.getRight() + 2.0F && p.x >= thumb.getRight() - kGrip) return Part::end;
        if (thumb.contains(p.x, thumb.getCentreY())) return Part::middle;
        return Part::none;
    }

    void beginDrag(juce::Point<float> p)
    {
        drag = partAt(p);
        dragStartX = p.x;
        dragStartMs = startMs;
        dragEndMs = endMs;
        if (drag == Part::none)
        {
            // A click beside the thumb centres the view there.
            const double span = endMs - startMs;
            const double centre = static_cast<double>(p.x) * msPerPx();
            emit(centre - span * 0.5, centre + span * 0.5);
            drag = Part::middle;
            dragStartMs = startMs;
            dragEndMs = endMs;
        }
    }

    void continueDrag(float x)
    {
        const double delta = static_cast<double>(x - dragStartX) * msPerPx();
        const double minSpan = juce::jmax(50.0, msPerPx() * 2.0);
        switch (drag)
        {
        case Part::middle:
        {
            const double span = dragEndMs - dragStartMs;
            const double from = juce::jlimit(0.0, juce::jmax(0.0, totalMs - span), dragStartMs + delta);
            emit(from, from + span);
            break;
        }
        case Part::start: emit(juce::jlimit(0.0, dragEndMs - minSpan, dragStartMs + delta), dragEndMs); break;
        case Part::end: emit(dragStartMs, juce::jlimit(dragStartMs + minSpan, totalMs, dragEndMs + delta)); break;
        case Part::none: break;
        }
    }

    void emit(double from, double to)
    {
        startMs = from;
        endMs = to;
        repaint();
        if (onChange) onChange(from, to);
    }

    double totalMs = 1.0, startMs = 0.0, endMs = 1.0;
    Part drag = Part::none;
    float dragStartX = 0.0F;
    double dragStartMs = 0.0, dragEndMs = 0.0;
};

} // namespace stemcel::native
