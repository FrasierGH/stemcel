#include "EngineHost.h"

#include "AudioDeviceCommands.h"
#include "BridgeDispatch.h"
#include "CrashHandler.h"
#include "EditUndoState.h"
#include "Log.h"
#include "RecordingCommands.h"

#include <utility>

namespace stemcel
{
namespace
{
constexpr int kPlayheadUpdateHz = 60;
// Peak jobs are disk-bound; four workers keep imports responsive without saturating cores.
constexpr int kPeakWorkerCount = 4;

juce::var engineError(const juce::String& message, const juce::String& context)
{
    auto* p = new juce::DynamicObject();
    p->setProperty("message", message);
    p->setProperty("context", context);
    return juce::var(p);
}

juce::var audioStatus(const AudioEngine& engine)
{
    auto* status = new juce::DynamicObject();
    status->setProperty("state", engine.isAudioReady() ? "ready" : "no_device");
    return juce::var(status);
}
} // namespace

EngineHost::EngineHost(Options opts)
    : options(std::move(opts)),
      peakWorkers(kPeakWorkerCount),
      // Callbacks are frozen at construction so the bridge's I/O-thread reads are race-free.
      bridgeServer(
          options.bridgeToken,
          [this](BridgeServer&, const juce::String& type, const juce::var& payload) { dispatch(type, payload); },
          [this](const BridgeServer::SendToClient& sendToClient) { sendInitialState(sendToClient); })
{
    // Lets PROJECT_STATE flag slots whose plugin is missing on this machine, without that
    // machine-specific fact ever reaching the project file (ADR 0025).
    projectState.setPluginAvailabilityProbe([this](const juce::String& identifier) {
        return audioEngine.pluginCatalogue().hasPlugin(identifier);
    });
}

EngineHost::~EngineHost()
{
    // Drain worker jobs before captured bridge/cache/engine references destruct.
    peakWorkers.removeAllJobs(false, 5000);
    log::info("main", "engine host: workers drained");
    if (emitter != nullptr) emitter->stopTimer();
    bridgeServer.stop();
    log::info("main", "engine host: bridge stopped");
    shutdownRecording();
    audioEngine.shutdown();
    log::info("main", "engine host: engine shut down");
}

void EngineHost::startEngine()
{
    // Device-independent graph setup: registers audio formats and wires the source graph.
    crash::setPhase("audio-graph-init");
    audioEngine.initialiseGraph();
    audioEngine.setSafetyLimiterEnabled(projectState.getSafetyLimiterEnabled(), /*snap*/ true);
    audioEngine.setProjectMixGlue(projectState.getProjectMixGlueAmount(), /*snap*/ true);
    audioEngine.setProjectMixGlueMakeup(projectState.getProjectMixGlueMakeupDb(), /*snap*/ true);

    // Enable keep-awake BEFORE the device opens so the keep-alive dither + wake burst run from
    // the first audio block and rouse a cold sleep-prone DAC at stream start.
    if (options.keepAwake) audioEngine.setKeepAwakeEnabled(true);
    log::info("audio", "preferred output: type='" + options.preferredDeviceType + "' name='"
                           + options.preferredDeviceName + "'"
                           + (options.preferredDeviceType.isEmpty() && options.preferredDeviceName.isEmpty()
                                  ? " (system default)"
                                  : "")
                           + (options.keepAwake ? " [keep-awake]" : ""));

    // Dirty broadcasts mirror message-thread ValueTree mutations to the UI's chrome.
    projectState.setDirtyChangedCallback(
        [this](bool dirty, ProjectState::DirtyReason reason)
        {
            auto* p = new juce::DynamicObject();
            p->setProperty("dirty", dirty);
            // Only a transition INTO dirty has a cause worth naming: going clean is always a
            // save or a load. "analysis" tells the UI the change came from background tempo
            // analysis landing rather than anything the user did.
            if (dirty)
            {
                p->setProperty("reason",
                               reason == ProjectState::DirtyReason::BackgroundAnalysis ? "analysis" : "edit");
            }
            bridgeServer.broadcast("PROJECT_DIRTY", juce::var(p));
        });

    // Re-broadcast device changes after the engine snapshot/fallback is already updated.
    audioEngine.setDeviceListChangedCallback(
        [this]()
        {
            broadcastAudioDevicesList(bridgeServer,
                                      buildAudioDevicesListEnvelope(audioEngine.getAudioDevicesSnapshot()),
                                      /*dedupe*/ true);
        });

    // Open the audio device synchronously on the message thread. An earlier worker-thread
    // open destabilised output devices: activating the WASAPI/COM device on a transient MTA
    // worker that then exited invalidated the device's COM objects. A failed open is non-fatal
    // (logged); the engine stays alive and the app remains usable without output.
    crash::setPhase("audio-device-init");
    if (const auto err = audioEngine.openAudioDevice(options.preferredDeviceType, options.preferredDeviceName);
        err.isNotEmpty())
    {
        log::error("engine", "audio device open failed: " + err);
    }
    broadcastAudioDevicesList(bridgeServer, buildAudioDevicesListEnvelope(audioEngine.getAudioDevicesSnapshot()),
                              /*dedupe*/ false);
    bridgeServer.broadcast("ENGINE_AUDIO_STATUS", audioStatus(audioEngine));

    emitter = std::make_unique<PlayheadEmitter>(audioEngine, bridgeServer, projectState);
    emitter->startTimerHz(kPlayheadUpdateHz);
}

void EngineHost::dispatch(const juce::String& type, const juce::var& payload)
{
    // Message-thread crash firewall: prefer an ENGINE_ERROR over a dead engine.
    try
    {
        dispatchBridgeMessage(type, payload, audioEngine, projectState, bridgeServer, peakWorkers, peaks, decoded,
                              peakJobs, projectSession);
    }
    catch (const std::exception& e)
    {
        log::error("bridge", "handler threw for type=" + type + ": " + juce::String(e.what())
                                 + " - engine kept alive");
        bridgeServer.broadcast("ENGINE_ERROR", engineError(juce::String(e.what()), type));
    }
    catch (...)
    {
        log::error("bridge", "handler threw unknown exception for type=" + type + " - engine kept alive");
        bridgeServer.broadcast("ENGINE_ERROR", engineError("Unknown engine error", type));
    }
}

void EngineHost::sendInitialState(const BridgeServer::SendToClient& sendToClient)
{
    sendToClient("PROJECT_STATE", buildProjectStateEnvelope(projectSession, projectState, false));
    // Seeds menu state before first paint.
    sendToClient("EDIT_UNDO_STATE", buildEditUndoStateEnvelope(projectState));
    // Runs after the synchronous startup open, so a non-ready engine has no output device
    // rather than an open still in progress.
    sendToClient("ENGINE_AUDIO_STATUS", audioStatus(audioEngine));
}

} // namespace stemcel
