#include "TimeEdit.h"

#include "Automation.h"
#include "MixTransitions.h"

#include <algorithm>

namespace stemcel::native::timeedit
{
namespace
{
constexpr double kEdgeMs = 1.0; // a clip within this of an edge isn't cut there

struct Moved
{
    juce::String id;
    double oldStart = 0.0, oldEnd = 0.0;
};

void notify(TimelineActions& actions, const juce::String& message)
{
    if (actions.onNotify) actions.onNotify(message, true);
}

// A clip `at` would cut that can't be cut: its name, else empty.
juce::String uncuttableAt(const ProjectModel& model, double at)
{
    for (const auto& t : model.tracks)
        for (const auto& c : t.clips)
        {
            if (!(c.offsetMs + kEdgeMs < at && at < c.endMs() - kEdgeMs)) continue;
            const auto* item = model.findLibraryItem(c.libraryItemId);
            if (c.locked || (item != nullptr && item->kind == "clip"))
                return c.name.isNotEmpty() ? c.name : item != nullptr ? item->displayName() : c.id;
        }
    return {};
}

// Cuts every clip crossing `at`; the right-hand pieces' ids.
std::set<juce::String> cutAt(TimelineActions& actions, ProjectMirror& mirror, double at)
{
    std::set<juce::String> right;
    std::vector<juce::String> crossing;
    for (const auto& t : mirror.model().tracks)
        for (const auto& c : t.clips)
            if (c.offsetMs + kEdgeMs < at && at < c.endMs() - kEdgeMs) crossing.push_back(c.id);
    for (const auto& id : crossing)
    {
        const auto piece = actions.splitClipAt(id, at);
        mirror.refreshNow();
        if (piece.isNotEmpty()) right.insert(piece);
    }
    return right;
}

// Moves the clips starting at or after `fromMs` by `delta` (those in `skip` excepted).
void shiftClips(TimelineActions& actions, ProjectMirror& mirror, double fromMs, double delta, std::vector<Moved>& moved,
                const std::set<juce::String>& skip = {})
{
    std::vector<ClipModel> later;
    for (const auto& t : mirror.model().tracks)
        for (const auto& c : t.clips)
            if (c.offsetMs >= fromMs - 0.5 && skip.count(c.id) == 0) later.push_back(c);
    // Into the space they leave: leftwards from the front, rightwards from the back.
    std::sort(later.begin(), later.end(),
              [delta](const ClipModel& a, const ClipModel& b) { return delta < 0.0 ? a.offsetMs < b.offsetMs : a.offsetMs > b.offsetMs; });
    for (const auto& c : later)
    {
        actions.moveClip(c.id, juce::jmax(0.0, c.offsetMs + delta), c.trackId);
        moved.push_back({c.id, c.offsetMs, c.endMs()});
    }
}

// Every automation lane (tracks and master) and the tempo map, through `edit`, which maps
// one lane's points (with a value lookup on the old curve) to its new points.
template <typename Edit>
void editCurves(TimelineActions& actions, const ProjectModel& model, Edit edit)
{
    std::vector<const TrackModel*> owners;
    for (const auto& t : model.tracks) owners.push_back(&t);
    owners.push_back(&model.master);
    for (const auto* track : owners)
        for (const auto& paramId : actions.automatedParams(track->id))
        {
            const auto before = automation::pointsOf(*track, paramId);
            const auto after = edit(before, [&](double ms) { return automation::valueAt(before, ms, 0.0); });
            if (after.size() != before.size()
                || !std::equal(after.begin(), after.end(), before.begin(),
                               [](const automation::Point& a, const automation::Point& b)
                               { return std::abs(a.timeMs - b.timeMs) < 1.0e-6 && std::abs(a.value - b.value) < 1.0e-9; }))
                actions.setAutomation(track->id, paramId, after);
        }
    if (!model.tempoMap.empty())
    {
        std::vector<automation::Point> before;
        for (const auto& p : model.tempoMap) before.push_back({p.timeMs, p.bpm});
        const auto curve = model.tempoCurve();
        const auto after = edit(before, [&](double ms) { return curve.bpmAt(ms); });
        std::vector<TempoPoint> map;
        for (const auto& p : after) map.push_back({p.timeMs, p.value});
        if (map.size() != model.tempoMap.size()
            || !std::equal(map.begin(), map.end(), model.tempoMap.begin(),
                           [](const TempoPoint& a, const TempoPoint& b) { return std::abs(a.timeMs - b.timeMs) < 1.0e-6; }))
            actions.setTempoMap(map);
    }
}

void retime(TimelineActions& actions, ProjectMirror& mirror, const std::vector<Moved>& moved)
{
    mirror.refreshNow();
    juce::StringArray removed;
    for (const auto& m : moved)
        mix::retimeAfterClipEdit(actions.engineClient(), actions, mirror.model(), m.id, m.oldStart, m.oldEnd, removed);
    if (!removed.isEmpty() && actions.onNotify)
        actions.onNotify("Removed transition " + removed.joinIntoString(", ") + ": its clips no longer meet", false);
}
} // namespace

bool deleteTime(TimelineActions& actions, ProjectMirror& mirror, double fromMs, double toMs)
{
    if (toMs - fromMs < 1.0) return false;
    mirror.refreshNow();
    for (const double edge : {fromMs, toMs})
        if (const auto name = uncuttableAt(mirror.model(), edge); name.isNotEmpty())
        {
            notify(actions, "\"" + name + "\" is locked or linked, so it can't be cut there. Unlock it first.");
            return false;
        }
    const double length = toMs - fromMs;
    actions.engineClient().inUndoGroup("Delete time",
                                       [&]
                                       {
                                           cutAt(actions, mirror, fromMs);
                                           cutAt(actions, mirror, toMs);
                                           // What lies inside goes.
                                           std::vector<juce::String> inside;
                                           for (const auto& t : mirror.model().tracks)
                                               for (const auto& c : t.clips)
                                                   if (c.offsetMs >= fromMs - 0.5 && c.endMs() <= toMs + 0.5) inside.push_back(c.id);
                                           if (!inside.empty()) actions.removeClips(inside);
                                           mirror.refreshNow();
                                           const auto model = mirror.model();
                                           for (const auto& m : model.markers)
                                           {
                                               if (m.positionMs >= fromMs && m.positionMs < toMs) actions.removeMarker(m.id);
                                               else if (m.positionMs >= toMs) actions.moveMarker(m.id, m.positionMs - length);
                                           }
                                           editCurves(actions, model,
                                                      [&](const std::vector<automation::Point>& points, auto valueAt)
                                                      {
                                                          std::vector<automation::Point> out;
                                                          bool after = false;
                                                          for (auto p : points)
                                                          {
                                                              if (p.timeMs < fromMs) out.push_back(p);
                                                              else if (p.timeMs >= toMs)
                                                              {
                                                                  after = true;
                                                                  p.timeMs -= length;
                                                                  out.push_back(p);
                                                              }
                                                          }
                                                          // The join takes the value the curve had at the far edge.
                                                          if (after && !points.empty() && points.front().timeMs < toMs)
                                                          {
                                                              automation::Point join;
                                                              join.timeMs = fromMs;
                                                              join.value = valueAt(toMs);
                                                              out.push_back(join);
                                                          }
                                                          std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.timeMs < b.timeMs; });
                                                          return out;
                                                      });
                                           std::vector<Moved> moved;
                                           shiftClips(actions, mirror, toMs, -length, moved);
                                           retime(actions, mirror, moved);
                                           actions.setTimelineSelection(std::nullopt, false);
                                       });
    return true;
}

bool insertTime(TimelineActions& actions, ProjectMirror& mirror, double atMs, double lengthMs)
{
    if (lengthMs < 1.0) return false;
    mirror.refreshNow();
    if (const auto name = uncuttableAt(mirror.model(), atMs); name.isNotEmpty())
    {
        notify(actions, "\"" + name + "\" is locked or linked, so it can't be cut there. Unlock it first.");
        return false;
    }
    actions.engineClient().inUndoGroup("Insert time",
                                       [&]
                                       {
                                           cutAt(actions, mirror, atMs);
                                           const auto model = mirror.model();
                                           // From the back, so a marker never passes another.
                                           auto markers = model.markers;
                                           std::sort(markers.begin(), markers.end(), [](const auto& a, const auto& b) { return a.positionMs > b.positionMs; });
                                           for (const auto& m : markers)
                                               if (m.positionMs >= atMs) actions.moveMarker(m.id, m.positionMs + lengthMs);
                                           editCurves(actions, model,
                                                      [&](const std::vector<automation::Point>& points, auto valueAt)
                                                      {
                                                          std::vector<automation::Point> out;
                                                          bool before = false, after = false;
                                                          for (auto p : points)
                                                          {
                                                              if (p.timeMs >= atMs)
                                                              {
                                                                  after = true;
                                                                  p.timeMs += lengthMs;
                                                              }
                                                              else before = true;
                                                              out.push_back(p);
                                                          }
                                                          // The gap holds the value the curve had where it opened.
                                                          if (before && after)
                                                          {
                                                              automation::Point hold;
                                                              hold.timeMs = atMs;
                                                              hold.value = valueAt(atMs);
                                                              hold.curve = "hold";
                                                              out.push_back(hold);
                                                          }
                                                          std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.timeMs < b.timeMs; });
                                                          return out;
                                                      });
                                           std::vector<Moved> moved;
                                           shiftClips(actions, mirror, atMs, lengthMs, moved);
                                           retime(actions, mirror, moved);
                                       });
    return true;
}

void rippleDelete(TimelineActions& actions, ProjectMirror& mirror, const std::set<juce::String>& clipIds)
{
    mirror.refreshNow();
    const auto model = mirror.model();
    std::vector<ClipModel> gone;
    for (const auto& id : clipIds)
        if (const auto* c = model.findClip(id); c != nullptr && !c->locked) gone.push_back(*c);
    if (gone.empty())
    {
        if (!clipIds.empty()) notify(actions, "Locked clips were left in place.");
        return;
    }
    // Each remaining clip moves left by the lengths of the deleted clips before it on its track
    // (overlapping deleted clips count once, by the time they cover).
    std::map<juce::String, double> shiftOf;
    for (const auto& t : model.tracks)
    {
        std::vector<std::pair<double, double>> spans;
        for (const auto& g : gone)
            if (g.trackId == t.id) spans.emplace_back(g.offsetMs, g.endMs());
        if (spans.empty()) continue;
        std::sort(spans.begin(), spans.end());
        std::vector<std::pair<double, double>> merged;
        for (const auto& s : spans)
            if (!merged.empty() && s.first <= merged.back().second) merged.back().second = juce::jmax(merged.back().second, s.second);
            else merged.push_back(s);
        for (const auto& c : t.clips)
        {
            if (clipIds.count(c.id) != 0 || c.locked) continue;
            double shift = 0.0;
            for (const auto& [from, to] : merged)
                if (c.offsetMs >= to - 0.5) shift += to - from;
            if (shift > 0.0) shiftOf[c.id] = shift;
        }
    }
    actions.engineClient().inUndoGroup(gone.size() > 1 ? "Ripple delete clips" : "Ripple delete clip",
                                       [&]
                                       {
                                           std::vector<juce::String> ids;
                                           for (const auto& g : gone) ids.push_back(g.id);
                                           actions.removeClips(ids);
                                           mirror.refreshNow();
                                           std::vector<ClipModel> later;
                                           for (const auto& t : mirror.model().tracks)
                                               for (const auto& c : t.clips)
                                                   if (shiftOf.count(c.id) != 0) later.push_back(c);
                                           std::sort(later.begin(), later.end(), [](const auto& a, const auto& b) { return a.offsetMs < b.offsetMs; });
                                           std::vector<Moved> moved;
                                           for (const auto& c : later)
                                           {
                                               actions.moveClip(c.id, juce::jmax(0.0, c.offsetMs - shiftOf[c.id]), c.trackId);
                                               moved.push_back({c.id, c.offsetMs, c.endMs()});
                                           }
                                           retime(actions, mirror, moved);
                                       });
}

} // namespace stemcel::native::timeedit
