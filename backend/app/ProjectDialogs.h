#pragma once

#include "AppPreferences.h"
#include "EngineClient.h"
#include "ProjectModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace stemcel::native
{

// Project Properties (the renderer's ProjectPropertiesDialog): name, tempo, length, sample
// rate, bar counter start and the project's own audio output.
class ProjectPropertiesDialog final : public juce::Component
{
  public:
    ProjectPropertiesDialog(EngineClient& engine, AppPreferences& prefs, const ProjectModel& project);
    static void show(EngineClient& engine, AppPreferences& prefs, const ProjectModel& project);
    void paint(juce::Graphics&) override;
    void resized() override;

  private:
    void apply();
    void close();

    EngineClient& engine;
    AppPreferences& prefs;
    juce::var original;
    juce::Label nameLabel{{}, "Name"}, bpmLabel{{}, "Tempo (BPM)"}, lengthLabel{{}, "Length (m:ss, empty = auto)"},
        rateLabel{{}, "Sample rate"}, barLabel{{}, "Bar counter starts at"}, outputLabel{{}, "Audio output"};
    juce::TextEditor name, bpm, length, barStart;
    juce::ComboBox rate, output;
    juce::StringArray outputTypes, outputDevices;
    juce::TextButton applyButton{"Apply"}, cancelButton{"Cancel"};
};

// Missing audio (the renderer's RelinkDialog): every library item whose file is gone, with
// Locate (pick the file) and Search folder (find files by name under a folder).
class RelinkDialog final : public juce::Component, private juce::ListBoxModel, private juce::ChangeListener
{
  public:
    RelinkDialog(EngineClient& engine, ProjectMirror& mirror);
    ~RelinkDialog() override;
    // Shows the dialog when the project has missing audio.
    static void showIfNeeded(EngineClient& engine, ProjectMirror& mirror);
    void paint(juce::Graphics&) override;
    void resized() override;

  private:
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics&, int width, int height, bool selected) override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void refresh();
    void locate();
    void searchFolder();
    void close();

    EngineClient& engine;
    ProjectMirror& mirror;
    juce::Label heading;
    juce::ListBox list;
    juce::TextButton locateButton{"Locate..."}, searchButton{"Search folder..."}, closeButton{"Close"};
    std::vector<std::pair<juce::String, juce::String>> missing; // item id, file path
    std::unique_ptr<juce::FileChooser> chooser;
};

// Import from Project (the renderer's ProjectImportDialog): pick another .stemcel, then
// choose which of its stems, samples and recordings to bring into this project's library.
class ProjectImportDialog final : public juce::Component, private juce::ListBoxModel
{
  public:
    ProjectImportDialog(EngineClient& engine, const juce::File& source,
                        std::function<void(const juce::String&, bool)> notify);
    // Asks for a project file, then shows the picker.
    static void chooseAndShow(EngineClient& engine, std::function<void(const juce::String&, bool)> notify);
    void paint(juce::Graphics&) override;
    void resized() override;

  private:
    struct Entry
    {
        juce::String id, name, group;
        bool chosen = true;
    };
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent&) override;
    void import();
    void close();

    EngineClient& engine;
    juce::File source;
    std::function<void(const juce::String&, bool)> notify;
    Subscriptions subscriptions;
    juce::Label heading;
    juce::ListBox list;
    juce::TextButton importButton{"Import"}, cancelButton{"Cancel"};
    std::vector<Entry> entries;
};

} // namespace stemcel::native
