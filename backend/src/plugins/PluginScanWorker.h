#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <memory>

namespace stemcel::plugins
{

// Identifies the child-process scan mode on the command line; shared with the coordinator.
inline constexpr const char* kScanWorkerUid = "stemcel-plugin-scan";

// True when this process was launched to scan plugins rather than to run the engine.
bool isScanWorkerCommandLine(const juce::String& commandLine);

// Runs the scan child until the coordinator disconnects. Only valid when the command line
// is a scan-worker one.
int runScanWorker(const juce::String& commandLine);

// Starts the scan child inside an already-running JUCE app (the native app, whose message
// loop belongs to JUCEApplication). Keep the result alive; when the coordinator disconnects
// the worker stops the dispatch loop, which ends the app. Null if the handshake fails.
std::unique_ptr<juce::ChildProcessWorker> startScanWorker(const juce::String& commandLine);

} // namespace stemcel::plugins
