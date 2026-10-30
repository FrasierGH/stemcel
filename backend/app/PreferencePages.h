#pragma once

#include "AppPreferences.h"
#include "EngineClient.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <deque>

namespace stemcel::native
{

// Pushes the engine-side preferences the renderer sends on connect: brake and backspin
// feel, enabled MIDI inputs with their deck selections and crossfader direction.
void pushEnginePreferences(EngineClient& engine, AppPreferences& prefs);

// Preferences > General (PreferencesGeneralTab.vue): waveform display and notifications.
class GeneralPreferencesPage final : public juce::Component
{
  public:
    explicit GeneralPreferencesPage(AppPreferences& prefs);
    void resized() override;

  private:
    AppPreferences& prefs;
    juce::Label waveLabel{{}, "Waveforms"};
    juce::ComboBox waveMode;
    juce::ToggleButton toasts{"Show toast notifications"};
};

// Preferences > Timeline (PreferencesTimelineTab.vue): follow, tempo seed and auto-warp,
// beat-grid alignment after analysis, and what the skip buttons jump to.
class TimelinePreferencesPage final : public juce::Component
{
  public:
    explicit TimelinePreferencesPage(AppPreferences& prefs);
    void resized() override;

  private:
    AppPreferences& prefs;
    juce::ToggleButton follow{"Follow playback"}, seedTempo{"Set project tempo from first clip"},
        matchTempo{"Auto-warp clips to project tempo"}, align{"Align clips to the beat grid after analysis"};
    juce::Label skipLabel{{}, "Skip buttons"};
    juce::ComboBox skipTarget;
};

// Preferences > Project (PreferencesProjectTab.vue): default folders, autosave and the
// clean-up of generated files when they are removed from the library.
class ProjectPreferencesPage final : public juce::Component
{
  public:
    explicit ProjectPreferencesPage(AppPreferences& prefs);
    void resized() override;

  private:
    void chooseFolder(const char* key, juce::Label& label, const juce::String& title);
    AppPreferences& prefs;
    juce::Label pathsHeading{{}, "Default paths"}, projectLabel{{}, "Project folder"}, projectFolder,
        clipLabel{{}, "Clip folder"}, clipFolder, autosaveHeading{{}, "Autosave"}, intervalLabel{{}, "Save every (s)"},
        cleanupHeading{{}, "File cleanup"}, cleanupHint;
    juce::TextButton projectChange{"Change..."}, clipChange{"Change..."};
    juce::ToggleButton autosave{"Keep a recovery copy in the background"}, cleanup{"Clean up project files on remove"};
    juce::ToggleButton saveProjectFile{"Save the project itself every 3 minutes (once it has a file)"};
    juce::Slider interval;
    std::unique_ptr<juce::FileChooser> chooser;
};

// Preferences > Effects: the turntable brake and backspin feel.
class EffectsPreferencesPage final : public juce::Component
{
  public:
    EffectsPreferencesPage(EngineClient& engine, AppPreferences& prefs);
    void resized() override;

  private:
    void apply();
    EngineClient& engine;
    AppPreferences& prefs;
    juce::Label brakeLabel{{}, "Brake"}, backspinLabel{{}, "Backspin"};
    juce::ComboBox brakeDuration, brakeCurve, spinDuration, spinIntensity;
};

// Preferences > Stems: GPU, backup model, enhancement passes and model downloads.
class StemsPreferencesPage final : public juce::Component
{
  public:
    explicit StemsPreferencesPage(AppPreferences& prefs);
    void resized() override;

  private:
    AppPreferences& prefs;
    juce::ToggleButton gpu{"Use GPU acceleration when available"}, backup{"Always use the backup model"};
    juce::OwnedArray<juce::ToggleButton> enhance;
    juce::OwnedArray<juce::ComboBox> strength;
    juce::OwnedArray<juce::Component> models;
};

// Preferences > Developer: full-session diagnostic logging (applies from the next launch).
class DeveloperPreferencesPage final : public juce::Component
{
  public:
    explicit DeveloperPreferencesPage(AppPreferences& prefs);
    void resized() override;

  private:
    AppPreferences& prefs;
    juce::ToggleButton logging{"Write a full diagnostic log every session (from the next launch)"};
    juce::Label folder;
    juce::TextButton open{"Open log folder"};
};

// Preferences > MIDI: controllers (enable, crossfader direction, default deck) and a
// monitor of incoming messages.
class MidiPreferencesPage final : public juce::Component, private juce::ListBoxModel
{
  public:
    MidiPreferencesPage(EngineClient& engine, AppPreferences& prefs);
    void resized() override;

  private:
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;
    void devices(const juce::var& list);
    void setDevicePref(const juce::String& id, const juce::String& key, const juce::String& value);

    EngineClient& engine;
    AppPreferences& prefs;
    Subscriptions subscriptions;
    juce::ListBox deviceList;
    juce::TextButton rescan{"Rescan"};
    juce::Label monitorLabel{{}, "Monitor"};
    juce::TextEditor monitor;
    juce::var inputs;
    std::deque<juce::String> messages;
};

} // namespace stemcel::native
