#include "Toasts.h"

#include "Log.h"

#include "Theme.h"

namespace stemcel::native
{
namespace
{
constexpr int kWidth = 340;
constexpr int kHeight = 48;
constexpr int kGap = 8;
constexpr int kMargin = 16;
constexpr double kLifetimeMs = 4500.0;
constexpr size_t kMaxVisible = 4;
} // namespace

Toasts::Toasts()
{
    setInterceptsMouseClicks(true, false);
}

void Toasts::show(const juce::String& message, bool isError)
{
    log::info("notify", (isError ? "error: " : "info: ") + message);
    if (!enabled) return;
    toasts.push_back({message, isError, juce::Time::getMillisecondCounterHiRes()});
    while (toasts.size() > kMaxVisible) toasts.pop_front();
    startTimerHz(10);
    repaint();
}

juce::Rectangle<int> Toasts::boundsFor(int indexFromBottom) const
{
    return {getWidth() - kWidth - kMargin, getHeight() - kMargin - (indexFromBottom + 1) * kHeight - indexFromBottom * kGap,
            kWidth, kHeight};
}

void Toasts::paint(juce::Graphics& g)
{
    int index = 0;
    for (auto it = toasts.rbegin(); it != toasts.rend(); ++it, ++index)
    {
        const auto area = boundsFor(index).toFloat();
        g.setColour(colours::zinc800);
        g.fillRoundedRectangle(area, 6.0f);
        g.setColour(it->error ? colours::red500 : colours::zinc700);
        g.drawRoundedRectangle(area.reduced(0.5f), 6.0f, 1.0f);
        g.setColour(it->error ? colours::red500 : colours::accent);
        g.fillRect(area.withWidth(3.0f).reduced(0.0f, 8.0f));
        g.setColour(colours::text);
        g.setFont(uiFont(13.0f));
        g.drawFittedText(it->message, area.reduced(14.0f, 6.0f).toNearestInt(), juce::Justification::centredLeft, 2);
    }
}

bool Toasts::hitTest(int x, int y)
{
    for (int i = 0; i < static_cast<int>(toasts.size()); ++i)
        if (boundsFor(i).contains(x, y)) return true;
    return false;
}

void Toasts::mouseUp(const juce::MouseEvent& e)
{
    for (int i = 0; i < static_cast<int>(toasts.size()); ++i)
    {
        if (boundsFor(i).contains(e.getPosition()))
        {
            toasts.erase(toasts.end() - 1 - i);
            repaint();
            return;
        }
    }
}

void Toasts::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto before = toasts.size();
    while (!toasts.empty() && now - toasts.front().shownAtMs > kLifetimeMs) toasts.pop_front();
    if (toasts.size() != before) repaint();
    if (toasts.empty()) stopTimer();
}

} // namespace stemcel::native
