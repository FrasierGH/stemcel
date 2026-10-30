#pragma once

#include "MainComponent.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace stemcel::native
{

class MainWindow final : public juce::DocumentWindow
{
  public:
    MainWindow(EngineClient& engine, AppPreferences& prefs, AppController& controller,
               juce::ApplicationCommandManager& commands);
    ~MainWindow() override;

    void closeButtonPressed() override;

    MainComponent& content() { return *mainComponent; }

    // Persists the window's bounds and maximised state into the preferences.
    void saveWindowState();

  private:
    AppPreferences& prefs;
    AppController& controller;
    MainComponent* mainComponent = nullptr;
};

} // namespace stemcel::native
