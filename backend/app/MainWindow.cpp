#include "MainWindow.h"

#include "Theme.h"

namespace stemcel::native
{
namespace
{
constexpr int kMinWidth = 900;
constexpr int kMinHeight = 600;
} // namespace

MainWindow::MainWindow(EngineClient& engine, AppPreferences& preferences, AppController& appController,
                       juce::ApplicationCommandManager& commands)
    : juce::DocumentWindow("Stemcel", colours::background, juce::DocumentWindow::allButtons),
      prefs(preferences), controller(appController)
{
    setUsingNativeTitleBar(true);
    mainComponent = new MainComponent(engine, preferences, appController, commands);
    setContentOwned(mainComponent, false);

#if JUCE_MAC
    const auto appMenuExtras = mainComponent->macAppMenuExtras();
    juce::MenuBarModel::setMacMainMenu(mainComponent, &appMenuExtras);
#else
    setMenuBar(mainComponent);
#endif
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
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(nullptr);
#else
    setMenuBar(nullptr);
#endif
    clearContentComponent();
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
