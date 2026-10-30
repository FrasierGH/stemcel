#pragma once

#include "AppController.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace stemcel::native
{

// The landing page shown before a project is open (the renderer's StartupScreen): New
// Project, Open Project…, and the recent-projects list.
class StartupScreen final : public juce::Component
{
  public:
    StartupScreen(AppController& controller, AppPreferences& prefs);

    // Re-reads the recent projects and the engine's readiness.
    void refresh(bool engineReady, const juce::String& engineMessage);

    void paint(juce::Graphics&) override;
    void resized() override;

  private:
    AppController& controller;
    AppPreferences& prefs;

    juce::TextButton newButton{"New Project"};
    juce::TextButton openButton{"Open Project..."};
    juce::Label statusLabel;
    juce::Label recentHeading;
    juce::OwnedArray<juce::TextButton> recentButtons;
    bool ready = false;
};

} // namespace stemcel::native
