#pragma once

#include <juce_core/juce_core.h>

namespace stemcel
{

class ProjectState;


void applyMarkerAdd(const juce::var& payload, ProjectState& projectState);
void applyMarkerMove(const juce::var& payload, ProjectState& projectState);
void applyMarkerRemove(const juce::var& payload, ProjectState& projectState);

} // namespace stemcel
