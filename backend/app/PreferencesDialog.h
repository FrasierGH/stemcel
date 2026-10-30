#pragma once

#include "AppPreferences.h"
#include "EngineClient.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace stemcel::native
{

// Preferences ▸ Audio: driver type, output device, keep-awake, and what the engine opened.
// The renderer's PreferencesAudioTab, talking to the engine with the same commands.
class AudioPreferencesPage final : public juce::Component
{
  public:
    AudioPreferencesPage(EngineClient& engine, AppPreferences& prefs);

    void resized() override;

  private:
    void applyDeviceList(const juce::var& list);
    void selectionChanged();

    EngineClient& engine;
    AppPreferences& prefs;
    Subscriptions subscriptions;

    juce::Label driverLabel{{}, "Driver"};
    juce::ComboBox driverBox;
    juce::Label deviceLabel{{}, "Output device"};
    juce::ComboBox deviceBox;
    juce::ToggleButton keepAwake{"Keep this device awake (stops sleep-prone DACs muting the first notes)"};
    juce::Label infoLabel;
    // New projects' sample rate until File > Project Properties sets one (PreferencesAudioTab.vue).
    juce::Label rateLabel{{}, "Default project sample rate"};
    juce::ComboBox rateBox;

    juce::var deviceList;
    bool updating = false;
};

// The Preferences window. Tabs beyond Audio arrive with the features they configure.
class PreferencesDialog final : public juce::DialogWindow
{
  public:
    PreferencesDialog(EngineClient& engine, AppPreferences& prefs);

    void closeButtonPressed() override;

    static void show(EngineClient& engine, AppPreferences& prefs);
};

} // namespace stemcel::native
