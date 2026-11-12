#include "PerformanceTake.h"

#include "Automation.h"

#include <algorithm>
#include <cmath>

namespace stemcel::native::perf
{
namespace
{
constexpr double kMinClipMs = 4.0;

juce::var obj(std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();
    for (const auto& [k, v] : props) o->setProperty(k, v);
    return juce::var(o);
}

// Ramer–Douglas–Peucker: the fewest points that stay within `tolerance` of the curve.
std::vector<std::pair<double, double>> thin(const std::vector<std::pair<double, double>>& pts, double tolerance)
{
    if (pts.size() <= 2) return pts;
    std::vector<bool> keep(pts.size(), false);
    keep.front() = keep.back() = true;
    std::vector<std::pair<size_t, size_t>> stack{{0, pts.size() - 1}};
    while (!stack.empty())
    {
        const auto [a, b] = stack.back();
        stack.pop_back();
        double worst = 0.0;
        size_t at = a;
        const auto& p = pts[a];
        const auto& q = pts[b];
        for (size_t i = a + 1; i < b; ++i)
        {
            const double t = q.first > p.first ? (pts[i].first - p.first) / (q.first - p.first) : 0.0;
            const double d = std::abs(pts[i].second - (p.second + t * (q.second - p.second)));
            if (d > worst)
            {
                worst = d;
                at = i;
            }
        }
        if (worst > tolerance)
        {
            keep[at] = true;
            stack.push_back({a, at});
            stack.push_back({at, b});
        }
    }
    std::vector<std::pair<double, double>> out;
    for (size_t i = 0; i < pts.size(); ++i)
        if (keep[i]) out.push_back(pts[i]);
    return out;
}

double tolerance(const juce::String& param)
{
    if (param == "filter" || param == "colorFx" || param.endsWith("Level")) return 0.01;
    if (param == "fxBpm") return 0.05;
    return 0.3;
}

// Whole-number parameters (types, beat steps, on/off) hold their value to the next change.
bool stepped(const juce::String& param)
{
    return param.endsWith("Type") || param.endsWith("Beats") || param.endsWith("On");
}

// A control's recorded values as a curve: the recorder only logs changes, so a value holds
// until just before the next one (a knob moved later, not a ramp from the last change), and
// the curve runs to the end of the take.
constexpr double kHoldGapMs = 25.0;
std::vector<std::pair<double, double>> heldCurve(const std::vector<std::pair<double, double>>& pts, double endMs)
{
    std::vector<std::pair<double, double>> out;
    for (const auto& p : pts)
    {
        if (!out.empty() && p.first - out.back().first > kHoldGapMs)
            out.push_back({p.first - 5.0, out.back().second});
        out.push_back(p);
    }
    if (!out.empty() && endMs > out.back().first + 1.0) out.push_back({endMs, out.back().second});
    return out;
}

std::vector<std::pair<double, double>> changesOnly(const std::vector<std::pair<double, double>>& pts)
{
    std::vector<std::pair<double, double>> out;
    for (const auto& p : pts)
        if (out.empty() || std::abs(out.back().second - p.second) > 1.0e-6) out.push_back(p);
    return out;
}
} // namespace

void PerformanceTake::clear()
{
    startAt = -1.0;
    lastAt = 0.0;
    segments.clear();
    platter.clear();
    params.clear();
    sampleHits.clear();
}

void PerformanceTake::add(const juce::var& payload)
{
    const auto* events = payload.getProperty("events", {}).getArray();
    if (events == nullptr) return;
    for (const auto& e : *events)
    {
        const auto kind = e.getProperty("kind", "").toString();
        const double at = static_cast<double>(e.getProperty("at", 0.0));
        const int deck = static_cast<int>(e.getProperty("deck", 0));
        if (kind == "start")
        {
            clear();
            startAt = at;
            rate = static_cast<double>(e.getProperty("sampleRate", 44100.0));
            continue;
        }
        if (startAt < 0.0) continue; // before a take began
        lastAt = std::max(lastAt, at);
        if (kind == "segment")
        {
            TakeSegment s;
            s.at = at;
            s.deck = deck;
            s.itemId = e.getProperty("libraryItemId", "").toString();
            s.endPos = static_cast<double>(e.getProperty("endPos", 0.0));
            s.startPos = static_cast<double>(e.getProperty("startPos", 0.0));
            s.tempo = static_cast<double>(e.getProperty("tempo", 1.0));
            s.semitones = static_cast<int>(e.getProperty("semitones", 0));
            s.moving = static_cast<bool>(e.getProperty("moving", false));
            s.audible = static_cast<bool>(e.getProperty("audible", false));
            s.keyLock = static_cast<bool>(e.getProperty("keyLock", false));
            s.scratch = static_cast<bool>(e.getProperty("scratch", false));
            segments.push_back(std::move(s));
        }
        else if (kind == "sample")
        {
            const int slot = static_cast<int>(e.getProperty("slot", 0));
            if (static_cast<bool>(e.getProperty("start", false)))
                sampleHits.push_back({at, -1.0, static_cast<double>(e.getProperty("lengthSec", 0.0)), slot,
                                      e.getProperty("libraryItemId", "").toString()});
            else
                for (auto it = sampleHits.rbegin(); it != sampleHits.rend(); ++it)
                    if (it->slot == slot && it->stopAt < 0.0)
                    {
                        it->stopAt = at;
                        break;
                    }
        }
        else if (kind == "platter")
            platter[deck].push_back({at, static_cast<double>(e.getProperty("pos", 0.0))});
        else if (kind == "param")
            params[deck][e.getProperty("param", "").toString()].push_back({at, static_cast<double>(e.getProperty("value", 0.0))});
    }
}

bool PerformanceTake::hasAudio() const
{
    for (const auto& s : segments)
        if (s.moving && s.audible) return true;
    return !sampleHits.empty();
}

double PerformanceTake::lengthSec() const { return startAt < 0.0 ? 0.0 : (lastAt - startAt) / rate; }

TakeEdits PerformanceTake::edits(double startMs) const
{
    TakeEdits out;
    if (startAt < 0.0) return out;
    auto ms = [&](double at) { return startMs + (at - startAt) / rate * 1000.0; };
    out.lengthMs = (lastAt - startAt) / rate * 1000.0;

    for (int deck = 1; deck <= kDecks; ++deck)
    {
        std::vector<const TakeSegment*> list;
        for (const auto& s : segments)
            if (s.deck == deck) list.push_back(&s);
        bool used = false;
        for (size_t i = 0; i + 1 < list.size(); ++i)
        {
            const auto& s = *list[i];
            const auto& next = *list[i + 1];
            if (!s.moving || !s.audible || s.itemId.isEmpty()) continue;
            const double t0 = ms(s.at), t1 = ms(next.at);
            if (t1 - t0 < kMinClipMs) continue;
            auto addClip = [&](double from, double to, double srcFrom, double srcTo)
            {
                TakeEdits::Clip c;
                c.deck = deck;
                c.itemId = s.itemId;
                c.positionMs = from;
                c.semitones = s.semitones;
                c.keyLock = s.keyLock && !s.scratch;
                c.reversed = srcTo < srcFrom;
                c.inMs = std::min(srcFrom, srcTo) * 1000.0;
                c.durationMs = std::abs(srcTo - srcFrom) * 1000.0;
                c.tempoRatio = juce::jlimit(0.25, 4.0, c.durationMs / std::max(1.0e-6, to - from));
                if (c.durationMs >= 1.0 && to - from >= kMinClipMs) out.clips.push_back(c);
                used = true;
            };
            if (!s.scratch)
            {
                addClip(t0, t1, s.startPos, next.endPos);
                continue;
            }
            // The jog had it: follow the platter, a clip for each run in one direction.
            std::vector<std::pair<double, double>> path{{s.at, s.startPos}};
            const auto it = platter.find(deck);
            if (it != platter.end())
                for (const auto& p : it->second)
                    if (p.first > s.at && p.first < next.at) path.push_back(p);
            path.push_back({next.at, next.endPos});
            size_t runStart = 0;
            for (size_t k = 1; k < path.size(); ++k)
            {
                const double dir = path[k].second - path[k - 1].second;
                const bool last = k + 1 == path.size();
                const double nextDir = last ? dir : path[k + 1].second - path[k].second;
                if (last || (dir >= 0.0) != (nextDir >= 0.0))
                {
                    const auto& a = path[runStart];
                    const auto& b = path[k];
                    // Held still (the record stopped under the hand) is silence.
                    if (std::abs(b.second - a.second) > 0.002) addClip(ms(a.first), ms(b.first), a.second, b.second);
                    runStart = k;
                }
            }
        }
        if (used) out.decksUsed.push_back(deck);
        // The channel's curves, thinned to what changes them.
        if (const auto pit = params.find(deck); pit != params.end() && used)
            for (const auto& [param, points] : pit->second)
            {
                std::vector<std::pair<double, double>> curve;
                for (const auto& p : points)
                {
                    double v = p.value;
                    if (param == "level") v = juce::jlimit(-60.0, 6.0, v);
                    curve.push_back({ms(p.at), v});
                }
                curve = heldCurve(curve, ms(lastAt));
                out.automation[deck][param] = stepped(param) ? changesOnly(curve) : thin(curve, tolerance(param));
                if (out.automation[deck][param].size() == 1)
                    out.automation[deck][param].push_back({ms(lastAt), out.automation[deck][param].front().second});
            }
    }
    // The master's Beat FX (when it was on the master) and the sampler.
    if (const auto pit = params.find(0); pit != params.end())
    {
        bool used = false;
        if (const auto on = pit->second.find("beatFxOn"); on != pit->second.end())
            for (const auto& p : on->second) used = used || p.value > 0.5;
        if (used)
            for (const auto& [param, points] : pit->second)
            {
                std::vector<std::pair<double, double>> curve;
                for (const auto& p : points) curve.push_back({ms(p.at), p.value});
                curve = heldCurve(curve, ms(lastAt));
                out.master[param] = stepped(param) ? changesOnly(curve) : thin(curve, tolerance(param));
                if (out.master[param].size() == 1) out.master[param].push_back({ms(lastAt), out.master[param].front().second});
            }
    }
    for (const auto& h : sampleHits)
    {
        if (h.itemId.isEmpty()) continue;
        const double endAt = h.stopAt >= 0.0 ? h.stopAt : h.at + h.lengthSec * rate;
        const double length = std::min(h.lengthSec * 1000.0, (endAt - h.at) / rate * 1000.0);
        if (length >= kMinClipMs) out.samples.push_back({h.slot, h.itemId, ms(h.at), length});
    }
    if (const auto pit = params.find(kDecks + 1); pit != params.end())
        if (const auto level = pit->second.find("level"); level != pit->second.end())
        {
            std::vector<std::pair<double, double>> curve;
            for (const auto& p : level->second) curve.push_back({ms(p.at), juce::jlimit(-60.0, 6.0, p.value)});
            out.samplerLevel = heldCurve(curve, ms(lastAt));
        }
    return out;
}

bool PerformanceTake::write(EngineClient& engine, ProjectMirror& mirror, TimelineActions& actions, double startMs,
                            int takeNumber) const
{
    const auto e = edits(startMs);
    if (e.clips.empty() && e.samples.empty()) return false;
    engine.inUndoGroup(
        "Live performance",
        [&]
        {
            std::map<int, juce::String> tracks;
            for (const int deck : e.decksUsed)
            {
                const auto id = actions.addTrack();
                actions.renameTrack(id, "Deck " + juce::String(deck) + (takeNumber > 1 ? " (take " + juce::String(takeNumber) + ")" : ""));
                tracks[deck] = id;
            }
            for (const auto& c : e.clips)
            {
                const auto clipId = EngineClient::newId("clip");
                engine.send("CLIP_ADD", obj({{"trackId", tracks[c.deck]},
                                             {"clipId", clipId},
                                             {"libraryItemId", c.itemId},
                                             {"positionMs", c.positionMs},
                                             {"inMs", c.inMs},
                                             {"durationMs", c.durationMs}}));
                const bool timed = std::abs(c.tempoRatio - 1.0) > 1.0e-4 || c.semitones != 0 || c.keyLock;
                // Pinned: the clip plays at the speed it was played, whatever the project tempo does.
                engine.send("CLIP_SET_WARP", obj({{"clipId", clipId},
                                                  {"warpEnabled", timed},
                                                  {"warpMode", c.keyLock ? "pro" : "repitch"},
                                                  {"tempoRatio", c.tempoRatio},
                                                  {"semitones", c.semitones},
                                                  {"cents", 0}}));
                if (c.reversed) engine.send("CLIP_SET_REVERSED", obj({{"clipId", clipId}, {"reversed", true}}));
            }
            for (const auto& [deck, curves] : e.automation)
            {
                const auto track = tracks.find(deck);
                if (track == tracks.end()) continue;
                std::vector<automation::Lane> lanes;
                for (const auto& [param, pts] : curves)
                {
                    std::vector<automation::Point> points;
                    for (const auto& [t, v] : pts) points.push_back({t, v, stepped(param) ? "hold" : "linear"});
                    if (points.empty()) continue;
                    actions.setAutomation(track->second, param, points);
                    lanes.push_back({param, automation::kDefaultLaneHeight});
                }
                if (!lanes.empty()) actions.setAutomationLanes(track->second, lanes);
            }
            // The master's Beat FX, added to whatever its lanes already hold after the take.
            for (const auto& [param, pts] : e.master)
            {
                std::vector<automation::Point> points;
                for (const auto& [t, v] : pts) points.push_back({t, v, stepped(param) ? "hold" : "linear"});
                if (!points.empty()) actions.setAutomation(kMasterTrackId, param, points);
            }
            // Sampler hits: a track per slot used, at the sampler's volume.
            std::map<int, juce::String> samplerTracks;
            for (const auto& h : e.samples)
            {
                auto& track = samplerTracks[h.slot];
                if (track.isEmpty())
                {
                    track = actions.addTrack();
                    actions.renameTrack(track, "Sampler " + juce::String(h.slot % 16 + 1) +
                                                   (takeNumber > 1 ? " (take " + juce::String(takeNumber) + ")" : ""));
                    if (!e.samplerLevel.empty())
                    {
                        std::vector<automation::Point> points;
                        for (const auto& [t, v] : e.samplerLevel) points.push_back({t, v});
                        actions.setAutomation(track, "level", points);
                    }
                }
                engine.send("CLIP_ADD", obj({{"trackId", track},
                                             {"clipId", EngineClient::newId("clip")},
                                             {"libraryItemId", h.itemId},
                                             {"positionMs", h.positionMs},
                                             {"inMs", 0.0},
                                             {"durationMs", h.durationMs}}));
            }
            actions.addMarker(startMs);
        });
    mirror.refreshNow();
    return true;
}

} // namespace stemcel::native::perf
