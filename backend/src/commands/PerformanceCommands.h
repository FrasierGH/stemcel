#pragma once

// Live performance mode's bridge commands (PERF_*): opening and closing the mode, loading
// decks, controls from the view, recording, and the controller's LEDs.
//
//   PERF_OPEN / PERF_CLOSE
//   PERF_LOAD     {deck, libraryItemId, fromDeck?, cueSec?, hotCues?, memoryCues?}
//   PERF_CONTROL  {control, deck?, index?, mode?, value}
//   PERF_RECORD   {on}
//   PERF_MIDI_INJECT {bytes: [..], device?}  raw MIDI as if from the controller (tests)
// Broadcasts: PERF_STATE (60 Hz), PERF_REC (the take log), PERF_UI (browser and LOAD controls),
// PERF_LOADED, PERF_CUES (hot/memory cues changed, to save with the item).

#include <juce_core/juce_core.h>

namespace stemcel
{

class AudioEngine;
class BridgeServer;
class ProjectState;
class DecodedCache;

bool dispatchPerformanceCommand(const juce::String& type, const juce::var& payload, AudioEngine& engine,
                                ProjectState& project, BridgeServer& bridge, const DecodedCache& decodedCache,
                                juce::ThreadPool& pool);
void shutdownPerformance();

} // namespace stemcel
