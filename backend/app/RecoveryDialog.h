#pragma once

#include "AppController.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace stemcel::native
{

// Offered at startup when autosaves are newer than their projects (the renderer's
// RecoveryDialog): restore one, or discard each.
class RecoveryDialog final : public juce::Component, private juce::ListBoxModel
{
  public:
    RecoveryDialog(AppController& controller, std::vector<Autosave::Entry> entries);

    // Shows the dialog when there is anything to recover.
    static void showIfNeeded(AppController& controller);

    void paint(juce::Graphics&) override;
    void resized() override;

  private:
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
    void close();
    void restoreSelected();
    void discardSelected();

    AppController& controller;
    std::vector<Autosave::Entry> entries;
    juce::Label heading;
    juce::ListBox list;
    juce::TextButton restoreButton{"Restore"}, discardButton{"Discard"}, laterButton{"Not now"};
};

} // namespace stemcel::native
