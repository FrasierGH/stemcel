// Entry point of the native Stemcel app: the JUCE UI and the audio engine in one process.

#include "AppController.h"
#include "AppPaths.h"
#include "AppPreferences.h"
#include "CrashHandler.h"
#include "EngineClient.h"
#include "Log.h"
#include "MainWindow.h"
#include "DebugAssertHook.h"
#include "PreferencePages.h"
#include "PluginScanWorker.h"
#include "SmokeTest.h"
#include "Theme.h"
#include "Version.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <utility>

namespace stemcel::native
{

class StemcelApplication final : public juce::JUCEApplication
{
  public:
    const juce::String getApplicationName() override { return "Stemcel"; }
    const juce::String getApplicationVersion() override { return kBackendVersion; }

    // One instance per user, except the plugin-scan child (this same executable relaunched
    // by the scan coordinator) and a smoke test, which works in its own folder and must not
    // hand itself to an app the user has open.
    bool moreThanOneInstanceAllowed() override
    {
        const auto commandLine = getCommandLineParameters();
        return plugins::isScanWorkerCommandLine(commandLine) || commandLine.contains("--smoke-test");
    }

    void initialise(const juce::String& commandLine) override
    {
        if (plugins::isScanWorkerCommandLine(commandLine))
        {
            scanWorker = plugins::startScanWorker(commandLine);
            if (scanWorker == nullptr) quit();
            return;
        }

        // A smoke test keeps the whole session in the log, so a failure can be read afterwards.
        const bool smoke = commandLine.contains("--smoke-test");
        const auto home = juce::File::getSpecialLocation(juce::File::userHomeDirectory);
        setAppFolderName("Stemcel");

        const auto diagnostics = home.getChildFile(appFolderName()).getChildFile("Diagnostics");
        crash::install(diagnostics.getFullPathName());
        crash::setPhase("startup");
        // Developer > full-session logging, read straight from the shared preferences file.
        const bool fullLog = smoke
                             || static_cast<bool>(juce::JSON::parse(juce::File(AppPreferences().file()))
                                                      .getProperty("debug", {})
                                                      .getProperty("loggingEnabled", false));
        log::initialise(diagnostics.getFullPathName(), log::Level::Info, /*truncate*/ true, /*startupOnly*/ !fullLog);
        installDebugAssertHook();
        log::info("main", juce::String("Stemcel native v") + kBackendVersion + " - "
                              + juce::SystemStats::getOperatingSystemName());

        prefs = std::make_unique<AppPreferences>();
        prefs->load();

        // The theme is set before anything is drawn (Preferences > General; STEMCEL_THEME
        // overrides it, which is how a smoke test runs under another theme).
        const auto themeOverride = juce::SystemStats::getEnvironmentVariable("STEMCEL_THEME", {});
        applyTheme(themeFromName(themeOverride.isNotEmpty() || smoke ? themeOverride
                                                                     : prefs->get("ui", "theme", "terminal").toString()));
        lookAndFeel.applyColours();
        juce::LookAndFeel::setDefaultLookAndFeel(&lookAndFeel);

        EngineHost::Options options;
        options.preferredDeviceType = prefs->outputTypeName();
        options.preferredDeviceName = prefs->outputDeviceName();
        options.keepAwake = prefs->keepAwakeFor(options.preferredDeviceName);
        engine = std::make_unique<EngineClient>(options);
        controller = std::make_unique<AppController>(*engine, *prefs);

        commands.registerAllCommandsForTarget(this);
        mainWindow = std::make_unique<MainWindow>(*engine, *prefs, *controller, commands);

        // Opening the audio device blocks the message thread; let the window paint first.
        juce::MessageManager::callAsync(
            [this]
            {
                crash::setPhase("audio-engine-start");
                engine->start();
                applyStartupSettings();
                crash::setPhase("running");
                log::markStartupComplete();
                started = true;
                if (juce::File smokeDir; SmokeTest::isRequested(getCommandLineParameters(), smokeDir))
                {
                    smokeTest = std::make_unique<SmokeTest>(*engine, smokeDir);
                    smokeTest->openInUi = [this](const juce::File& file) { controller->openProject(file); };
                    smokeTest->start();
                    return;
                }
                openFromCommandLine(getCommandLineParameters());
                if (pendingOpen.isNotEmpty()) openFromCommandLine(std::exchange(pendingOpen, {}));
            });
    }

    void shutdown() override
    {
        if (scanWorker != nullptr)
        {
            scanWorker.reset();
            return;
        }
        log::info("main", "shutdown: smoke test");
        smokeTest.reset();
        if (mainWindow != nullptr) mainWindow->saveWindowState();
        log::info("main", "shutdown: window");
        mainWindow.reset();
        log::info("main", "shutdown: controller");
        controller.reset();
        log::info("main", "shutdown: engine");
        engine.reset();
        log::info("main", "shutdown: done");
        if (prefs != nullptr) prefs->save();
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
        log::shutdown();
    }

    void systemRequestedQuit() override
    {
        if (controller != nullptr) controller->requestQuit();
        else quit();
    }

    // A second launch (e.g. double-clicking a .stemcel file) hands its arguments here. On
    // macOS a file opened from Finder arrives here too, possibly while the app is still
    // launching and before the engine has started; it is opened once startup completes.
    void anotherInstanceStarted(const juce::String& commandLine) override
    {
        if (controller == nullptr) return;
        if (!started)
        {
            pendingOpen = commandLine;
            return;
        }
        openFromCommandLine(commandLine);
        if (mainWindow != nullptr) mainWindow->toFront(true);
    }

  private:
    // Settings the Electron renderer pushes when it connects, applied from preferences.
    void applyStartupSettings()
    {
        auto* seed = new juce::DynamicObject();
        seed->setProperty("enabled", prefs->seedTempoFromFirstClip());
        engine->send("PROJECT_SET_SEED_TEMPO_PREF", juce::var(seed));
        // The other settings the renderer pushes on connect (brake/backspin, MIDI).
        pushEnginePreferences(*engine, *prefs);
    }

    void openFromCommandLine(const juce::String& commandLine)
    {
        for (auto token : juce::StringArray::fromTokens(commandLine, true))
        {
            const juce::File file(token.unquoted());
            if (file.hasFileExtension("stemcel") && file.existsAsFile())
            {
                controller->guardUnsavedChanges([this, file] { controller->openProject(file); });
                return;
            }
        }
    }

    StemcelLookAndFeel lookAndFeel;
    juce::ApplicationCommandManager commands;
    std::unique_ptr<AppPreferences> prefs;
    std::unique_ptr<EngineClient> engine;
    std::unique_ptr<AppController> controller;
    std::unique_ptr<MainWindow> mainWindow;
    bool started = false;
    juce::String pendingOpen;
    std::unique_ptr<juce::ChildProcessWorker> scanWorker;
    std::unique_ptr<SmokeTest> smokeTest;
};

} // namespace stemcel::native

START_JUCE_APPLICATION(stemcel::native::StemcelApplication)
