#pragma once

#include "AppController.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace stemcel::native
{

// The strip of open projects above the workspace (AppController's tabs): click one to switch
// to it, its x to close it, + for a new mix. An unsaved tab shows a dot.
class ProjectTabStrip final : public juce::Component
{
  public:
    explicit ProjectTabStrip(AppController& controller);

    static constexpr int kHeight = 28;

    void refresh() { repaint(); }
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;

  private:
    friend class SmokeTest;
    struct Hit
    {
        juce::Rectangle<int> tab, close;
        int index = -1;
    };
    std::vector<Hit> layout() const;
    juce::Rectangle<int> plusArea() const;

    AppController& controller;
    int hovered = -1;
};

} // namespace stemcel::native
