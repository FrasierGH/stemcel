#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <deque>

namespace stemcel::native
{

// Transient notifications stacked in the bottom-right corner (the renderer's
// NotificationToasts). Transparent to the mouse except over a toast; click to dismiss.
class Toasts final : public juce::Component, private juce::Timer
{
  public:
    Toasts();

    void show(const juce::String& message, bool isError);
    // Preferences > Timeline: notifications off (still logged).
    bool enabled = true;

    void paint(juce::Graphics&) override;
    bool hitTest(int x, int y) override;
    void mouseUp(const juce::MouseEvent&) override;

  private:
    struct Toast
    {
        juce::String message;
        bool error = false;
        double shownAtMs = 0.0;
    };

    void timerCallback() override;
    juce::Rectangle<int> boundsFor(int indexFromBottom) const;

    std::deque<Toast> toasts;
};

} // namespace stemcel::native
