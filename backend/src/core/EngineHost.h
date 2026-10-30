#pragma once

#include "AudioEngine.h"
#include "BridgeServer.h"
#include "DecodedCache.h"
#include "PeakJobCoordinator.h"
#include "PeaksCache.h"
#include "PlayheadEmitter.h"
#include "ProjectSession.h"
#include "ProjectState.h"

#include <juce_core/juce_core.h>

#include <memory>

namespace stemcel
{

// Owns the engine and everything the bridge commands act on, wired the way both front ends
// need it: the console backend (driven by the Electron UI over the WebSocket bridge) and the
// native app (driving the same commands in-process). Keeping one wiring means a command
// behaves identically whichever UI sends it.
//
// Two phases, so the console backend can start listening before the slow part:
//   1. Construction: cheap, device-independent objects, including the (unstarted) bridge.
//   2. startEngine(): audio graph, preferred output device, change callbacks, playhead timer.
// Everything runs on the JUCE message thread.
class EngineHost
{
  public:
    struct Options
    {
        // Required by WebSocket clients; unused by an in-process host that never starts the bridge.
        juce::String bridgeToken;
        // Output device to open at start; empty names mean the system default.
        juce::String preferredDeviceType;
        juce::String preferredDeviceName;
        // Per-device keep-awake for the preferred output, enabled before the device opens.
        bool keepAwake = false;
    };

    explicit EngineHost(Options opts);
    ~EngineHost();

    EngineHost(const EngineHost&) = delete;
    EngineHost& operator=(const EngineHost&) = delete;
    EngineHost(EngineHost&&) = delete;
    EngineHost& operator=(EngineHost&&) = delete;

    // Phase 2: initialise the audio graph, open the preferred device, hook up the dirty and
    // device-list broadcasts and start the 60 Hz playhead/meter emitter.
    void startEngine();

    // Runs one bridge command in-process, behind the same crash firewall as the WebSocket
    // path: a throwing handler becomes an ENGINE_ERROR broadcast, never a dead engine.
    void dispatch(const juce::String& type, const juce::var& payload);

    // The state a freshly connected UI needs before its first paint.
    void sendInitialState(const BridgeServer::SendToClient& sendToClient);

    BridgeServer& bridge() { return bridgeServer; }
    AudioEngine& engine() { return audioEngine; }
    ProjectState& project() { return projectState; }
    ProjectSession& session() { return projectSession; }
    juce::ThreadPool& peakPool() { return peakWorkers; }
    const PeaksCache& peaksCache() const { return peaks; }
    const DecodedCache& decodedCache() const { return decoded; }

  private:
    Options options;

    // Declaration order is destruction order in reverse: the bridge and the emitter, which
    // capture references to everything above them, go first.
    AudioEngine audioEngine;
    ProjectState projectState;
    ProjectSession projectSession;
    const PeaksCache peaks;
    const DecodedCache decoded;
    PeakJobCoordinator peakJobs;
    juce::ThreadPool peakWorkers;
    BridgeServer bridgeServer;
    std::unique_ptr<PlayheadEmitter> emitter;
};

} // namespace stemcel
