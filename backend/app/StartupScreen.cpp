#include "StartupScreen.h"

#include "Theme.h"

namespace stemcel::native
{
namespace
{
constexpr int kColumnWidth = 420;
constexpr int kButtonHeight = 36;
constexpr int kRecentRowHeight = 30;
} // namespace

StartupScreen::StartupScreen(AppController& appController, AppPreferences& preferences)
    : controller(appController), prefs(preferences)
{
    newButton.onClick = [this] { controller.newProject(); };
    openButton.onClick = [this] { controller.openProjectDialog(); };
    newButton.setColour(juce::TextButton::buttonColourId, colours::sky600);
    addAndMakeVisible(newButton);
    addAndMakeVisible(openButton);

    statusLabel.setJustificationType(juce::Justification::centred);
    statusLabel.setFont(uiFont(13.0f));
    statusLabel.setColour(juce::Label::textColourId, colours::textDim);
    addAndMakeVisible(statusLabel);

    recentHeading.setText("Recent Projects", juce::dontSendNotification);
    recentHeading.setFont(uiFont(12.0f, true));
    recentHeading.setColour(juce::Label::textColourId, colours::textFaint);
    addAndMakeVisible(recentHeading);

    refresh(false, "Starting the audio engine...");
}

void StartupScreen::refresh(bool engineReady, const juce::String& engineMessage)
{
    ready = engineReady;
    newButton.setEnabled(ready);
    openButton.setEnabled(ready);
    statusLabel.setText(engineReady ? juce::String("Start a new project or open an existing one.") : engineMessage,
                        juce::dontSendNotification);

    recentButtons.clear();
    for (const auto& recent : prefs.recentProjects())
    {
        auto* button = recentButtons.add(new juce::TextButton(
            (recent.name.isNotEmpty() ? recent.name : juce::File(recent.path).getFileNameWithoutExtension())));
        button->setTooltip(recent.path);
        button->setColour(juce::TextButton::buttonColourId, colours::zinc900);
        button->setEnabled(ready);
        const juce::File file(recent.path);
        button->onClick = [this, file] { controller.openProject(file); };
        addAndMakeVisible(button);
    }
    recentHeading.setVisible(!recentButtons.isEmpty());
    resized();
    repaint();
}

void StartupScreen::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
    auto column = getLocalBounds().withSizeKeepingCentre(kColumnWidth, getHeight());
    g.setColour(colours::zinc100);
    g.setFont(uiFont(30.0f, true));
    g.drawText("Stemcel", column.withY(getHeight() / 2 - 190).withHeight(44), juce::Justification::centred);
}

void StartupScreen::resized()
{
    auto column = getLocalBounds().withSizeKeepingCentre(kColumnWidth, getHeight());
    int y = getHeight() / 2 - 130;
    statusLabel.setBounds(column.getX(), y, kColumnWidth, 24);
    y += 36;
    const int half = (kColumnWidth - 12) / 2;
    newButton.setBounds(column.getX(), y, half, kButtonHeight);
    openButton.setBounds(column.getX() + half + 12, y, half, kButtonHeight);
    y += kButtonHeight + 28;
    recentHeading.setBounds(column.getX(), y, kColumnWidth, 20);
    y += 24;
    for (auto* button : recentButtons)
    {
        button->setBounds(column.getX(), y, kColumnWidth, kRecentRowHeight);
        y += kRecentRowHeight + 4;
    }
}

} // namespace stemcel::native
