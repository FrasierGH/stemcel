#include "ProjectState.h"

#include <cmath>

namespace stemcel
{

std::vector<TempoPoint> ProjectState::getTempoMap() const
{
    std::vector<TempoPoint> points;
    const auto map = root.getChildWithName(kTempoMap);
    for (int i = 0; i < map.getNumChildren(); ++i)
    {
        const auto p = map.getChild(i);
        if (!p.hasType(kTempoPoint)) continue;
        const double bpm = static_cast<double>(p.getProperty(kBpm, 0.0));
        if (bpm > 0.0) points.push_back({static_cast<double>(p.getProperty(kPositionMs, 0.0)), bpm});
    }
    std::stable_sort(points.begin(), points.end(), [](const TempoPoint& a, const TempoPoint& b) { return a.timeMs < b.timeMs; });
    return points;
}

TempoCurve ProjectState::getTempoCurve() const
{
    return TempoCurve(getTempoMap(), getBpm());
}

bool ProjectState::setTempoMap(const std::vector<TempoPoint>& requested)
{
    std::vector<TempoPoint> points;
    for (const auto& p : requested)
        if (p.bpm >= 20.0 && p.bpm <= 400.0 && p.timeMs >= 0.0 && std::isfinite(p.timeMs)) points.push_back(p);
    std::stable_sort(points.begin(), points.end(), [](const TempoPoint& a, const TempoPoint& b) { return a.timeMs < b.timeMs; });

    const auto current = getTempoMap();
    if (current.size() == points.size()
        && std::equal(current.begin(), current.end(), points.begin(),
                      [](const TempoPoint& a, const TempoPoint& b)
                      { return std::abs(a.timeMs - b.timeMs) < 1.0e-6 && std::abs(a.bpm - b.bpm) < 1.0e-9; }))
        return false;

    auto map = root.getChildWithName(kTempoMap);
    if (!map.isValid())
    {
        map = juce::ValueTree(kTempoMap);
        root.appendChild(map, &undoManager);
    }
    map.removeAllChildren(&undoManager);
    for (const auto& p : points)
    {
        juce::ValueTree node(kTempoPoint);
        node.setProperty(kPositionMs, p.timeMs, nullptr);
        node.setProperty(kBpm, p.bpm, nullptr);
        map.appendChild(node, &undoManager);
    }
    return true;
}

void ProjectState::scaleTempoMap(double factor)
{
    if (!(factor > 0.0) || std::abs(factor - 1.0) < 1.0e-12) return;
    auto points = getTempoMap();
    if (points.empty()) return;
    for (auto& p : points)
    {
        p.timeMs /= factor;
        p.bpm = juce::jlimit(20.0, 400.0, p.bpm * factor);
    }
    setTempoMap(points);
}

double ProjectState::getClipTempoFollowBpm(const juce::String& clipId) const
{
    const auto map = root.getChildWithName(kTempoMap);
    const auto clip = findClip(clipId);
    // A drifting source follows the project tempo beat for beat, map or not.
    const bool drifting = clip.isValid() && clip.getProperty(kSourceTempo).isArray();
    if (!drifting && (!map.isValid() || map.getNumChildren() == 0)) return 0.0;
    if (!clip.isValid() || !static_cast<bool>(clip.getProperty(kWarpEnabled, false)) || clip.hasProperty(kTempoRatio))
        return 0.0;
    const double bpm = getLibraryItemBpm(clip.getProperty(kLibraryItemId, {}).toString());
    return bpm > 0.0 ? bpm : 0.0;
}

juce::var ProjectState::tempoMapAsJson() const
{
    juce::Array<juce::var> list;
    for (const auto& p : getTempoMap())
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("timeMs", p.timeMs);
        o->setProperty("bpm", p.bpm);
        list.add(juce::var(o));
    }
    return juce::var(list);
}

} // namespace stemcel
