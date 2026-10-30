#pragma once

#include "EngineClient.h"
#include "ProjectModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace stemcel::native
{

// Library item info (the renderer's LibraryItemInfoDialog): cover art, the tags saved in the
// project media store, the audio's format and analysis, and the per-item settings — what
// kind of audio it is (music / one-shot / automatic) and its cover (hide, or choose an
// image of its own).
class LibraryInfoDialog final : public juce::Component, private juce::ChangeListener
{
  public:
    LibraryInfoDialog(EngineClient& engine, ProjectMirror& mirror, const juce::String& itemId);
    ~LibraryInfoDialog() override;
    static void show(EngineClient& engine, ProjectMirror& mirror, const juce::String& itemId);

    void paint(juce::Graphics&) override;
    void resized() override;

  private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void refresh();
    void chooseCover();
    juce::File mediaRoot() const;

    EngineClient& engine;
    ProjectMirror& mirror;
    juce::String itemId;
    juce::Image cover;
    juce::StringPairArray rows;
    juce::ComboBox audioType;
    juce::ToggleButton hideCover{"Hide cover art"};
    juce::TextButton chooseCoverButton{"Choose cover..."}, showFile{"Show file"}, closeButton{"Close"};
    std::unique_ptr<juce::FileChooser> chooser;
};

} // namespace stemcel::native
