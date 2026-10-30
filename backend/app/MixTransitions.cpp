#include "MixTransitions.h"

#include "Automation.h"
#include "TimelineActions.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace stemcel::native::mix
{
namespace
{
using Points = std::vector<std::pair<double, double>>;

constexpr double kSilentDb = -60.0;
constexpr double kKillDb = -40.0;
// Where the shape hands back to the curve around it: just before the window, and just after.
constexpr double kAnchorMs = 1.0;
constexpr double kRestoreMs = 5.0;

// Equal-power fades of a level in dB over [a, b] (the crossfade a DJ mixer's curve gives):
// the two together keep the summed power steady, where linear fades dip in the middle.
Points fadeOut(double a, double b)
{
    Points p{{0.0, 0.0}};
    for (int i = 0; i <= 8; ++i)
    {
        const double u = i / 8.0;
        const double gain = std::cos(u * juce::MathConstants<double>::halfPi);
        p.push_back({a + (b - a) * u, gain > 1.0e-3 ? juce::jmax(kSilentDb, 20.0 * std::log10(gain)) : kSilentDb});
    }
    p.push_back({1.0, kSilentDb});
    return p;
}

Points fadeIn(double a, double b)
{
    Points p{{0.0, kSilentDb}};
    for (int i = 0; i <= 8; ++i)
    {
        const double u = i / 8.0;
        const double gain = std::sin(u * juce::MathConstants<double>::halfPi);
        p.push_back({a + (b - a) * u, gain > 1.0e-3 ? juce::jmax(kSilentDb, 20.0 * std::log10(gain)) : kSilentDb});
    }
    p.push_back({1.0, 0.0});
    return p;
}

// Holds `from` until a, moves in a straight line to `to` by b, holds after.
Points ramp(double a, double b, double from, double to)
{
    return {{0.0, from}, {a, from}, {b, to}, {1.0, to}};
}

// Jumps from `from` to `to` at `at` (a cut, a kill, a swap on the downbeat).
Points step(double at, double from, double to)
{
    return {{0.0, from}, {at, from}, {at, to}, {1.0, to}};
}

Preset preset(const char* id, const char* name, const char* category, const char* description, double bars,
              std::vector<Shape> out, std::vector<Shape> in)
{
    Preset p;
    p.id = id;
    p.name = name;
    p.category = category;
    p.description = description;
    p.bars = bars;
    p.out = std::move(out);
    p.in = std::move(in);
    return p;
}

std::vector<Preset> buildPresets()
{
    std::vector<Preset> list;
    auto add = [&list](Preset p, bool harmonicSafe, bool tempoGapSafe, int energy) -> Preset&
    {
        p.harmonicSafe = harmonicSafe;
        p.tempoGapSafe = tempoGapSafe;
        p.energy = energy;
        list.push_back(std::move(p));
        return list.back();
    };

    // ── Blends ─────────────────────────────────────────────────────────────────
    add(preset("smoothBlend", "Smooth Blend", "Blends",
               "A long equal-power crossfade: both tracks ride together and trade places. For tracks in key.", 16,
               {{"level", fadeOut(0.0, 1.0)}}, {{"level", fadeIn(0.0, 1.0)}}),
        false, false, 0);
    add(preset("bassSwap", "Bass Swap", "Blends",
               "The incoming track comes in without its bass; on the middle downbeat the basslines swap, then the "
               "outgoing track fades. The house and techno standard.",
               8, {{"toneBass", step(0.5, 0.0, kKillDb)}, {"level", fadeOut(0.5, 1.0)}},
               {{"toneBass", step(0.5, kKillDb, 0.0)}, {"level", fadeIn(0.0, 0.5)}}),
        false, false, 0);
    add(preset("eqBlend", "Three-Band Blend", "Blends",
               "Highs first, then mids, then a bass swap: the incoming track takes over band by band, so the two "
               "never fight in the same range.",
               16,
               {{"toneTreble", ramp(0.33, 0.66, 0.0, kKillDb)},
                {"toneMid", ramp(0.66, 0.9, 0.0, kKillDb)},
                {"toneBass", step(0.66, 0.0, kKillDb)},
                {"level", fadeOut(0.75, 1.0)}},
               {{"level", fadeIn(0.0, 0.25)},
                {"toneTreble", ramp(0.0, 0.33, kKillDb, 0.0)},
                {"toneMid", ramp(0.33, 0.66, kKillDb, 0.0)},
                {"toneBass", step(0.66, kKillDb, 0.0)}}),
        false, false, 0);
    add(preset("vocalSafeBlend", "Vocal-Safe Blend", "Blends",
               "A blend that keeps two vocals apart: the incoming mids (where voices sit) stay down until the "
               "outgoing ones drop, with a bass swap at the same moment.",
               16,
               {{"toneMid", step(0.5, 0.0, kKillDb)}, {"toneBass", step(0.5, 0.0, kKillDb)}, {"level", fadeOut(0.0, 1.0)}},
               {{"toneMid", step(0.5, kKillDb, 0.0)}, {"toneBass", step(0.5, kKillDb, 0.0)}, {"level", fadeIn(0.0, 1.0)}}),
        true, false, 0);
    add(preset("doubleDrop", "Double Drop", "Blends",
               "Both tracks at full volume, drops together, the incoming bass held off so the low end stays clean; "
               "the outgoing track drops out at the end.",
               8, {{"level", fadeOut(0.9, 1.0)}}, {{"toneBass", step(0.999, kKillDb, 0.0)}}),
        false, false, 1);
    auto& tempoRamp = add(preset("tempoRampBlend", "Tempo Ramp Blend", "Blends",
                                 "The project tempo glides from the outgoing track's to the incoming track's across the "
                                 "blend, with a bass swap in the middle: for joining tracks at different tempos without a jump.",
                                 16, {{"toneBass", step(0.5, 0.0, kKillDb)}, {"level", fadeOut(0.5, 1.0)}},
                                 {{"toneBass", step(0.5, kKillDb, 0.0)}, {"level", fadeIn(0.0, 0.5)}}),
                          false, false, 0); // the auto mixer ramps only a modest gap (under 16%)
    tempoRamp.extra = Extra::tempoRamp;
    add(preset("linearFade", "Linear Fade", "Blends",
               "A plain fade in dB, the outgoing track out as the incoming one comes in: the radio segue.", 8,
               {{"level", ramp(0.0, 1.0, 0.0, kSilentDb)}}, {{"level", ramp(0.0, 1.0, kSilentDb, 0.0)}}),
        false, true, -1);

    // ── Filters ────────────────────────────────────────────────────────────────
    add(preset("lowPassOut", "Low-Pass Filter Out", "Filters",
               "The outgoing track's highs close down until only its thump is left, as the incoming track rises "
               "in. Bright and busy to dark and minimal.",
               8, {{"filter", ramp(0.0, 0.85, 0.0, -1.0)}, {"level", fadeOut(0.85, 1.0)}},
               {{"toneBass", step(0.5, kKillDb, 0.0)}, {"level", fadeIn(0.25, 0.75)}}),
        true, false, -1);
    add(preset("highPassOut", "High-Pass Filter Out", "Filters",
               "The outgoing track thins out from the bottom up as the incoming one's bass arrives: a lift into "
               "the next track.",
               8, {{"filter", ramp(0.0, 0.85, 0.0, 1.0)}, {"level", fadeOut(0.85, 1.0)}},
               {{"toneBass", step(0.5, kKillDb, 0.0)}, {"level", fadeIn(0.0, 0.5)}}),
        true, false, 1);
    add(preset("filterSwap", "Filter Swap", "Filters",
               "Mirror sweeps: the outgoing track closes with a low-pass while the incoming one opens out of a "
               "high-pass. Two busy tracks cross without clashing.",
               8, {{"filter", ramp(0.0, 1.0, 0.0, -1.0)}, {"level", fadeOut(0.8, 1.0)}},
               {{"filter", ramp(0.0, 1.0, 1.0, 0.0)}, {"level", fadeIn(0.0, 0.3)}}),
        true, false, 0);
    add(preset("lowPassIn", "Muffled Intro", "Filters",
               "The incoming track starts behind a closed low-pass and opens up as the outgoing one fades, as if a "
               "door opened onto it.",
               8, {{"toneBass", step(0.5, 0.0, kKillDb)}, {"level", fadeOut(0.5, 1.0)}},
               {{"filter", ramp(0.0, 1.0, -1.0, 0.0)}, {"level", fadeIn(0.0, 0.25)}}),
        false, false, 1);
    add(preset("highPassIn", "Thin Intro", "Filters",
               "The incoming track starts as hats and air behind a high-pass and fills in its body while the "
               "outgoing one hands over its bass.",
               8, {{"toneBass", step(0.6, 0.0, kKillDb)}, {"level", fadeOut(0.6, 1.0)}},
               {{"filter", ramp(0.0, 0.9, 1.0, 0.0)}, {"level", fadeIn(0.0, 0.25)}}),
        false, false, 1);

    // ── FX ─────────────────────────────────────────────────────────────────────
    auto& echo = add(preset("echoOut", "Echo Out", "FX",
                            "The outgoing track is thrown into the echo and cut on the beat; its repeats ring out as the "
                            "incoming track drops. Covers any key or tempo change.",
                            2, {{"delaySend", ramp(0.55, 0.7, 0.0, 1.0)}, {"level", step(0.75, 0.0, kSilentDb)}},
                            {{"level", step(0.75, kSilentDb, 0.0)}}),
                     true, true, 0);
    echo.usesEcho = true;
    auto& wash = add(preset("reverbWash", "Reverb Wash", "FX",
                            "The outgoing track lifts into a swelling reverb and a rising high-pass, then cuts, leaving a "
                            "wash for the incoming track to land in.",
                            4, {{"reverbSend", ramp(0.4, 0.8, 0.0, 1.0)}, {"filter", ramp(0.4, 0.8, 0.0, 0.6)},
                                {"level", step(0.8, 0.0, kSilentDb)}},
                            {{"level", step(0.8, kSilentDb, 0.0)}}),
                     true, true, -1);
    wash.usesReverb = true;
    auto& crush = add(preset("crushOut", "Bitcrush Out", "FX",
                             "The outgoing track degrades into crunchy low-resolution grit, then cuts to the incoming one.",
                             2, {{"bitCrusherMix", ramp(0.4, 0.75, 0.0, 1.0)}, {"bitCrusherRate", ramp(0.4, 0.9, 1.0, 0.05)},
                                 {"level", step(0.9, 0.0, kSilentDb)}},
                             {{"level", step(0.9, kSilentDb, 0.0)}}),
                      true, true, 1);
    juce::ignoreUnused(crush);
    auto& throwDrop = add(preset("echoDrop", "Echo Into the Drop", "FX",
                                 "The outgoing track builds behind a rising high-pass and an echo throw, then stops dead as "
                                 "the incoming track hits on its drop.",
                                 4, {{"filter", ramp(0.4, 0.95, 0.0, 0.7)}, {"delaySend", ramp(0.85, 0.95, 0.0, 1.0)},
                                     {"level", step(0.999, 0.0, kSilentDb)}},
                                 {{"level", step(0.999, kSilentDb, 0.0)}}),
                          true, true, 1);
    throwDrop.usesEcho = true;

    // ── Cuts and turntable moves ───────────────────────────────────────────────
    add(preset("quickCut", "Quick Cut", "Cuts",
               "A clean swap on the downbeat. Instant, punchy, and fine for any key or tempo jump.", 1,
               {{"level", step(0.5, 0.0, kSilentDb)}}, {{"level", step(0.5, kSilentDb, 0.0)}}),
        true, true, 1);
    add(preset("bassCut", "Bass Kill Cut", "Cuts",
               "Both tracks play over the window with the incoming bass killed, then the outgoing track cuts and the "
               "incoming bass slams in together.",
               4, {{"level", step(0.999, 0.0, kSilentDb)}}, {{"toneBass", step(0.999, kKillDb, 0.0)}, {"level", fadeIn(0.0, 0.5)}}),
        false, false, 1);
    auto& brake = add(preset("brake", "Vinyl Brake", "Cuts",
                             "The outgoing record grinds to a stop at its end and the incoming track drops in after it.", 1,
                             {}, {{"level", step(0.999, kSilentDb, 0.0)}}),
                      true, true, 0);
    brake.extra = Extra::brake;
    auto& spin = add(preset("backspin", "Backspin", "Cuts",
                            "A hand-pulled spinback on the outgoing record, then the incoming track on the next downbeat.", 1,
                            {}, {{"level", step(0.999, kSilentDb, 0.0)}}),
                     true, true, 1);
    spin.extra = Extra::backspin;
    auto& roll = add(preset("loopRoll", "Loop Roll", "Cuts",
                            "The outgoing track's last bar stutters in shrinking loops (quarters, eighths, sixteenths) "
                            "under a rising filter, then the incoming track drops.",
                            1, {{"filter", ramp(0.0, 0.98, 0.0, 0.5)}}, {{"level", step(0.999, kSilentDb, 0.0)}}),
                     true, true, 1);
    roll.extra = Extra::loopRoll;
    return list;
}

double defaultFor(const juce::String& param)
{
    if (const auto* p = automation::find(param)) return p->defaultValue;
    return 0.0;
}

// Writes `points` (window-relative) over [start, end] on one track's parameter, keeping its curve
// outside the window and handing back to it on both sides.
void writeShape(TimelineActions& actions, const TrackModel& track, const Shape& shape, double start, double end)
{
    const auto existing = automation::pointsOf(track, shape.param);
    const double fallback = defaultFor(shape.param);
    const double before = automation::valueAt(existing, start - kAnchorMs, fallback);
    const double after = automation::valueAt(existing, end + kRestoreMs, fallback);
    std::vector<automation::Point> next;
    for (const auto& p : existing)
        if (p.timeMs < start - kAnchorMs || p.timeMs > end + kRestoreMs) next.push_back(p);
    next.push_back({start - kAnchorMs, before});
    double lastTime = -1.0;
    if (!shape.shaped.empty())
    {
        // Drawn by hand: its points as they are, with their segment shapes.
        for (auto p : shape.shaped)
        {
            double time = start + juce::jlimit(0.0, 1.0, p.timeMs) * (end - start);
            if (time <= lastTime) time = lastTime + 1.0;
            p.timeMs = time;
            next.push_back(p);
            lastTime = time;
        }
        next.push_back({juce::jmax(end + kRestoreMs, lastTime + 1.0), after});
        actions.setAutomation(track.id, shape.param, next);
        return;
    }
    for (const auto& [t, v] : shape.points)
    {
        double time = start + juce::jlimit(0.0, 1.0, t) * (end - start);
        if (time <= lastTime) time = lastTime + 1.0; // a step: the second value a millisecond on
        next.push_back({time, v});
        lastTime = time;
    }
    next.push_back({juce::jmax(end + kRestoreMs, lastTime + 1.0), after});
    actions.setAutomation(track.id, shape.param, next);
}

// Takes a parameter's shape back out of the window (the curve either side joins up again).
void clearShape(TimelineActions& actions, const TrackModel& track, const juce::String& param, double start, double end)
{
    const auto existing = automation::pointsOf(track, param);
    std::vector<automation::Point> next;
    for (const auto& p : existing)
        if (p.timeMs < start - kAnchorMs - 0.5 || p.timeMs > end + kRestoreMs + 1.5) next.push_back(p);
    if (next.size() != existing.size()) actions.setAutomation(track.id, param, next);
}

double sourceBpmOf(const ProjectModel& model, const ClipModel& clip)
{
    for (const auto* item = model.findLibraryItem(clip.libraryItemId); item != nullptr;
         item = item->sourceItemId.isNotEmpty() ? model.findLibraryItem(item->sourceItemId) : nullptr)
        if (item->bpm > 0.0) return item->bpm;
    return 0.0;
}
} // namespace

namespace
{
juce::String& transitionClipboard()
{
    static juce::String preset;
    return preset;
}
} // namespace

void copyPreset(const juce::String& presetId)
{
    transitionClipboard() = presetId;
}

juce::String copiedPreset()
{
    return transitionClipboard();
}

const std::vector<Preset>& presets()
{
    static const std::vector<Preset> list = buildPresets();
    return list;
}

const Preset* find(const juce::String& id)
{
    for (const auto& p : presets())
        if (p.id == id) return &p;
    return nullptr;
}

juce::StringArray categories()
{
    juce::StringArray list;
    for (const auto& p : presets()) list.addIfNotAlreadyThere(p.category);
    return list;
}

std::optional<std::pair<double, double>> overlap(const ClipModel& out, const ClipModel& in)
{
    if (out.trackId == in.trackId || out.id == in.id) return std::nullopt;
    const double start = in.offsetMs;
    const double end = out.endMs();
    if (!(start > out.offsetMs) || !(end > start) || !(in.endMs() > end)) return std::nullopt;
    return std::make_pair(start, end);
}

std::vector<std::pair<juce::String, juce::String>> pairsFor(const ProjectModel& model, const juce::String& clipId)
{
    std::vector<std::pair<juce::String, juce::String>> pairs;
    const auto* clip = model.findClip(clipId);
    if (clip == nullptr) return pairs;
    for (const auto& track : model.tracks)
        for (const auto& other : track.clips)
        {
            if (overlap(*clip, other).has_value()) pairs.emplace_back(clip->id, other.id);
            if (overlap(other, *clip).has_value()) pairs.emplace_back(other.id, clip->id);
        }
    return pairs;
}

const juce::var* recordFor(const ProjectModel& model, const juce::String& outClipId, const juce::String& inClipId)
{
    if (const auto* list = model.raw.getProperty("mixTransitions", {}).getArray())
        for (const auto& t : *list)
        {
            const auto a = t.getProperty("outClipId", {}).toString();
            const auto b = t.getProperty("inClipId", {}).toString();
            if ((a == outClipId && b == inClipId) || (a == inClipId && b == outClipId)) return &t;
        }
    return nullptr;
}

namespace
{
// The tempo map ramps from the outgoing tempo to the incoming track's over the window, and the
// outgoing clip gets the source that still ends it on the window.
void rampTempo(EngineClient& engine, TimelineActions& actions, const ProjectModel& model, const ClipModel& out, const ClipModel& in,
               double start, double end)
{
    const double target = sourceBpmOf(model, in);
    if (target <= 0.0) return;
    auto points = model.tempoMap;
    points.erase(std::remove_if(points.begin(), points.end(),
                                [&](const TempoPoint& p) { return p.timeMs >= start - 0.5 && p.timeMs <= end + 0.5; }),
                 points.end());
    points.push_back({start, model.tempoCurve().bpmAt(start)});
    points.push_back({end, target});
    actions.setTempoMap(points);
    // The outgoing clip now speeds up or slows down along the ramp, so it would end early or
    // late: give it the source that ends it on the window.
    const double outBpm = sourceBpmOf(model, out);
    if (out.warpEnabled && outBpm > 0.0 && !out.raw.hasProperty("tempoRatio"))
    {
        const TempoCurve next(points, model.bpm);
        auto* trim = new juce::DynamicObject();
        trim->setProperty("clipId", out.id);
        trim->setProperty("startMs", out.offsetMs);
        trim->setProperty("inMs", out.inMs);
        trim->setProperty("durationMs", next.sourceMsBetween(out.offsetMs, end, outBpm));
        engine.send("CLIP_TRIM", juce::var(trim));
    }
}

void undoTempoRamp(TimelineActions& actions, const ProjectModel& model, const ClipModel* out, double start, double end)
{
    if (model.tempoMap.empty()) return;
    std::vector<TempoPoint> next;
    for (const auto& p : model.tempoMap)
        if (std::abs(p.timeMs - start) > 0.5 && std::abs(p.timeMs - end) > 0.5) next.push_back(p);
    if (next.size() == model.tempoMap.size()) return;
    actions.setTempoMap(next);
    // The ramp had re-trimmed the outgoing clip to end on the window; without it, the source
    // that ends it there is different again.
    const double outBpm = out != nullptr ? sourceBpmOf(model, *out) : 0.0;
    if (out != nullptr && out->warpEnabled && outBpm > 0.0 && !out->raw.hasProperty("tempoRatio"))
    {
        const TempoCurve curve(next, model.bpm);
        auto* trim = new juce::DynamicObject();
        trim->setProperty("clipId", out->id);
        trim->setProperty("startMs", out->offsetMs);
        trim->setProperty("inMs", out->inMs);
        trim->setProperty("durationMs", curve.sourceMsBetween(out->offsetMs, end, outBpm));
        actions.engineClient().send("CLIP_TRIM", juce::var(trim));
    }
}

void applyExtra(TimelineActions& actions, const ProjectModel& model, const TrackModel& outTrack, const juce::String& outClipId,
                Extra extra, double end)
{
    switch (extra)
    {
        case Extra::brake: actions.setClipBrake(outClipId, true); break;
        case Extra::backspin: actions.setClipBackspin(outClipId, true); break;
        case Extra::loopRoll:
        {
            // The last bar, in shrinking repeats.
            const auto curve = model.tempoCurve();
            const double endBeat = std::round(curve.beatAt(end));
            actions.addBeatRepeat(outTrack.id, endBeat - 4.0, 2.0, "1/4");
            actions.addBeatRepeat(outTrack.id, endBeat - 2.0, 1.0, "1/8");
            actions.addBeatRepeat(outTrack.id, endBeat - 1.0, 1.0, "1/16");
            break;
        }
        case Extra::tempoRamp:
        case Extra::none: break;
    }
}

void undoExtra(TimelineActions& actions, const ProjectModel& model, const TrackModel& outTrack, const juce::String& outClipId,
               Extra extra, double start, double end)
{
    if (extra == Extra::brake) actions.setClipBrake(outClipId, false);
    if (extra == Extra::backspin) actions.setClipBackspin(outClipId, false);
    if (extra == Extra::loopRoll)
        if (const auto* regions = outTrack.raw.getProperty("beatRepeats", {}).getArray())
        {
            const auto curve = model.tempoCurve();
            for (const auto& r : *regions)
            {
                const double at = curve.msAtBeat(static_cast<double>(r.getProperty("startBeat", 0.0)));
                if (at >= start - 1.0 && at < end + 1.0) actions.deleteBeatRepeat(outTrack.id, r.getProperty("id", {}).toString());
            }
        }
}

// An unconfigured project echo or reverb would make a throw silent: give it a DJ one.
void readySends(EngineClient& engine, const ProjectModel& model, bool echo, bool reverb)
{
    if (echo && static_cast<double>(model.raw.getProperty("delayMix", 0.0)) < 0.05)
    {
        auto* d = new juce::DynamicObject();
        d->setProperty("noteValue", "1/4");
        d->setProperty("feedback", 0.55);
        d->setProperty("mix", 0.85);
        engine.send("PROJECT_SET_DELAY", juce::var(d));
    }
    if (reverb && static_cast<double>(model.raw.getProperty("reverbMix", 0.0)) < 0.05)
    {
        auto* r = new juce::DynamicObject();
        r->setProperty("size", 0.85);
        r->setProperty("decay", 0.75);
        r->setProperty("mix", 0.85);
        engine.send("PROJECT_SET_REVERB", juce::var(r));
    }
}

// The parts of `remove` that undo a transition's sound, shared with re-applying.
void undoEffect(TimelineActions& actions, const ProjectModel& model, const juce::var& record)
{
    const auto* out = model.findClip(record.getProperty("outClipId", {}).toString());
    const auto* in = model.findClip(record.getProperty("inClipId", {}).toString());
    const auto* outTrack = out != nullptr ? model.findTrack(out->trackId) : nullptr;
    const auto* inTrack = in != nullptr ? model.findTrack(in->trackId) : nullptr;
    const double start = static_cast<double>(record.getProperty("startMs", 0.0));
    const double end = static_cast<double>(record.getProperty("endMs", 0.0));
    if (record.getProperty("settings", {}).isObject())
    {
        const auto join = joinOf(record);
        if (join.out.has_value() && outTrack != nullptr)
        {
            for (const auto& s : shapesFor(*join.out, true)) clearShape(actions, *outTrack, s.param, start, end);
            undoExtra(actions, model, *outTrack, out->id, join.out->extra, start, end);
        }
        if (join.in.has_value() && inTrack != nullptr)
            for (const auto& s : shapesFor(*join.in, false)) clearShape(actions, *inTrack, s.param, start, end);
        if (join.rampTempo) undoTempoRamp(actions, model, out, start, end);
        return;
    }
    const auto* preset = find(record.getProperty("preset", {}).toString());
    if (preset == nullptr) return;
    if (outTrack != nullptr)
    {
        for (const auto& s : preset->out) clearShape(actions, *outTrack, s.param, start, end);
        undoExtra(actions, model, *outTrack, out->id, preset->extra, start, end);
    }
    if (inTrack != nullptr)
        for (const auto& s : preset->in) clearShape(actions, *inTrack, s.param, start, end);
    if (preset->extra == Extra::tempoRamp) undoTempoRamp(actions, model, out, start, end);
}
} // namespace

bool apply(EngineClient& engine, TimelineActions& actions, const ProjectModel& model, const juce::String& outClipId,
           const juce::String& inClipId, const juce::String& presetId, std::optional<std::pair<double, double>> window)
{
    const auto* preset = find(presetId);
    const auto* out = model.findClip(outClipId);
    const auto* in = model.findClip(inClipId);
    if (preset == nullptr || out == nullptr || in == nullptr) return false;
    if (!window.has_value()) window = overlap(*out, *in);
    if (!window.has_value()) return false;
    // Plain copies: lambdas below capture them (C++17 cannot capture structured bindings).
    const double start = window->first;
    const double end = window->second;
    const auto* outTrack = model.findTrack(out->trackId);
    const auto* inTrack = model.findTrack(in->trackId);
    if (outTrack == nullptr || inTrack == nullptr) return false;

    const auto* existing = recordFor(model, outClipId, inClipId);
    const auto id = existing != nullptr ? existing->getProperty("id", {}).toString() : juce::Uuid().toDashedString();
    engine.inUndoGroup("Transition: " + preset->name,
                       [&]
                       {
                           if (existing != nullptr) undoEffect(actions, model, *existing);
                           for (const auto& s : preset->out) writeShape(actions, *outTrack, s, start, end);
                           for (const auto& s : preset->in) writeShape(actions, *inTrack, s, start, end);
                           if (preset->extra == Extra::tempoRamp) rampTempo(engine, actions, model, *out, *in, start, end);
                           else applyExtra(actions, model, *outTrack, outClipId, preset->extra, end);
                           readySends(engine, model, preset->usesEcho, preset->usesReverb);
                           auto* rec = new juce::DynamicObject();
                           rec->setProperty("id", id);
                           rec->setProperty("outClipId", outClipId);
                           rec->setProperty("inClipId", inClipId);
                           rec->setProperty("preset", presetId);
                           rec->setProperty("startMs", start);
                           rec->setProperty("endMs", end);
                           engine.send("MIX_TRANSITION_SET", juce::var(rec));
                       });
    return true;
}

void remove(EngineClient& engine, TimelineActions& actions, const ProjectModel& model, const juce::String& transitionId)
{
    if (const auto* list = model.raw.getProperty("mixTransitions", {}).getArray())
        for (const auto& t : *list)
            if (t.getProperty("id", {}).toString() == transitionId)
            {
                const auto* preset = find(t.getProperty("preset", {}).toString());
                engine.inUndoGroup("Remove Transition" + (preset != nullptr ? ": " + preset->name : juce::String()),
                                   [&]
                                   {
                                       undoEffect(actions, model, t);
                                       auto* o = new juce::DynamicObject();
                                       o->setProperty("id", transitionId);
                                       engine.send("MIX_TRANSITION_REMOVE", juce::var(o));
                                   });
                return;
            }
}

// ── Outs and ins ─────────────────────────────────────────────────────────────
namespace
{
const char* extraName(Extra e)
{
    switch (e)
    {
        case Extra::brake: return "brake";
        case Extra::backspin: return "backspin";
        case Extra::loopRoll: return "loopRoll";
        case Extra::tempoRamp: return "tempoRamp";
        case Extra::none: break;
    }
    return "none";
}

Extra extraFrom(const juce::String& name)
{
    if (name == "brake") return Extra::brake;
    if (name == "backspin") return Extra::backspin;
    if (name == "loopRoll") return Extra::loopRoll;
    return Extra::none;
}

// How far along a move is at u (0..1) for each curve shape: equal power eases out, fast does
// most of it early, slow holds then goes, an S-curve eases both ends.
double shaped(const juce::String& curve, double u)
{
    u = juce::jlimit(0.0, 1.0, u);
    if (curve == "linear") return u;
    if (curve == "fast") return 1.0 - std::pow(1.0 - u, 3.0);
    if (curve == "slow") return u * u * u;
    if (curve == "sCurve") return 0.5 - 0.5 * std::cos(u * juce::MathConstants<double>::pi);
    return std::sin(u * juce::MathConstants<double>::halfPi);
}

constexpr int kCurveSteps = 12;

// A move from `from` to `to` over [a, b] along the curve, held either side.
Points curved(double a, double b, double from, double to, const juce::String& curve)
{
    if (b - a < 1.0e-4) return step(a, from, to);
    if (curve == "linear") return ramp(a, b, from, to);
    Points p{{0.0, from}};
    for (int i = 0; i <= kCurveSteps; ++i)
    {
        const double u = static_cast<double>(i) / kCurveSteps;
        p.push_back({a + (b - a) * u, from + (to - from) * shaped(curve, u)});
    }
    p.push_back({1.0, to});
    return p;
}

// The fader over [a, b], in dB. The curve shapes the gain: equal power is the DJ mixer's
// constant-power fade (cos out, sin in), linear a straight line in dB like a radio segue.
Points levelFade(double a, double b, const juce::String& curve, bool outgoing)
{
    const double from = outgoing ? 0.0 : kSilentDb, to = outgoing ? kSilentDb : 0.0;
    if (b - a < 1.0e-4) return step(a, from, to);
    if (curve == "linear") return ramp(a, b, from, to);
    Points p{{0.0, from}};
    for (int i = 0; i <= kCurveSteps; ++i)
    {
        const double u = static_cast<double>(i) / kCurveSteps;
        // Equal power: the out side follows cos, which is the in side's sin mirrored.
        const double moved = curve == "equalPower" ? (outgoing ? 1.0 - std::cos(u * juce::MathConstants<double>::halfPi)
                                                               : std::sin(u * juce::MathConstants<double>::halfPi))
                                                   : shaped(curve, u);
        const double gain = outgoing ? 1.0 - moved : moved;
        p.push_back({a + (b - a) * u, gain > 1.0e-3 ? juce::jmax(kSilentDb, 20.0 * std::log10(gain)) : kSilentDb});
    }
    p.push_back({1.0, to});
    return p;
}

Half half(const char* kind, double from, double to, const char* level, const char* curve = "equalPower")
{
    Half h;
    h.kind = kind;
    h.from = from;
    h.to = to;
    h.level = level;
    h.curve = curve;
    return h;
}

std::vector<HalfKind> buildHalfKinds()
{
    std::vector<HalfKind> list;
    auto add = [&list](const char* name, const char* description, bool outgoing, Half h) -> Half&
    {
        list.push_back({h.kind, name, description, outgoing, h});
        return list.back().defaults;
    };
    // ── Outs ──
    add("Fade Out", "An equal-power fade out across the window.", true, half("fadeOut", 0.0, 1.0, "fade"));
    {
        auto& h = add("Bass Swap Out", "Gives up its bass on the swap point, then fades.", true, half("bassSwapOut", 0.5, 1.0, "fade"));
        h.low = "swap";
    }
    {
        auto& h = add("EQ Fade Out", "Highs, then mids sweep away, the bass swaps out, then the fader closes.", true,
                      half("eqOut", 0.33, 1.0, "edge", "linear"));
        h.high = "sweep";
        h.mid = "sweep";
        h.low = "swap";
        h.swapAt = 0.66;
    }
    {
        auto& h = add("Low-Pass Out", "Its highs close down until only the thump is left, then it fades.", true,
                      half("lowPassOut", 0.0, 1.0, "edge", "slow"));
        h.filter = "lowPass";
    }
    {
        auto& h = add("High-Pass Out", "Thins out from the bottom up, then fades: a lift.", true, half("highPassOut", 0.0, 1.0, "edge", "slow"));
        h.filter = "highPass";
    }
    {
        auto& h = add("Echo Out", "Thrown into the echo and cut on the beat; the repeats ring on.", true, half("echoOut", 0.55, 0.75, "cut"));
        h.effect = "echo";
    }
    {
        auto& h = add("Reverb Wash", "Swells into reverb behind a rising high-pass, then cuts.", true, half("reverbOut", 0.4, 0.8, "cut"));
        h.effect = "reverb";
        h.filter = "highPass";
        h.filterDepth = 0.6;
    }
    {
        auto& h = add("Bitcrush Out", "Degrades into low-resolution grit, then cuts.", true, half("crushOut", 0.4, 0.9, "cut"));
        h.effect = "crush";
    }
    add("Cut", "Stops dead on the downbeat.", true, half("cutOut", 0.5, 0.5, "cut"));
    {
        auto& h = add("Vinyl Brake", "The record grinds to a stop at the clip's end.", true, half("brakeOut", 1.0, 1.0, "none"));
        h.extra = Extra::brake;
    }
    {
        auto& h = add("Backspin", "A hand-pulled spinback at the clip's end.", true, half("backspinOut", 1.0, 1.0, "none"));
        h.extra = Extra::backspin;
    }
    {
        auto& h = add("Loop Roll", "The last bar stutters in shrinking loops under a rising filter.", true,
                      half("loopRollOut", 0.0, 0.98, "none", "linear"));
        h.extra = Extra::loopRoll;
        h.filter = "highPass";
        h.filterDepth = 0.5;
    }
    // ── Ins ──
    add("Fade In", "An equal-power fade in across the window.", false, half("fadeIn", 0.0, 1.0, "fade"));
    {
        auto& h = add("Bass Swap In", "Comes in without its bass, which swaps in on the swap point.", false, half("bassSwapIn", 0.0, 0.5, "fade"));
        h.low = "swap";
    }
    {
        auto& h = add("EQ Fade In", "Highs first, then mids, then the bass swaps in.", false, half("eqIn", 0.0, 0.66, "edge", "linear"));
        h.high = "sweep";
        h.mid = "sweep";
        h.low = "swap";
        h.swapAt = 0.66;
    }
    {
        auto& h = add("Muffled Intro", "Starts behind a closed low-pass that opens up.", false, half("lowPassIn", 0.0, 1.0, "edge", "slow"));
        h.filter = "lowPass";
    }
    {
        auto& h = add("Thin Intro", "Starts as hats and air behind a high-pass and fills in its body.", false,
                      half("highPassIn", 0.0, 0.9, "edge", "slow"));
        h.filter = "highPass";
    }
    {
        auto& h = add("Bass Held", "Plays at full level with its bass held back until the swap point (a double drop).", false,
                      half("bassHeldIn", 0.0, 1.0, "none"));
        h.low = "swap";
        h.swapAt = 0.999;
    }
    add("Drop In", "Silent, then in at full level at the end of the window.", false, half("dropIn", 0.999, 1.0, "cut"));
    add("Cut In", "Cuts in on the downbeat.", false, half("cutIn", 0.5, 0.5, "cut"));
    return list;
}

Half kindDefaults(const juce::String& id)
{
    if (const auto* k = findHalf(id)) return k->defaults;
    return half(id.toRawUTF8(), 0.0, 1.0, "fade");
}
} // namespace

juce::var Half::toVar() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty("kind", kind);
    o->setProperty("from", from);
    o->setProperty("to", to);
    o->setProperty("curve", curve);
    o->setProperty("level", level);
    o->setProperty("filter", filter);
    o->setProperty("filterDepth", filterDepth);
    o->setProperty("low", low);
    o->setProperty("mid", mid);
    o->setProperty("high", high);
    o->setProperty("swapAt", swapAt);
    o->setProperty("killDb", killDb);
    o->setProperty("effect", effect);
    o->setProperty("extra", extraName(extra));
    if (!manual.empty())
    {
        auto* drawn = new juce::DynamicObject();
        for (const auto& [param, points] : manual)
        {
            juce::Array<juce::var> list;
            for (const auto& p : points)
            {
                auto* point = new juce::DynamicObject();
                point->setProperty("at", p.timeMs);
                point->setProperty("value", p.value);
                point->setProperty("curve", p.curve);
                point->setProperty("tension", p.tension);
                list.add(juce::var(point));
            }
            drawn->setProperty(param, list);
        }
        o->setProperty("manual", juce::var(drawn));
    }
    return juce::var(o);
}

Half Half::fromVar(const juce::var& v)
{
    Half h = kindDefaults(v.getProperty("kind", {}).toString());
    auto num = [&v](const char* key, double fallback) { return static_cast<double>(v.getProperty(key, fallback)); };
    auto str = [&v](const char* key, const juce::String& fallback) { return v.getProperty(key, fallback).toString(); };
    h.from = juce::jlimit(0.0, 1.0, num("from", h.from));
    h.to = juce::jlimit(h.from, 1.0, num("to", h.to));
    h.curve = str("curve", h.curve);
    h.level = str("level", h.level);
    h.filter = str("filter", h.filter);
    h.filterDepth = juce::jlimit(0.0, 1.0, num("filterDepth", h.filterDepth));
    h.low = str("low", h.low);
    h.mid = str("mid", h.mid);
    h.high = str("high", h.high);
    h.swapAt = juce::jlimit(0.0, 1.0, num("swapAt", h.swapAt));
    h.killDb = juce::jlimit(-40.0, -3.0, num("killDb", h.killDb));
    h.effect = str("effect", h.effect);
    h.extra = extraFrom(str("extra", extraName(h.extra)));
    if (const auto* drawn = v.getProperty("manual", {}).getDynamicObject())
        for (const auto& [param, list] : drawn->getProperties())
            if (const auto* points = list.getArray())
            {
                std::vector<automation::Point> curve;
                for (const auto& p : *points)
                    curve.push_back({juce::jlimit(0.0, 1.0, static_cast<double>(p.getProperty("at", 0.0))),
                                     static_cast<double>(p.getProperty("value", 0.0)), p.getProperty("curve", "linear").toString(),
                                     juce::jlimit(-1.0, 1.0, static_cast<double>(p.getProperty("tension", 0.0)))});
                std::sort(curve.begin(), curve.end(), [](const auto& a, const auto& b) { return a.timeMs < b.timeMs; });
                if (curve.size() >= 2) h.manual[param.toString()] = std::move(curve);
            }
    return h;
}

const std::vector<HalfKind>& halfKinds()
{
    static const std::vector<HalfKind> list = buildHalfKinds();
    return list;
}

const HalfKind* findHalf(const juce::String& id)
{
    for (const auto& k : halfKinds())
        if (k.id == id) return &k;
    return nullptr;
}

std::vector<Shape> shapesFor(const Half& h, bool outgoing)
{
    std::vector<Shape> list;
    const double a = juce::jlimit(0.0, 1.0, h.from);
    const double b = juce::jlimit(a, 1.0, h.to);
    if (h.level == "fade" || h.level == "edge")
    {
        double fa = a, fb = b;
        if (h.level == "edge")
        {
            if (outgoing) fa = a + 0.75 * (b - a);
            else fb = a + 0.25 * (b - a);
        }
        list.push_back({"level", levelFade(fa, fb, h.curve, outgoing)});
    }
    else if (h.level == "cut")
    {
        list.push_back({"level", outgoing ? step(b, 0.0, kSilentDb) : step(a, kSilentDb, 0.0)});
    }
    if (h.filter == "lowPass" || h.filter == "highPass")
    {
        const double open = 0.0, closed = (h.filter == "lowPass" ? -1.0 : 1.0) * juce::jlimit(0.0, 1.0, h.filterDepth);
        list.push_back({"filter", curved(a, b, outgoing ? open : closed, outgoing ? closed : open, h.curve)});
    }
    const double kill = juce::jlimit(-40.0, -3.0, h.killDb);
    const std::pair<juce::String, const char*> bands[] = {{h.low, "toneBass"}, {h.mid, "toneMid"}, {h.high, "toneTreble"}};
    for (const auto& [move, param] : bands)
    {
        if (move == "swap") list.push_back({param, outgoing ? step(h.swapAt, 0.0, kill) : step(h.swapAt, kill, 0.0)});
        else if (move == "sweep") list.push_back({param, curved(a, b, outgoing ? 0.0 : kill, outgoing ? kill : 0.0, h.curve)});
    }
    if (outgoing)
    {
        if (h.effect == "echo") list.push_back({"delaySend", ramp(a, a + 0.75 * (b - a), 0.0, 1.0)});
        else if (h.effect == "reverb") list.push_back({"reverbSend", ramp(a, b, 0.0, 1.0)});
        else if (h.effect == "crush")
        {
            list.push_back({"bitCrusherMix", ramp(a, a + 0.7 * (b - a), 0.0, 1.0)});
            list.push_back({"bitCrusherRate", ramp(a, b, 1.0, 0.05)});
        }
    }
    // Curves drawn by hand replace what the options write for their parameter.
    for (const auto& [param, points] : h.manual)
    {
        if (points.size() < 2) continue;
        list.erase(std::remove_if(list.begin(), list.end(), [&](const Shape& s) { return s.param == param; }), list.end());
        Shape drawn{param, {}, points};
        constexpr int kSamples = 64;
        for (int i = 0; i <= kSamples; ++i)
        {
            const double t = static_cast<double>(i) / kSamples;
            drawn.points.emplace_back(t, automation::valueAt(points, t, points.front().value));
        }
        list.push_back(std::move(drawn));
    }
    return list;
}

std::vector<automation::Point> manualSeed(const Half& h, bool outgoing, const juce::String& param)
{
    auto plain = h;
    plain.manual.erase(param);
    for (const auto& shape : shapesFor(plain, outgoing))
    {
        if (shape.param != param || shape.points.size() < 2) continue;
        std::vector<automation::Point> seed;
        if (shape.points.size() <= 4)
        {
            // A step or a ramp: its points, a step held until it moves.
            for (size_t i = 0; i < shape.points.size(); ++i)
            {
                const auto& [t, v] = shape.points[i];
                const bool stepNext = i + 1 < shape.points.size() && std::abs(shape.points[i + 1].first - t) < 1.0e-9;
                seed.push_back({juce::jlimit(0.0, 1.0, t + (i > 0 && std::abs(shape.points[i - 1].first - t) < 1.0e-9 ? 0.001 : 0.0)),
                                v, stepNext ? "hold" : "linear", 0.0});
            }
        }
        else
        {
            // A curve: its ends and middle, smoothly joined.
            const auto& first = shape.points.front();
            const auto& middle = shape.points[shape.points.size() / 2];
            const auto& last = shape.points.back();
            seed = {{first.first, first.second, "smooth", 0.0}, {middle.first, middle.second, "smooth", 0.0},
                    {last.first, last.second, "linear", 0.0}};
        }
        return seed;
    }
    const double value = defaultFor(param);
    const double a = juce::jlimit(0.0, 1.0, h.from), b = juce::jlimit(a, 1.0, h.to);
    return {{a, value, "linear", 0.0}, {juce::jmax(b, a + 0.05), value, "linear", 0.0}};
}

Join joinForPreset(const juce::String& presetId)
{
    Join j;
    auto with = [](const char* kind, std::function<void(Half&)> tweak = {})
    {
        auto h = kindDefaults(kind);
        if (tweak) tweak(h);
        return h;
    };
    const auto& id = presetId;
    if (id == "bassSwap") j = {with("bassSwapOut"), with("bassSwapIn"), false};
    else if (id == "eqBlend") j = {with("eqOut"), with("eqIn"), false};
    else if (id == "vocalSafeBlend")
        j = {with("fadeOut", [](Half& h) { h.mid = h.low = "swap"; }), with("fadeIn", [](Half& h) { h.mid = h.low = "swap"; }), false};
    else if (id == "doubleDrop") j = {with("fadeOut", [](Half& h) { h.from = 0.9; }), with("bassHeldIn"), false};
    else if (id == "tempoRampBlend") j = {with("bassSwapOut"), with("bassSwapIn"), true};
    else if (id == "linearFade")
        j = {with("fadeOut", [](Half& h) { h.curve = "linear"; }), with("fadeIn", [](Half& h) { h.curve = "linear"; }), false};
    else if (id == "lowPassOut" || id == "highPassOut")
        j = {with(id.toRawUTF8()), with("bassSwapIn", [](Half& h) { h.from = 0.25; h.to = 0.75; }), false};
    else if (id == "filterSwap") j = {with("lowPassOut"), with("highPassIn", [](Half& h) { h.to = 1.0; }), false};
    else if (id == "lowPassIn") j = {with("bassSwapOut"), with("lowPassIn"), false};
    else if (id == "highPassIn") j = {with("bassSwapOut", [](Half& h) { h.from = h.swapAt = 0.6; }), with("highPassIn"), false};
    else if (id == "echoOut") j = {with("echoOut"), with("cutIn", [](Half& h) { h.from = h.to = 0.75; }), false};
    else if (id == "reverbWash") j = {with("reverbOut"), with("cutIn", [](Half& h) { h.from = h.to = 0.8; }), false};
    else if (id == "crushOut") j = {with("crushOut"), with("cutIn", [](Half& h) { h.from = h.to = 0.9; }), false};
    else if (id == "echoDrop")
        j = {with("echoOut", [](Half& h) { h.from = 0.4; h.to = 0.999; h.filter = "highPass"; h.filterDepth = 0.7; }), with("dropIn"), false};
    else if (id == "quickCut") j = {with("cutOut"), with("cutIn"), false};
    else if (id == "bassCut")
        j = {with("cutOut", [](Half& h) { h.from = h.to = 0.999; }), with("fadeIn", [](Half& h) { h.to = 0.5; h.low = "swap"; h.swapAt = 0.999; }), false};
    else if (id == "brake") j = {with("brakeOut"), with("dropIn"), false};
    else if (id == "backspin") j = {with("backspinOut"), with("dropIn"), false};
    else if (id == "loopRoll") j = {with("loopRollOut"), with("dropIn"), false};
    else j = {with("fadeOut"), with("fadeIn"), false};
    return j;
}

Join joinOf(const juce::var& record)
{
    const auto settings = record.getProperty("settings", {});
    if (!settings.isObject()) return joinForPreset(record.getProperty("preset", {}).toString());
    Join j;
    if (settings.getProperty("out", {}).isObject()) j.out = Half::fromVar(settings.getProperty("out", {}));
    if (settings.getProperty("in", {}).isObject()) j.in = Half::fromVar(settings.getProperty("in", {}));
    j.rampTempo = static_cast<bool>(settings.getProperty("rampTempo", false));
    return j;
}

juce::String labelOf(const juce::var& record)
{
    if (!record.getProperty("settings", {}).isObject())
    {
        const auto* preset = find(record.getProperty("preset", {}).toString());
        return preset != nullptr ? preset->name : juce::String("Transition");
    }
    const auto join = joinOf(record);
    auto name = [](const std::optional<Half>& h)
    {
        if (!h.has_value()) return juce::String();
        const auto* k = findHalf(h->kind);
        return k != nullptr ? k->name : juce::String("Custom");
    };
    const auto out = name(join.out), in = name(join.in);
    if (out.isNotEmpty() && in.isNotEmpty()) return out + " + " + in;
    return out.isNotEmpty() ? out : in.isNotEmpty() ? in : juce::String("Transition");
}

bool applyJoin(EngineClient& engine, TimelineActions& actions, const ProjectModel& model, const juce::String& outClipId,
               const juce::String& inClipId, const Join& join)
{
    const auto* out = model.findClip(outClipId);
    const auto* in = model.findClip(inClipId);
    if (out == nullptr || in == nullptr) return false;
    const auto window = overlap(*out, *in);
    if (!window.has_value()) return false;
    const double start = window->first;
    const double end = window->second;
    const auto* outTrack = model.findTrack(out->trackId);
    const auto* inTrack = model.findTrack(in->trackId);
    if (outTrack == nullptr || inTrack == nullptr) return false;
    const auto* existing = recordFor(model, outClipId, inClipId);
    const auto id = existing != nullptr ? existing->getProperty("id", {}).toString() : juce::Uuid().toDashedString();

    auto* settings = new juce::DynamicObject();
    if (join.out.has_value()) settings->setProperty("out", join.out->toVar());
    if (join.in.has_value()) settings->setProperty("in", join.in->toVar());
    settings->setProperty("rampTempo", join.rampTempo);
    juce::var record(new juce::DynamicObject());
    record.getDynamicObject()->setProperty("settings", juce::var(settings));
    engine.inUndoGroup("Transition: " + labelOf(record),
                       [&]
                       {
                           if (existing != nullptr) undoEffect(actions, model, *existing);
                           if (join.out.has_value())
                           {
                               for (const auto& s : shapesFor(*join.out, true)) writeShape(actions, *outTrack, s, start, end);
                               applyExtra(actions, model, *outTrack, outClipId, join.out->extra, end);
                           }
                           if (join.in.has_value())
                               for (const auto& s : shapesFor(*join.in, false)) writeShape(actions, *inTrack, s, start, end);
                           if (join.rampTempo) rampTempo(engine, actions, model, *out, *in, start, end);
                           if (join.out.has_value()) readySends(engine, model, join.out->effect == "echo", join.out->effect == "reverb");
                           auto* rec = record.getDynamicObject();
                           rec->setProperty("id", id);
                           rec->setProperty("outClipId", outClipId);
                           rec->setProperty("inClipId", inClipId);
                           rec->setProperty("preset", "custom");
                           rec->setProperty("startMs", start);
                           rec->setProperty("endMs", end);
                           engine.send("MIX_TRANSITION_SET", record);
                       });
    return true;
}

juce::String drop(EngineClient& engine, TimelineActions& actions, const ProjectModel& model, const juce::String& clipId, double atMs,
                  const juce::String& what)
{
    const auto parts = juce::StringArray::fromTokens(what, ":", "");
    if (parts.size() != 3 || parts[0] != "transition") return "Not a transition";
    const auto& side = parts[1];
    const auto& id = parts[2];
    // The clip's joins that this can go on, the nearest to where it was dropped first.
    std::vector<std::pair<juce::String, juce::String>> candidates;
    for (const auto& pair : pairsFor(model, clipId))
        if (side == "preset" || (side == "out" && pair.first == clipId) || (side == "in" && pair.second == clipId)) candidates.push_back(pair);
    if (candidates.empty())
    {
        if (side == "out") return "Drop an Out on a clip that a clip on another track overlaps as it ends.";
        if (side == "in") return "Drop an In on a clip that starts while a clip on another track is still playing.";
        return "Overlap this clip with a clip on another track first.";
    }
    auto distance = [&](const std::pair<juce::String, juce::String>& pair)
    {
        const auto* o = model.findClip(pair.first);
        const auto* i = model.findClip(pair.second);
        const auto w = o != nullptr && i != nullptr ? overlap(*o, *i) : std::nullopt;
        if (!w.has_value()) return 1.0e12;
        return atMs < w->first ? w->first - atMs : atMs > w->second ? atMs - w->second : 0.0;
    };
    const auto best = *std::min_element(candidates.begin(), candidates.end(),
                                        [&](const auto& x, const auto& y) { return distance(x) < distance(y); });
    if (side == "preset")
    {
        if (find(id) == nullptr) return "Unknown transition " + id;
        apply(engine, actions, model, best.first, best.second, id);
        return {};
    }
    const auto* kind = findHalf(id);
    if (kind == nullptr || kind->outgoing != (side == "out")) return "Unknown transition " + id;
    const auto* record = recordFor(model, best.first, best.second);
    auto join = record != nullptr ? joinOf(*record) : Join{};
    // A plain preset's other half carries on as its nearest equivalent.
    if (side == "out") join.out = kind->defaults;
    else join.in = kind->defaults;
    applyJoin(engine, actions, model, best.first, best.second, join);
    return {};
}

} // namespace stemcel::native::mix
