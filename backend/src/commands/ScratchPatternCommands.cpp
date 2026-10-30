#include "ScratchPatternCommands.h"

#include "BridgeServer.h"
#include "Log.h"
#include "PayloadHelpers.h"
#include "ProjectSession.h"
#include "ProjectState.h"
#include "scratch/ScratchProtocol.h"

namespace stemcel
{

using stemcel::bridge::tryGetRequiredString;

void handleScratchPatternSave(const juce::var& payload, ProjectState& projectState,
                              BridgeServer& bridge, const ProjectSession& session)
{
    if (!scratch::hasValidProtocolVersion(payload))
    {
        stemcel::log::warn("scratch", "rejected SCRATCH_PATTERN_SAVE: bad protocolVersion");
        return;
    }

    const auto patternVar = payload.getProperty("pattern", juce::var());
    if (!patternVar.isObject())
    {
        stemcel::log::warn("scratch", "rejected SCRATCH_PATTERN_SAVE: missing pattern object");
        return;
    }

    const auto parsed = scratch::parsePattern(patternVar);
    if (!parsed)
    {
        stemcel::log::warn("scratch", "rejected SCRATCH_PATTERN_SAVE: pattern failed validation");
        return;
    }

    const bool updated = projectState.hasScratchPattern(parsed->id)
                             ? projectState.updateScratchPattern(parsed->id, patternVar)
                             : projectState.addScratchPattern(patternVar);

    if (!updated)
    {
        stemcel::log::warn("scratch", "SCRATCH_PATTERN_SAVE: state mutation failed for id=" + parsed->id);
        return;
    }

    stemcel::log::info("scratch", "SCRATCH_PATTERN_SAVE ok id=" + parsed->id + " name=" + parsed->name);
    bridge.broadcast("PROJECT_STATE", stemcel::buildProjectStateEnvelope(session, projectState, false));
}

void handleScratchPatternDelete(const juce::var& payload, ProjectState& projectState,
                                BridgeServer& bridge, const ProjectSession& session)
{
    if (!scratch::hasValidProtocolVersion(payload))
    {
        stemcel::log::warn("scratch", "rejected SCRATCH_PATTERN_DELETE: bad protocolVersion");
        return;
    }

    const auto patternId = tryGetRequiredString(payload, "patternId").value_or(juce::String{});
    if (patternId.isEmpty())
    {
        stemcel::log::warn("scratch", "rejected SCRATCH_PATTERN_DELETE: missing patternId");
        return;
    }

    if (!projectState.removeScratchPattern(patternId))
    {
        stemcel::log::warn("scratch", "SCRATCH_PATTERN_DELETE: id not found: " + patternId);
        return;
    }

    stemcel::log::info("scratch", "SCRATCH_PATTERN_DELETE ok id=" + patternId);
    bridge.broadcast("PROJECT_STATE", stemcel::buildProjectStateEnvelope(session, projectState, false));
}

void handleScratchPatternRename(const juce::var& payload, ProjectState& projectState,
                                BridgeServer& bridge, const ProjectSession& session)
{
    if (!scratch::hasValidProtocolVersion(payload))
    {
        stemcel::log::warn("scratch", "rejected SCRATCH_PATTERN_RENAME: bad protocolVersion");
        return;
    }

    const auto patternId = tryGetRequiredString(payload, "patternId").value_or(juce::String{});
    const auto newName = tryGetRequiredString(payload, "name").value_or(juce::String{});
    if (patternId.isEmpty() || newName.isEmpty())
    {
        stemcel::log::warn("scratch", "rejected SCRATCH_PATTERN_RENAME: missing patternId or name");
        return;
    }

    if (!projectState.renameScratchPattern(patternId, newName))
    {
        stemcel::log::warn("scratch", "SCRATCH_PATTERN_RENAME: id not found: " + patternId);
        return;
    }

    stemcel::log::info("scratch", "SCRATCH_PATTERN_RENAME ok id=" + patternId + " name=" + newName);
    bridge.broadcast("PROJECT_STATE", stemcel::buildProjectStateEnvelope(session, projectState, false));
}

} // namespace stemcel
