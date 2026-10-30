#pragma once

#include <juce_core/juce_core.h>

namespace stemcel
{

class BridgeServer;
class ProjectState;
struct ProjectSession;

void handleScratchPatternSave(const juce::var& payload, ProjectState& projectState,
                              BridgeServer& bridge, const ProjectSession& session);

void handleScratchPatternDelete(const juce::var& payload, ProjectState& projectState,
                                BridgeServer& bridge, const ProjectSession& session);

void handleScratchPatternRename(const juce::var& payload, ProjectState& projectState,
                                BridgeServer& bridge, const ProjectSession& session);

} // namespace stemcel
