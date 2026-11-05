#pragma once

#include <juce_core/juce_core.h>

namespace stemcel
{

class AudioEngine;
class ProjectState;
class BridgeServer;
class DecodedCache;
struct ProjectSession;

// Track gain/mute/solo share effective-gain fan-out in TrackCommands.cpp.

void handleTrackAdd(const juce::var& payload, ProjectState& projectState, BridgeServer& bridge);
void handleTrackRemove(const juce::var& payload, AudioEngine& engine, ProjectState& projectState,
                       BridgeServer& bridge);
void handleTrackRename(const juce::var& payload, ProjectState& projectState);
// TRACK_DUPLICATE {trackId, newTrackId, name}: a copy of the track after it, with its clips,
// transitions, plugins and automation; the engine is rebuilt from the project to play it.
void handleTrackDuplicate(const juce::var& payload, AudioEngine& engine, ProjectState& projectState, BridgeServer& bridge,
                          juce::ThreadPool& peakPool, const DecodedCache& decodedCache, ProjectSession& session);
void handleTrackGain(const juce::var& payload, AudioEngine& engine, ProjectState& projectState,
                     BridgeServer& bridge);
void handleTrackMute(const juce::var& payload, AudioEngine& engine, ProjectState& projectState,
                     BridgeServer& bridge);
void handleTrackSolo(const juce::var& payload, AudioEngine& engine, ProjectState& projectState,
                     BridgeServer& bridge);
void handleTrackSetSends(const juce::var& payload, AudioEngine& engine, ProjectState& projectState,
                         BridgeServer& bridge);
void handleTrackSetPan(const juce::var& payload, AudioEngine& engine, ProjectState& projectState,
                       BridgeServer& bridge);
void handleTrackSetTone(const juce::var& payload, AudioEngine& engine, ProjectState& projectState,
                        BridgeServer& bridge);
void handleTrackSetPunch(const juce::var& payload, AudioEngine& engine, ProjectState& projectState,
                         BridgeServer& bridge);
void handleTrackSetSaturation(const juce::var& payload, AudioEngine& engine,
                              ProjectState& projectState, BridgeServer& bridge);
void handleTrackSetBitCrusher(const juce::var& payload, AudioEngine& engine,
                              ProjectState& projectState, BridgeServer& bridge);
void handleTrackSetAutomation(const juce::var& payload, AudioEngine& engine, ProjectState& projectState,
                              BridgeServer& bridge);

} // namespace stemcel
