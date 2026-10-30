#include "AppPaths.h"
#include "AudioEngine.h"
#include "AudioDeviceCommands.h"
#include "BridgeDispatch.h"
#include "BridgeServer.h"
#include "CrashHandler.h"
#include "DecodedCache.h"
#include "EditUndoState.h"
#include "EngineHost.h"
#include "Log.h"
#include "NativeApplication.h"
#include "PeakJobCoordinator.h"
#include "PeaksCache.h"
#include "PlayheadEmitter.h"
#include "PluginScanWorker.h"
#include "ProjectSession.h"
#include "ProjectState.h"
#include "Version.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <iostream>
#include <juce_events/juce_events.h>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>

// Entry point: bridge I/O and audio threads marshal engine mutations onto the JUCE message thread.
// NOTE: keep string literals ASCII-only; use juce::CharPointer_UTF8 for Unicode.

namespace
{
constexpr int kMinBridgePort = 1024;
constexpr int kMaxBridgePort = 65535;

std::atomic<bool> g_shouldQuit{false};

void onSignal(int /*sig*/)
{
    g_shouldQuit.store(true);
    juce::MessageManager::getInstance()->stopDispatchLoop();
}

// Strict port parsing keeps invalid dynamic-port setup debuggable.
int parsePort(std::string_view value, std::string_view source)
{
    if (value.empty())
    {
        stemcel::log::warn("main",
                             juce::String("empty port value from ") + juce::String(std::string(source)));
        return -1;
    }

    int port = 0;
    try
    {
        std::size_t consumed = 0;
        port = std::stoi(std::string(value), &consumed);
        if (consumed != value.size())
        {
            throw std::invalid_argument("trailing characters");
        }
    }
    catch (const std::exception& e)
    {
        stemcel::log::warn("main",
                             juce::String("invalid port from ") + juce::String(std::string(source)) + " (" +
                                 juce::String(std::string(value)) + "): " + juce::String(e.what()));
        return -1;
    }

    if (port < kMinBridgePort || port > kMaxBridgePort)
    {
        stemcel::log::warn("main",
                             juce::String("port ") + juce::String(port) + " from " +
                                 juce::String(std::string(source)) + " outside [" +
                                 juce::String(kMinBridgePort) + ", " + juce::String(kMaxBridgePort) + "]");
        return -1;
    }

    return port;
}

// Electron owns dynamic port selection; missing/invalid `--port` is a startup error.
// `argv` must remain a C-style array for main-compatible helpers.
// NOLINTNEXTLINE(modernize-avoid-c-arrays,hicpp-avoid-c-arrays,cppcoreguidelines-avoid-c-arrays)
int resolveBridgePort(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg{argv[i]};
        if (arg == "--port" && i + 1 < argc)
        {
            return parsePort(argv[i + 1], "--port");
        }
        constexpr std::string_view prefix = "--port=";
        if (arg.size() > prefix.size() && arg.substr(0, prefix.size()) == prefix)
        {
            return parsePort(arg.substr(prefix.size()), "--port=");
        }
    }

    stemcel::log::error("main",
                          juce::String("missing required --port <N> argument (range [") +
                              juce::String(kMinBridgePort) + ", " + juce::String(kMaxBridgePort) +
                              "]); refusing to start");
    return -1;
}

// Prefer Electron's per-session env token; CLI token exists only for manual backend testing.
// `argv` must remain a C-style array for main-compatible helpers.
// NOLINTNEXTLINE(modernize-avoid-c-arrays,hicpp-avoid-c-arrays,cppcoreguidelines-avoid-c-arrays)
juce::String resolveBridgeToken(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg{argv[i]};
        if (arg == "--token" && i + 1 < argc)
        {
            return juce::String{argv[i + 1]};
        }
        constexpr std::string_view prefix = "--token=";
        if (arg.size() > prefix.size() && arg.substr(0, prefix.size()) == prefix)
        {
            return juce::String{std::string(arg.substr(prefix.size()))};
        }
    }

    return juce::SystemStats::getEnvironmentVariable("STEMCEL_BRIDGE_TOKEN", {});
}


// `argv` must remain a C-style array for main-compatible helpers.
// NOLINTNEXTLINE(modernize-avoid-c-arrays,hicpp-avoid-c-arrays,cppcoreguidelines-avoid-c-arrays)
int runBackend(int argc, char* argv[])
{
    // Two log sinks, both resolved up front:
    //  - STEMCEL_LOG_DIR: opt-in verbose session log (all levels), only set when
    //    the user enables diagnostic logging in Preferences.
    //  - STEMCEL_DIAG_DIR: an ALWAYS-ON diagnostics dir the frontend passes on
    //    every launch. Used for the crash report and, when verbose logging is off,
    //    for an INFO-level startup log — so a failed/crashed startup on a machine
    //    we can't attach to still leaves an easy-to-find trace regardless of the
    //    user's logging preference.
    const auto logDirOverride = juce::SystemStats::getEnvironmentVariable("STEMCEL_LOG_DIR", {});
    const auto diagDir = juce::SystemStats::getEnvironmentVariable("STEMCEL_DIAG_DIR", {});

    // Install the crash reporter first, so a fault anywhere below is captured.
    stemcel::crash::install(diagDir.isNotEmpty() ? diagDir : logDirOverride);
    stemcel::crash::setPhase("startup");

    if (logDirOverride.isNotEmpty())
    {
        stemcel::log::initialise(logDirOverride);
    }
    else if (diagDir.isNotEmpty())
    {
        // Always-on but startup-scoped: INFO+ (no per-frame DEBUG), truncated each
        // launch, and closed once startup succeeds (see markStartupComplete). It
        // exists only to diagnose a backend that can't start — ongoing session
        // logging is the opt-in verbose sink's job.
        stemcel::log::initialise(diagDir, stemcel::log::Level::Info, /*truncate*/ true,
                                   /*startupOnly*/ true);
    }

    const juce::String banner = juce::String("Stemcel Backend v") + stemcel::kBackendVersion + " - " +
                                juce::SystemStats::getOperatingSystemName() + " (" +
                                juce::SystemStats::getCpuVendor() + ")";
    std::cout << banner.toStdString() << '\n';
    stemcel::log::info("main", banner);

    const int bridgePort = resolveBridgePort(argc, argv);
    if (bridgePort < 0)
    {
        // Also print to stderr because file logging may be disabled.
        std::cerr << "[main] missing or invalid --port; expected: " << argv[0]
                  << " --port <" << kMinBridgePort << "-" << kMaxBridgePort << ">\n";
        return 2;
    }
    const juce::String bridgeToken = resolveBridgeToken(argc, argv);

    // Verbose, always-on startup diagnostics: enough to triage a slow or failed launch
    // from the log alone (no repro). One-shot, INFO-level; the diag log is startup-scoped.
    stemcel::log::info("sys",
                         "JUCE " + juce::SystemStats::getJUCEVersion() + " | OS "
                             + juce::SystemStats::getOperatingSystemName()
                             + (juce::SystemStats::isOperatingSystem64Bit() ? " (64-bit)" : " (32-bit)"));
    stemcel::log::info("sys",
                         "CPU " + juce::SystemStats::getCpuModel() + " ["
                             + juce::SystemStats::getCpuVendor() + "] "
                             + juce::String(juce::SystemStats::getNumCpus()) + " logical / "
                             + juce::String(juce::SystemStats::getNumPhysicalCpus()) + " physical @ "
                             + juce::String(juce::SystemStats::getCpuSpeedInMegahertz()) + " MHz | RAM "
                             + juce::String(juce::SystemStats::getMemorySizeInMegabytes()) + " MB");
    // CPU SIMD feature flags: the ONNX Runtime (stem separation) and the DSP paths pick
    // code paths off these, so a "why is separation slow / crashing here" report is far
    // easier to triage when the log shows what the machine actually supports.
    const auto cpuFlag = [](bool present, const char* name) -> juce::String
    { return present ? (juce::String(" ") + name) : juce::String(); };
    stemcel::log::info("sys",
                         "CPU features:" + cpuFlag(juce::SystemStats::hasSSE(), "SSE")
                             + cpuFlag(juce::SystemStats::hasSSE2(), "SSE2")
                             + cpuFlag(juce::SystemStats::hasSSE3(), "SSE3")
                             + cpuFlag(juce::SystemStats::hasSSSE3(), "SSSE3")
                             + cpuFlag(juce::SystemStats::hasSSE41(), "SSE4.1")
                             + cpuFlag(juce::SystemStats::hasSSE42(), "SSE4.2")
                             + cpuFlag(juce::SystemStats::hasAVX(), "AVX")
                             + cpuFlag(juce::SystemStats::hasAVX2(), "AVX2")
                             + cpuFlag(juce::SystemStats::hasAVX512F(), "AVX512F")
                             + cpuFlag(juce::SystemStats::hasFMA3(), "FMA3"));
    // Locale — useful for triaging text/encoding/number-format issues, and not
    // personally identifiable (deliberately no computer/host name is logged, as that
    // can carry the user's name).
    stemcel::log::info("sys",
                         "Locale " + juce::SystemStats::getUserLanguage() + "-"
                             + juce::SystemStats::getUserRegion());
    // Free/total disk on the volumes that matter: the temp workspace (unsaved projects,
    // decoded-audio cache, in-flight stems/channels/samples) and the install volume. An
    // "out of space" failure mid-export/import is otherwise invisible in the log.
    const auto tempDir = stemcel::systemTempDirectory();
    const auto exeFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    const auto diskGb = [](juce::int64 bytes) -> juce::String
    { return bytes >= 0 ? juce::String(static_cast<double>(bytes) / 1.0e9, 1) : juce::String("?"); };
    stemcel::log::info("sys",
                         "Disk temp '" + tempDir.getFullPathName() + "' free " + diskGb(tempDir.getBytesFreeOnVolume())
                             + " / " + diskGb(tempDir.getVolumeTotalSize()) + " GB | install '"
                             + exeFile.getParentDirectory().getFullPathName() + "' free "
                             + diskGb(exeFile.getBytesFreeOnVolume()) + " / "
                             + diskGb(exeFile.getVolumeTotalSize()) + " GB");
    stemcel::log::info("main",
                         "port=" + juce::String(bridgePort) + " token="
                             + (bridgeToken.isNotEmpty() ? "set" : "unset") + " diagDir="
                             + (diagDir.isNotEmpty() ? diagDir : juce::String("(none)")) + " logDir="
                             + (logDirOverride.isNotEmpty() ? logDirOverride : juce::String("(off)")));
    stemcel::log::info("main",
                         "exe=" + juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName()
                             + " cwd=" + juce::File::getCurrentWorkingDirectory().getFullPathName());

    // Required even for headless JUCE apps.
    const double tStartupBegin = juce::Time::getMillisecondCounterHiRes();
    const auto elapsedSinceStart = [tStartupBegin]() -> juce::String
    {
        return juce::String(juce::roundToInt(juce::Time::getMillisecondCounterHiRes() - tStartupBegin)) + " ms";
    };
    stemcel::initialiseNativeApplication();
    const juce::ScopedJuceInitialiser_GUI juceInit;
#if JUCE_MAC
    // The engine is a bare executable, not an app bundle, so AppKit would treat it as a
    // background-only process whose plugin editor windows can never become key. Run it as
    // an accessory (UI element) process instead: its editors can take focus, and it still
    // never shows a Dock icon or menu bar of its own beside the Electron app.
    juce::Process::setDockIconVisible(false);
#endif
    stemcel::log::info("perf", "juce-gui-init done @ " + elapsedSinceStart());

    // Construct the engine and its bridge dependencies (all cheap, device-independent) so the
    // bridge can start listening BEFORE the potentially slow, cold-paged graph/format setup and
    // audio-device open. On a first cold launch of the packaged app those steps stall for seconds
    // while Windows pages in and virus-scans freshly-installed binaries; starting the bridge first
    // lets the renderer connect and paint immediately instead of waiting behind that stall. Engine
    // mutations arrive via callAsync and are not dispatched until runDispatchLoop() below, which
    // runs after startEngine() completes, so this ordering is race-free.
    stemcel::crash::setPhase("audio-engine-construct");
    if (bridgeToken.isEmpty())
    {
        stemcel::log::warn("bridge",
                             "WARNING: no AUTH token set (STEMCEL_BRIDGE_TOKEN unset and --token not given); "
                             "accepting all loopback clients. DO NOT USE IN PRODUCTION.");
    }
    stemcel::EngineHost::Options hostOptions;
    hostOptions.bridgeToken = bridgeToken;
    hostOptions.preferredDeviceType = juce::SystemStats::getEnvironmentVariable("STEMCEL_OUTPUT_DEVICE_TYPE", {});
    hostOptions.preferredDeviceName = juce::SystemStats::getEnvironmentVariable("STEMCEL_OUTPUT_DEVICE_NAME", {});
    // Persisted per-device keep-awake for the preferred output, resolved by the launcher (the
    // renderer re-pushes it on connect).
    hostOptions.keepAwake = juce::SystemStats::getEnvironmentVariable("STEMCEL_OUTPUT_KEEP_AWAKE", {}) == "1";
    auto host = std::make_unique<stemcel::EngineHost>(hostOptions);
    stemcel::log::info("perf", "engine-constructed @ " + elapsedSinceStart());

    stemcel::crash::setPhase("bridge-start");
    if (!host->bridge().start(bridgePort))
    {
        stemcel::log::error("bridge", "failed to start; exiting");
        return 1;
    }
    stemcel::log::info("perf", "bridge-listening @ " + elapsedSinceStart());

    host->startEngine();
    stemcel::log::info("perf", "engine-started @ " + elapsedSinceStart());

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#if !JUCE_WINDOWS
    // A write to a pipe whose reader has gone (a LAME child that exited early, or the
    // Electron side of stdout during shutdown) must fail with EPIPE, not kill the engine.
    std::signal(SIGPIPE, SIG_IGN);
#endif

    stemcel::crash::setPhase("running");
    stemcel::log::info("perf", "startup complete @ " + elapsedSinceStart());
    // Startup succeeded: close the always-on diagnostics log so it never grows with
    // runtime logging. A later crash is still captured by the crash reporter.
    stemcel::log::markStartupComplete();
    juce::MessageManager::getInstance()->runDispatchLoop();

    // ~EngineHost drains workers, stops the emitter and the bridge, and shuts the engine down.
    host.reset();
    stemcel::log::info("main", "shutdown complete");
    stemcel::log::shutdown();
    std::cout << "[main] shutdown complete\n";
    return 0;
}
} // namespace

// `argv` must be a C-style array; `cerr` is non-throwing unless exceptions are enabled.
// NOLINTNEXTLINE(bugprone-exception-escape,modernize-avoid-c-arrays,hicpp-avoid-c-arrays,cppcoreguidelines-avoid-c-arrays)
int main(int argc, char* argv[])
{
    try
    {
        // The plugin scan child re-launches this executable, so it is answered before any
        // engine setup — it takes no --port and must never open the bridge (ADR 0025).
        juce::StringArray commandLineArgs;
        for (int i = 1; i < argc; ++i)
            commandLineArgs.add(juce::String{argv[i]});
        const auto commandLine = commandLineArgs.joinIntoString(" ");

        if (stemcel::plugins::isScanWorkerCommandLine(commandLine))
            return stemcel::plugins::runScanWorker(commandLine);

        return runBackend(argc, argv);
    }
    catch (const std::exception& e)
    {
        std::cerr << "[fatal] uncaught exception: " << e.what() << '\n';
        return 1;
    }
    catch (...)
    {
        std::cerr << "[fatal] uncaught non-standard exception\n";
        return 1;
    }
}
