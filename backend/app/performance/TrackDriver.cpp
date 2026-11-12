#include "TrackDriver.h"

#include "Automation.h"
#include "PerformanceTake.h"
#include "performance/PerfCurves.h"
#include "performance/PerformanceEngine.h"

#include <cmath>

namespace stemcel::native::perf
{
namespace
{
namespace curves = stemcel::perf::curves;

double epsilonFor(const juce::String& param)
{
    if (param == "level" || param.startsWith("tone")) return 0.05;
    if (param == "fxBpm") return 0.05;
    if (param.endsWith("Type") || param.endsWith("Beats") || param.endsWith("On")) return 0.5;
    return 0.004;
}

bool stepped(const juce::String& param) { return param.endsWith("Type") || param.endsWith("Beats") || param.endsWith("On"); }
} // namespace

TrackDriver::TrackDriver(EngineClient& e, ProjectMirror& m, TimelineActions& a, LiveState& l)
    : engine(e), mirror(m), actions(a), live(l)
{
}

bool TrackDriver::anyBound() const
{
    for (const auto& b : bindings)
        if (b.trackId.isNotEmpty()) return true;
    return false;
}

void TrackDriver::bind(int deck, const juce::String& trackId)
{
    if (deck < 1 || deck > kDecks) return;
    auto& b = bindings[static_cast<size_t>(deck - 1)];
    if (b.trackId == trackId) return;
    if (b.trackId.isNotEmpty())
    {
        commit(recording && live.playing);
        clearLive(b.trackId);
    }
    b = Binding{};
    b.trackId = trackId;
    // The channel drives the track; the deck rests.
    auto* o = new juce::DynamicObject();
    o->setProperty("control", "channelBind");
    o->setProperty("deck", deck);
    o->setProperty("value", trackId.isNotEmpty() ? 1.0 : 0.0);
    engine.send("PERF_CONTROL", juce::var(o));
}

std::map<juce::String, double> TrackDriver::valuesFor(const State& s, int deck, double bpm)
{
    const auto& x = s.decks[static_cast<size_t>(deck - 1)];
    std::map<juce::String, double> v;
    const double gain = juce::Decibels::decibelsToGain(curves::trimDb(x.trim), curves::kSilentDb) * curves::faderGain(x.fader) *
                        curves::crossfaderGain(s.crossfader, deck == 1 ? 0 : 1, curves::CrossfaderCurve::full);
    v["level"] = juce::jlimit(-60.0, 6.0, curves::gainToDb(gain));
    v["toneBass"] = curves::eqDb(x.eqLow);
    v["toneMid"] = curves::eqDb(x.eqMid);
    v["toneTreble"] = curves::eqDb(x.eqHi);
    const bool asFilter = s.fx.colorType == 0;
    v["filter"] = asFilter ? curves::filterAmount(x.filter) : 0.0;
    v["colorFxType"] = s.fx.colorType;
    v["colorFx"] = asFilter ? 0.0 : curves::filterAmount(x.filter);
    const bool routed = s.fx.channel == deck - 1;
    v["beatFxType"] = s.fx.type;
    v["beatFxBeats"] = s.fx.step;
    v["beatFxLevel"] = s.fx.level;
    v["beatFxOn"] = routed && s.fx.on ? 1.0 : 0.0;
    if (x.padFxHeld >= 0)
    {
        const auto& preset = stemcel::perf::padFxPreset(x.padMode == "padFx2" ? 1 : 0, x.padFxHeld % 8, x.padFxHeld >= 8);
        static const int releaseTypes[3] = {12, 1, 14};
        v["padFxType"] = preset.release ? releaseTypes[juce::jlimit(0, 2, s.fx.releaseType)] : preset.type;
        v["padFxBeats"] = preset.step;
        v["padFxLevel"] = preset.level;
        v["padFxOn"] = 1.0;
    }
    else
        v["padFxOn"] = 0.0;
    v["fxBpm"] = bpm > 0.0 ? bpm : 120.0;
    return v;
}

void TrackDriver::sendLive(const juce::String& trackId, const std::map<juce::String, double>& values)
{
    auto* vals = new juce::DynamicObject();
    for (const auto& [k, value] : values) vals->setProperty(k, value);
    auto* o = new juce::DynamicObject();
    o->setProperty("trackId", trackId);
    o->setProperty("values", juce::var(vals));
    engine.send("PERF_TRACK_LIVE", juce::var(o));
}

void TrackDriver::clearLive(const juce::String& trackId)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("trackId", trackId);
    o->setProperty("all", true);
    engine.send("PERF_TRACK_LIVE", juce::var(o));
}

void TrackDriver::update(const State& state)
{
    recording = state.recording;
    const bool playing = live.playing;
    if (playing && !wasPlaying)
    {
        playStartMs = live.positionMs;
        // Controls set while stopped are written from where the timeline starts.
        if (recording)
            for (auto& b : bindings)
                for (auto& [param, touch] : b.held)
                    if (!touch.writing)
                    {
                        touch.writing = true;
                        touch.points = {{live.positionMs, b.last[param]}};
                    }
    }
    if (!playing && wasPlaying)
    {
        // The timeline stopped: what was moved is written (with REC on), the tracks go back to
        // their curves.
        commit(recording);
    }
    wasPlaying = playing;
    const auto& model = mirror.model();
    const double bpm = model.tempoCurve().bpmAt(live.positionMs);
    for (int deck = 1; deck <= kDecks; ++deck)
    {
        auto& b = bindings[static_cast<size_t>(deck - 1)];
        if (b.trackId.isEmpty()) continue;
        const auto values = valuesFor(state, deck, bpm);
        if (!b.primed)
        {
            // Where the knobs are now is not a move: only what is moved from here on acts.
            b.last = values;
            b.primed = true;
            continue;
        }
        std::map<juce::String, double> changed;
        for (const auto& [param, value] : values)
        {
            const auto last = b.last.find(param);
            const bool moved = last == b.last.end() || std::abs(last->second - value) > epsilonFor(param);
            if (moved) changed[param] = value;
        }
        // The tempo the effects follow goes along with the effects.
        if (!changed.empty() && changed.count("fxBpm") == 0) changed["fxBpm"] = values.at("fxBpm");
        if (changed.empty()) continue;
        for (const auto& [param, value] : changed)
        {
            b.last[param] = value;
            auto& touch = b.held[param];
            if (playing && recording)
            {
                touch.writing = true;
                touch.points.push_back({live.positionMs, value});
            }
        }
        sendLive(b.trackId, changed);
    }
}

void TrackDriver::commit(bool write)
{
    bool any = false;
    for (const auto& b : bindings)
        for (const auto& [param, touch] : b.held)
            any = any || (write && touch.writing && !touch.points.empty());
    const double endMs = live.positionMs;
    if (any)
        engine.inUndoGroup(
            "Live automation",
            [&]
            {
                for (auto& b : bindings)
                {
                    const auto* track = mirror.model().findTrack(b.trackId);
                    if (track == nullptr) continue;
                    auto lanes = automation::lanesOf(*track);
                    bool lanesChanged = false;
                    for (const auto& [param, touch] : b.held)
                    {
                        if (!touch.writing || touch.points.empty() || param == "fxBpm") continue;
                        const auto existing = automation::pointsOf(*track, param);
                        const auto* info = automation::find(param);
                        const double fallback = info != nullptr ? info->defaultValue : 0.0;
                        const double t0 = touch.points.front().first, t1 = std::max(endMs, touch.points.back().first);
                        std::vector<automation::Point> merged;
                        for (const auto& p : existing)
                            if (p.timeMs < t0 - 1.0) merged.push_back(p);
                        if (!existing.empty() || t0 > 1.0)
                            merged.push_back({t0 - 1.0, automation::valueAt(existing, t0, fallback), stepped(param) ? "hold" : "linear"});
                        for (const auto& [t, v] : shapeCurve(touch.points, param, t1))
                            merged.push_back({t, v, stepped(param) ? "hold" : "linear"});
                        merged.push_back({t1 + 1.0, automation::valueAt(existing, t1, fallback), stepped(param) ? "hold" : "linear"});
                        for (const auto& p : existing)
                            if (p.timeMs > t1 + 1.0) merged.push_back(p);
                        actions.setAutomation(b.trackId, param, merged);
                        if (std::none_of(lanes.begin(), lanes.end(), [&](const auto& l) { return l.paramId == param; }))
                        {
                            lanes.push_back({param, automation::kDefaultLaneHeight});
                            lanesChanged = true;
                        }
                    }
                    // The effects' tempo along with any effect written.
                    if (const auto bpm = b.held.find("fxBpm"); bpm != b.held.end() && bpm->second.writing && !bpm->second.points.empty())
                        actions.setAutomation(b.trackId, "fxBpm", {{bpm->second.points.front().first, bpm->second.points.front().second}});
                    if (lanesChanged) actions.setAutomationLanes(b.trackId, lanes);
                }
            });
    for (auto& b : bindings)
    {
        if (b.trackId.isNotEmpty() && !b.held.empty()) clearLive(b.trackId);
        b.held.clear();
    }
    if (any) mirror.refreshNow();
}

void TrackDriver::transport(const juce::String& control, double value)
{
    const bool press = value > 0.5;
    if ((control == "play" || control == "playShift") && press)
    {
        if (live.playing) actions.pause();
        else
        {
            playStartMs = live.positionMs;
            actions.play();
        }
    }
    else if (control == "cue" && press)
    {
        // CUE: back to where the timeline last started, stopped.
        if (live.playing)
        {
            actions.pause();
            actions.seek(playStartMs);
        }
        else
            playStartMs = live.positionMs;
    }
    else if (control == "cueShift" && press)
        actions.seek(0.0);
    else if ((control == "jogBend" || control == "jogScratch" || control == "jogSearch") && !live.playing)
    {
        // Stopped, the wheel moves the playhead (SHIFT: faster).
        const double perTick = control == "jogSearch" ? 100.0 : 10.0;
        actions.seek(std::max(0.0, live.positionMs + value * perTick));
    }
}

void TrackDriver::finish()
{
    commit(recording && live.playing);
    for (int deck = 1; deck <= kDecks; ++deck)
        if (bindings[static_cast<size_t>(deck - 1)].trackId.isNotEmpty()) bind(deck, {});
}

} // namespace stemcel::native::perf
