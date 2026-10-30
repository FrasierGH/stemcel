#include "ProjectTabs.h"

#include "Theme.h"

namespace stemcel::native
{
namespace
{
constexpr int kTabWidth = 180;
constexpr int kCloseSize = 16;
} // namespace

ProjectTabStrip::ProjectTabStrip(AppController& c) : controller(c)
{
}

std::vector<ProjectTabStrip::Hit> ProjectTabStrip::layout() const
{
    std::vector<Hit> hits;
    const auto& tabs = controller.projectTabs();
    const int width = juce::jmin(kTabWidth, tabs.empty() ? kTabWidth : (getWidth() - 40) / static_cast<int>(tabs.size()));
    for (int i = 0; i < static_cast<int>(tabs.size()); ++i)
    {
        Hit h;
        h.index = i;
        h.tab = {4 + i * width, 3, width - 2, getHeight() - 3};
        h.close = h.tab.withLeft(h.tab.getRight() - kCloseSize - 6).withSizeKeepingCentre(kCloseSize, kCloseSize);
        hits.push_back(h);
    }
    return hits;
}

juce::Rectangle<int> ProjectTabStrip::plusArea() const
{
    const auto hits = layout();
    const int x = hits.empty() ? 4 : hits.back().tab.getRight() + 6;
    return {x, 4, 24, getHeight() - 6};
}

void ProjectTabStrip::paint(juce::Graphics& g)
{
    g.fillAll(colours::zinc950);
    const auto& tabs = controller.projectTabs();
    g.setFont(uiFont(12.0f, true));
    for (const auto& h : layout())
    {
        const auto& tab = tabs[static_cast<size_t>(h.index)];
        const bool active = h.index == controller.activeTabIndex();
        g.setColour(active ? colours::zinc800 : (h.index == hovered ? colours::zinc900.brighter(0.05f) : colours::zinc900));
        g.fillRoundedRectangle(h.tab.toFloat().withTrimmedBottom(-4.0f), 4.0f);
        if (active)
        {
            g.setColour(colours::accent);
            g.fillRect(h.tab.withHeight(2));
        }
        g.setColour(active ? colours::zinc100 : colours::zinc400);
        const auto label = (tab.dirty ? juce::String(juce::CharPointer_UTF8("\xe2\x80\xa2 ")) : juce::String()) + tab.name;
        g.drawText(label, h.tab.withTrimmedLeft(10).withTrimmedRight(kCloseSize + 10), juce::Justification::centredLeft, true);
        g.setColour(h.index == hovered || active ? colours::zinc400 : colours::zinc600);
        g.drawText(juce::String(juce::CharPointer_UTF8("\xc3\x97")), h.close, juce::Justification::centred);
    }
    g.setColour(colours::zinc400);
    g.setFont(uiFont(16.0f, true));
    g.drawText("+", plusArea(), juce::Justification::centred);
}

void ProjectTabStrip::mouseDown(const juce::MouseEvent& e)
{
    if (plusArea().contains(e.getPosition()))
    {
        controller.newProject();
        return;
    }
    for (const auto& h : layout())
    {
        if (h.close.contains(e.getPosition()))
        {
            controller.closeTab(h.index);
            return;
        }
        if (h.tab.contains(e.getPosition()))
        {
            controller.switchToTab(h.index);
            return;
        }
    }
}

void ProjectTabStrip::mouseMove(const juce::MouseEvent& e)
{
    int next = -1;
    for (const auto& h : layout())
        if (h.tab.contains(e.getPosition())) next = h.index;
    if (next != hovered)
    {
        hovered = next;
        repaint();
    }
}

void ProjectTabStrip::mouseExit(const juce::MouseEvent&)
{
    hovered = -1;
    repaint();
}

} // namespace stemcel::native
