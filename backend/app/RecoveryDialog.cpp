#include "RecoveryDialog.h"

#include "Theme.h"

namespace stemcel::native
{

RecoveryDialog::RecoveryDialog(AppController& appController, std::vector<Autosave::Entry> recoverable)
    : controller(appController), entries(std::move(recoverable))
{
    heading.setText("Stemcel closed with unsaved work. Restore it?", juce::dontSendNotification);
    heading.setFont(uiFont(15.0f, true));
    heading.setColour(juce::Label::textColourId, colours::text);
    addAndMakeVisible(heading);

    list.setModel(this);
    list.setRowHeight(46);
    list.setColour(juce::ListBox::backgroundColourId, colours::zinc950);
    list.selectRow(0);
    addAndMakeVisible(list);

    restoreButton.setColour(juce::TextButton::buttonColourId, colours::sky600);
    restoreButton.onClick = [this] { restoreSelected(); };
    discardButton.onClick = [this] { discardSelected(); };
    laterButton.onClick = [this] { close(); };
    for (auto* b : {&restoreButton, &discardButton, &laterButton}) addAndMakeVisible(b);
    setSize(560, 320);
}

void RecoveryDialog::showIfNeeded(AppController& controller)
{
    auto entries = Autosave::listRecoverable();
    if (entries.empty()) return;
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(new RecoveryDialog(controller, std::move(entries)));
    options.dialogTitle = "Recover unsaved work";
    options.dialogBackgroundColour = colours::background;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}

void RecoveryDialog::close()
{
    if (auto* window = findParentComponentOfClass<juce::DialogWindow>()) window->exitModalState(0);
}

void RecoveryDialog::restoreSelected()
{
    const int row = list.getSelectedRow();
    if (row < 0 || row >= static_cast<int>(entries.size())) return;
    const auto entry = entries[static_cast<size_t>(row)];
    // Through the unsaved-changes guard, like Open.
    auto& c = controller;
    close();
    c.guardUnsavedChanges([&c, entry] { c.restoreAutosave(entry); });
}

void RecoveryDialog::discardSelected()
{
    const int row = list.getSelectedRow();
    if (row < 0 || row >= static_cast<int>(entries.size())) return;
    Autosave::clear(entries[static_cast<size_t>(row)].projectId);
    entries.erase(entries.begin() + row);
    if (entries.empty())
    {
        close();
        return;
    }
    list.updateContent();
    list.selectRow(juce::jmin(row, static_cast<int>(entries.size()) - 1));
}

int RecoveryDialog::getNumRows()
{
    return static_cast<int>(entries.size());
}

void RecoveryDialog::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= static_cast<int>(entries.size())) return;
    const auto& e = entries[static_cast<size_t>(row)];
    if (selected) g.fillAll(colours::sky600.withAlpha(0.35f));
    g.setColour(colours::text);
    g.setFont(uiFont(14.0f, true));
    g.drawText(e.projectName, 10, 4, width - 20, 20, juce::Justification::centredLeft, true);
    const auto when = juce::Time::fromISO8601(e.savedAtIso);
    const auto original = e.originalPath.isEmpty() ? juce::String("Untitled (never saved)")
                          : e.originalExists       ? e.originalPath
                                                   : e.originalPath + " (file missing)";
    g.setColour(colours::textDim);
    g.setFont(uiFont(12.0f));
    g.drawText(when.toString(true, true, false) + "  -  " + original, 10, 24, width - 20, 18,
               juce::Justification::centredLeft, true);
}

void RecoveryDialog::listBoxItemDoubleClicked(int, const juce::MouseEvent&)
{
    restoreSelected();
}

void RecoveryDialog::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
}

void RecoveryDialog::resized()
{
    auto area = getLocalBounds().reduced(14);
    heading.setBounds(area.removeFromTop(28));
    area.removeFromTop(6);
    auto buttons = area.removeFromBottom(32);
    area.removeFromBottom(10);
    list.setBounds(area);
    restoreButton.setBounds(buttons.removeFromRight(100));
    buttons.removeFromRight(8);
    discardButton.setBounds(buttons.removeFromRight(100));
    laterButton.setBounds(buttons.removeFromLeft(100));
}

} // namespace stemcel::native
