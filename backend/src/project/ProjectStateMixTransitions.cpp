#include "ProjectState.h"

namespace stemcel
{

namespace
{
const juce::Identifier kOutClipId{"outClipId"};
const juce::Identifier kInClipId{"inClipId"};
const juce::Identifier kPreset{"preset"};
const juce::Identifier kStartMs{"startMs"};
const juce::Identifier kEndMs{"endMs"};
const juce::Identifier kSettings{"settings"};
} // namespace

std::vector<ProjectState::MixTransition> ProjectState::getMixTransitions() const
{
    std::vector<MixTransition> list;
    const auto node = root.getChildWithName(kMixTransitions);
    for (int i = 0; i < node.getNumChildren(); ++i)
    {
        const auto t = node.getChild(i);
        if (!t.hasType(kMixTransition)) continue;
        list.push_back({t.getProperty(kId).toString(), t.getProperty(kOutClipId).toString(),
                        t.getProperty(kInClipId).toString(), t.getProperty(kPreset).toString(),
                        static_cast<double>(t.getProperty(kStartMs, 0.0)), static_cast<double>(t.getProperty(kEndMs, 0.0)),
                        t.getProperty(kSettings).toString()});
    }
    return list;
}

bool ProjectState::setMixTransition(const MixTransition& t)
{
    if (t.id.isEmpty() || t.outClipId.isEmpty() || t.inClipId.isEmpty() || t.preset.isEmpty() || !(t.endMs >= t.startMs))
        return false;
    auto node = root.getChildWithName(kMixTransitions);
    if (!node.isValid())
    {
        node = juce::ValueTree(kMixTransitions);
        root.appendChild(node, &undoManager);
    }
    auto existing = node.getChildWithProperty(kId, t.id);
    if (!existing.isValid())
    {
        existing = juce::ValueTree(kMixTransition);
        existing.setProperty(kId, t.id, nullptr);
        node.appendChild(existing, &undoManager);
    }
    existing.setProperty(kOutClipId, t.outClipId, &undoManager);
    existing.setProperty(kInClipId, t.inClipId, &undoManager);
    existing.setProperty(kPreset, t.preset, &undoManager);
    existing.setProperty(kStartMs, t.startMs, &undoManager);
    existing.setProperty(kEndMs, t.endMs, &undoManager);
    if (t.settings.isNotEmpty()) existing.setProperty(kSettings, t.settings, &undoManager);
    else existing.removeProperty(kSettings, &undoManager);
    return true;
}

bool ProjectState::removeMixTransition(const juce::String& id)
{
    auto node = root.getChildWithName(kMixTransitions);
    const auto t = node.getChildWithProperty(kId, id);
    if (!t.isValid()) return false;
    node.removeChild(t, &undoManager);
    return true;
}

void ProjectState::retimeMixTransitionsForTempoChange(double previousBpm, double bpm)
{
    if (!(previousBpm > 0.0) || !(bpm > 0.0) || previousBpm == bpm) return;
    const double scale = previousBpm / bpm;
    auto node = root.getChildWithName(kMixTransitions);
    for (int i = 0; i < node.getNumChildren(); ++i)
    {
        auto t = node.getChild(i);
        t.setProperty(kStartMs, static_cast<double>(t.getProperty(kStartMs, 0.0)) * scale, &undoManager);
        t.setProperty(kEndMs, static_cast<double>(t.getProperty(kEndMs, 0.0)) * scale, &undoManager);
    }
}

juce::var ProjectState::mixTransitionsAsJson() const
{
    juce::Array<juce::var> list;
    for (const auto& t : getMixTransitions())
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("id", t.id);
        o->setProperty("outClipId", t.outClipId);
        o->setProperty("inClipId", t.inClipId);
        o->setProperty("preset", t.preset);
        o->setProperty("startMs", t.startMs);
        o->setProperty("endMs", t.endMs);
        if (t.settings.isNotEmpty()) o->setProperty("settings", juce::JSON::parse(t.settings));
        list.add(juce::var(o));
    }
    return juce::var(list);
}

} // namespace stemcel
