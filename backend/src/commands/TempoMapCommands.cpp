#include "TempoMapCommands.h"

#include "AudioEngine.h"
#include "BeatRepeatCommands.h"
#include "Log.h"
#include "ProjectState.h"

namespace stemcel
{

bool applyTempoMapSet(const juce::var& payload, ProjectState& projectState)
{
    const auto* list = payload.getProperty("points", juce::var()).getArray();
    if (list == nullptr)
    {
        log::warn("tempo", "TEMPO_MAP_SET rejected: no points array");
        return false;
    }
    std::vector<TempoPoint> points;
    for (const auto& p : *list)
    {
        const auto time = p.getProperty("timeMs", juce::var());
        const auto bpm = p.getProperty("bpm", juce::var());
        if ((time.isDouble() || time.isInt() || time.isInt64()) && (bpm.isDouble() || bpm.isInt() || bpm.isInt64()))
            points.push_back({static_cast<double>(time), static_cast<double>(bpm)});
    }
    const bool changed = projectState.setTempoMap(points);
    log::info("tempo", "TEMPO_MAP_SET points=" + juce::String(static_cast<int>(points.size()))
                           + (changed ? "" : " (unchanged)"));
    return changed;
}

void syncTempoMap(AudioEngine& engine, const ProjectState& projectState)
{
    // Beat-repeat regions sit on beats, so they move with the map.
    if (engine.setTempoMap(projectState.getTempoMap(), projectState.getBpm())) syncBeatRepeatRegions(engine, projectState);
    projectState.forEachWarpClip([&](const ProjectState::WarpClipInfo& info)
                                 { engine.setClipTempoFollow(info.clipId, projectState.getClipTempoFollowBpm(info.clipId)); });
}

bool applyMixTransitionSet(const juce::var& payload, ProjectState& projectState)
{
    ProjectState::MixTransition t;
    t.id = payload.getProperty("id", {}).toString();
    t.outClipId = payload.getProperty("outClipId", {}).toString();
    t.inClipId = payload.getProperty("inClipId", {}).toString();
    t.preset = payload.getProperty("preset", {}).toString();
    t.startMs = static_cast<double>(payload.getProperty("startMs", 0.0));
    t.endMs = static_cast<double>(payload.getProperty("endMs", 0.0));
    if (const auto settings = payload.getProperty("settings", {}); settings.isObject()) t.settings = juce::JSON::toString(settings, true);
    const bool ok = projectState.setMixTransition(t);
    if (!ok) log::warn("transition", "MIX_TRANSITION_SET rejected id=" + t.id);
    return ok;
}

bool applyMixTransitionRemove(const juce::var& payload, ProjectState& projectState)
{
    return projectState.removeMixTransition(payload.getProperty("id", {}).toString());
}

} // namespace stemcel
