#include "MainWindow.h"

#include "PluginEditorWindow.h"
#include "Theme.h"

namespace stemcel::native
{
namespace
{
constexpr int kMinWidth = 900;
constexpr int kMinHeight = 600;
} // namespace

MainWindow::MainWindow(EngineClient& engineClient, AppPreferences& preferences, AppController& appController,
                       juce::ApplicationCommandManager& commandManager)
    : juce::DocumentWindow("Stemcel", colours::background, juce::DocumentWindow::allButtons),
      engine(engineClient), commands(commandManager), prefs(preferences), controller(appController)
{
    setUsingNativeTitleBar(true);
    // Plugin editors stay in front of this window when it is clicked.
    plugins::PluginEditorWindow::setOwner(this);
    mainComponent = new MainComponent(engine, preferences, appController, commands);
    setContentOwned(mainComponent, false);
    attachMenu();
    addKeyListener(commands.getKeyMappings());

    setResizable(true, false);
    setResizeLimits(kMinWidth, kMinHeight, 100000, 100000);

    const auto saved = prefs.windowBounds();
    const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect(saved);
    if (saved.getX() == 0 && saved.getY() == 0)
        centreWithSize(saved.getWidth(), saved.getHeight());
    else if (display != nullptr && display->userArea.intersects(saved))
        setBounds(saved.constrainedWithin(display->userArea));
    else
        centreWithSize(saved.getWidth(), saved.getHeight());

    setVisible(true);
    if (prefs.windowMaximised()) setFullScreen(true);
}

MainWindow::~MainWindow()
{
    plugins::PluginEditorWindow::setOwner(nullptr);
    detachMenu();
    clearContentComponent();
}

void MainWindow::attachMenu()
{
#if JUCE_MAC
    const auto appMenuExtras = mainComponent->macAppMenuExtras();
    juce::MenuBarModel::setMacMainMenu(mainComponent, &appMenuExtras);
#else
    setMenuBar(mainComponent);
#endif
}

void MainWindow::detachMenu()
{
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(nullptr);
#else
    setMenuBar(nullptr);
#endif
}

void MainWindow::rebuildContent()
{
    // The open project's zoom and scroll go into it first, so the new timeline opens on them.
    if (controller.saveViewState) controller.saveViewState();
    const auto status = mainComponent->status();
    // The old content goes first: its destructor lets go of the controller's callbacks,
    // which the new one then takes.
    detachMenu();
    clearContentComponent();
    mainComponent = new MainComponent(engine, prefs, controller, commands, status);
    setContentOwned(mainComponent, false);
    attachMenu();
    setBackgroundColour(colours::background);
    repaint();
}

void MainWindow::saveWindowState()
{
    // While full-screen, keep the last normal bounds so the window un-maximises to them.
    if (isFullScreen())
        prefs.set("window", "maximized", true);
    else
        prefs.setWindowState(getBounds(), false);
    prefs.save();
}

void MainWindow::closeButtonPressed()
{
    controller.requestQuit();
}

} // namespace stemcel::native
