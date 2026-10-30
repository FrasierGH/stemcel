#include "Automation.h"

#include "AutomationShape.h"

#include <algorithm>
#include <map>
#include <memory>
#include <string>

namespace stemcel::native::automation
{

const std::vector<Param>& params()
{
    static const std::vector<Param> list{
        {"filter", "Filter", -1.0, 1.0, 0.0},
        {"pan", "Pan", -1.0, 1.0, 0.0},
        // Down to -40 dB, a DJ-mixer kill (bass swaps and EQ blends); the dials stop at -15.
        {"toneBass", "Bass", -40.0, 15.0, 0.0},
        {"toneMid", "Mid", -40.0, 15.0, 0.0},
        {"toneTreble", "Treble", -40.0, 15.0, 0.0},
        {"reverbSend", "Reverb Send", 0.0, 1.0, 0.0},
        {"delaySend", "Delay Send", 0.0, 1.0, 0.0},
        {"leveler", "Compressor", 0.0, 1.0, 0.0},
        {"punch", "Punch", 0.0, 1.0, 0.0},
        {"saturationDrive", "Saturation Drive", 0.0, 1.0, 0.0},
        {"saturationMix", "Saturation Mix", 0.0, 1.0, 1.0},
        {"bitCrusherRate", "Crusher Rate", 0.01, 1.0, 1.0},
        {"bitCrusherBits", "Crusher Bits", 1.0, 16.0, 16.0},
        {"bitCrusherBoost", "Crusher Boost", 0.0, 1.0, 0.0},
        {"bitCrusherMix", "Crusher Mix", 0.0, 1.0, 0.0},
        {"level", "Gain", -60.0, 6.0, 0.0},
    };
    return list;
}

const std::vector<Param>& masterParams()
{
    static const std::vector<Param> list{
        {"level", "Master Gain", -60.0, 6.0, 0.0},
        {"reverbMix", "Reverb Mix", 0.0, 1.0, 0.0},
        {"reverbSize", "Reverb Size", 0.0, 1.0, 0.5},
        {"reverbDecay", "Reverb Decay", 0.0, 1.0, 0.5},
        {"delayMix", "Echo Mix", 0.0, 1.0, 0.0},
        {"delayFeedback", "Echo Feedback", 0.0, 1.0, 0.3},
    };
    return list;
}

juce::String pluginParamId(const juce::String& slotId, int index)
{
    return "plugin:" + slotId + ":" + juce::String(index);
}

bool parsePluginParamId(const juce::String& paramId, juce::String& slotId, int& index)
{
    if (!paramId.startsWith("plugin:")) return false;
    const auto rest = paramId.substring(7);
    const int colon = rest.lastIndexOfChar(':');
    if (colon <= 0 || rest.substring(colon + 1).isEmpty() || !rest.substring(colon + 1).containsOnly("0123456789")) return false;
    slotId = rest.substring(0, colon);
    index = rest.substring(colon + 1).getIntValue();
    return true;
}

namespace
{
// Plugin parameters' Params, made on first use; the strings they point at live here.
struct PluginParam
{
    std::string id, label;
    Param param;
};
std::map<juce::String, std::unique_ptr<PluginParam>>& pluginParams()
{
    static std::map<juce::String, std::unique_ptr<PluginParam>> map;
    return map;
}

PluginParam& pluginParam(const juce::String& paramId)
{
    auto& entry = pluginParams()[paramId];
    if (entry == nullptr)
    {
        entry = std::make_unique<PluginParam>();
        entry->id = paramId.toStdString();
        juce::String slot;
        int index = 0;
        parsePluginParamId(paramId, slot, index);
        entry->label = ("Plugin parameter " + juce::String(index + 1)).toStdString();
        entry->param = {entry->id.c_str(), entry->label.c_str(), 0.0, 1.0, 0.5};
    }
    return *entry;
}
} // namespace

void namePluginParam(const juce::String& paramId, const juce::String& label)
{
    auto& entry = pluginParam(paramId);
    if (entry.label == label.toStdString()) return;
    entry.label = label.toStdString();
    entry.param.label = entry.label.c_str();
}

const Param* find(const juce::String& paramId)
{
    for (const auto& p : params())
        if (paramId == p.id) return &p;
    for (const auto& p : masterParams())
        if (paramId == p.id) return &p;
    juce::String slot;
    int index = 0;
    if (parsePluginParamId(paramId, slot, index)) return &pluginParam(paramId).param;
    return nullptr;
}

juce::String format(const juce::String& id, double v)
{
    auto pct = [](double x) { return juce::String(juce::roundToInt(x * 100.0)) + "%"; };
    if (id == "filter") return std::abs(v) < 0.02 ? juce::String("Off") : (v < 0 ? "LPF " : "HPF ") + pct(std::abs(v));
    if (id == "pan") return std::abs(v) < 0.02 ? juce::String("C") : (v < 0 ? "L" : "R") + juce::String(juce::roundToInt(std::abs(v) * 100.0));
    if (id == "toneBass" || id == "toneMid" || id == "toneTreble" || id == "level")
        return (v >= 0 ? "+" : "") + juce::String(v, 1) + " dB";
    if (id == "bitCrusherBits") return juce::String(juce::roundToInt(v)) + "-bit";
    if (id == "bitCrusherBoost") return "+" + juce::String(juce::roundToInt(v * 12.0)) + " dB";
    return pct(v);
}

double toFraction(const Param& p, double value)
{
    return p.max == p.min ? 0.0 : juce::jlimit(0.0, 1.0, (value - p.min) / (p.max - p.min));
}

double fromFraction(const Param& p, double fraction)
{
    return p.min + juce::jlimit(0.0, 1.0, fraction) * (p.max - p.min);
}

std::vector<Lane> lanesOf(const TrackModel& track)
{
    std::vector<Lane> out;
    if (const auto* list = track.raw.getProperty("automationLaneView", {}).getArray())
        for (const auto& l : *list)
        {
            const auto id = l.getProperty("paramId", {}).toString();
            if (find(id) == nullptr) continue;
            out.push_back({id, juce::jlimit(kMinLaneHeight, kMaxLaneHeight,
                                            static_cast<int>(l.getProperty("heightPx", kDefaultLaneHeight)))});
        }
    return out;
}

std::vector<Point> pointsOf(const TrackModel& track, const juce::String& paramId)
{
    std::vector<Point> out;
    // `automation` is a list of {paramId, points}.
    if (const auto* curves = track.raw.getProperty("automation", {}).getArray())
        for (const auto& curve : *curves)
            if (curve.getProperty("paramId", {}).toString() == paramId)
                if (const auto* list = curve.getProperty("points", {}).getArray())
                    for (const auto& p : *list)
                        out.push_back({static_cast<double>(p.getProperty("timeMs", 0.0)),
                                       static_cast<double>(p.getProperty("value", 0.0)),
                                       p.getProperty("curve", "linear").toString(),
                                       static_cast<double>(p.getProperty("tension", 0.0))});
    std::sort(out.begin(), out.end(), [](const Point& a, const Point& b) { return a.timeMs < b.timeMs; });
    return out;
}

double valueAt(const std::vector<Point>& points, double timeMs, double fallback)
{
    if (points.empty()) return fallback;
    if (timeMs <= points.front().timeMs) return points.front().value;
    if (timeMs >= points.back().timeMs) return points.back().value;
    for (size_t i = 1; i < points.size(); ++i)
        if (timeMs <= points[i].timeMs)
        {
            const auto& a = points[i - 1];
            const auto& b = points[i];
            const double span = b.timeMs - a.timeMs;
            if (span <= 0.0) return b.value;
            const auto shape = automationShapeFromString(a.curve.toRawUTF8());
            return a.value + (b.value - a.value) * shapeFraction(shape, a.tension, (timeMs - a.timeMs) / span);
        }
    return points.back().value;
}

juce::var toVar(const std::vector<Point>& points)
{
    juce::Array<juce::var> list;
    for (const auto& p : points)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("timeMs", juce::jmax(0.0, p.timeMs));
        o->setProperty("value", p.value);
        if (p.curve.isNotEmpty() && p.curve != "linear") o->setProperty("curve", p.curve);
        if (std::abs(p.tension) > 1.0e-4) o->setProperty("tension", p.tension);
        list.add(juce::var(o));
    }
    return list;
}

const std::vector<std::pair<const char*, const char*>>& curveShapes()
{
    static const std::vector<std::pair<const char*, const char*>> list{
        {"linear", "Linear"},       {"smooth", "Smooth (S-curve)"}, {"fastStart", "Fast start"},
        {"slowStart", "Slow start"}, {"hold", "Hold (step)"},       {"stairs", "Stairs"}};
    return list;
}

const std::vector<Pattern>& patterns()
{
    static const std::vector<Pattern> list{
        {"rampUp", "Ramp up"},
        {"rampDown", "Ramp down"},
        {"fadeIn", "Smooth fade in"},
        {"fadeOut", "Smooth fade out"},
        {"swell", "Swell (slow build)"},
        {"dropOff", "Drop off (fast fall)"},
        {"sine1", "Sine, 1 beat"},
        {"sine2", "Sine, 2 beats"},
        {"sine4", "Sine, 1 bar"},
        {"triangle1", "Triangle, 1 beat"},
        {"triangle4", "Triangle, 1 bar"},
        {"saw1", "Saw, 1 beat"},
        {"gate8", "Trance gate, 1/8"},
        {"gate16", "Trance gate, 1/16"},
        {"pump", "Sidechain pump"},
        {"stairs", "Stairs up"},
        {"random8", "Random steps, 1/8"},
    };
    return list;
}

namespace
{
// The span a pattern moves over for a parameter: the musically useful part of its range.
std::pair<double, double> patternRange(const Param& p)
{
    const juce::String id{p.id};
    if (id == "level") return {-40.0, 0.0};
    if (id.startsWith("tone")) return {-40.0, 0.0};
    if (id == "filter") return {-1.0, 0.0};
    return {p.min, p.max};
}

struct Step
{
    double at;       // 0..1 of the period
    double fraction; // 0 (bottom of the range) .. 1 (top)
    const char* curve;
};
} // namespace

std::vector<Point> applyPattern(const std::vector<Point>& existing, const Param& param, const juce::String& patternId,
                                double startMs, double endMs, double beatMs)
{
    if (!(endMs > startMs) || !(beatMs > 0.0)) return existing;
    const auto range = patternRange(param);
    const double lo = range.first, hi = range.second;
    std::vector<Point> shape;
    auto at = [&shape, lo, hi](double t, double fraction, const char* curve) { shape.push_back({t, lo + (hi - lo) * fraction, curve, 0.0}); };
    // Repeats one period's steps across the stretch.
    auto repeat = [&at, startMs, endMs, beatMs](double periodBeats, const std::vector<Step>& cycle)
    {
        const double period = periodBeats * beatMs;
        for (double t = startMs; t < endMs - 0.5; t += period)
            for (const auto& s : cycle)
                if (t + s.at * period < endMs - 0.5) at(t + s.at * period, s.fraction, s.curve);
    };
    const auto& id = patternId;
    if (id == "rampUp") { at(startMs, 0.0, "linear"); at(endMs, 1.0, "linear"); }
    else if (id == "rampDown") { at(startMs, 1.0, "linear"); at(endMs, 0.0, "linear"); }
    else if (id == "fadeIn") { at(startMs, 0.0, "smooth"); at(endMs, 1.0, "linear"); }
    else if (id == "fadeOut") { at(startMs, 1.0, "smooth"); at(endMs, 0.0, "linear"); }
    else if (id == "swell") { at(startMs, 0.0, "slowStart"); at(endMs, 1.0, "linear"); }
    else if (id == "dropOff") { at(startMs, 1.0, "fastStart"); at(endMs, 0.0, "linear"); }
    else if (id.startsWith("sine"))
        repeat(id == "sine1" ? 1.0 : id == "sine2" ? 2.0 : 4.0, {{0.0, 0.0, "smooth"}, {0.5, 1.0, "smooth"}});
    else if (id.startsWith("triangle"))
        repeat(id == "triangle1" ? 1.0 : 4.0, {{0.0, 0.0, "linear"}, {0.5, 1.0, "linear"}});
    else if (id == "saw1") repeat(1.0, {{0.0, 0.0, "linear"}, {0.98, 1.0, "hold"}});
    else if (id == "gate8") repeat(0.5, {{0.0, 1.0, "hold"}, {0.5, 0.0, "hold"}});
    else if (id == "gate16") repeat(0.25, {{0.0, 1.0, "hold"}, {0.5, 0.0, "hold"}});
    else if (id == "pump") repeat(1.0, {{0.0, 0.0, "fastStart"}, {0.6, 1.0, "hold"}});
    else if (id == "stairs") { at(startMs, 0.0, "stairs"); at(endMs, 1.0, "linear"); }
    else if (id == "random8")
    {
        juce::Random random(static_cast<juce::int64>(startMs));
        for (double t = startMs; t < endMs - 0.5; t += beatMs / 2.0) at(t, random.nextDouble(), "hold");
    }
    else
        return existing;
    if (shape.empty()) return existing;
    if (shape.back().timeMs < endMs - 0.5) shape.push_back({endMs, shape.back().value, "linear", 0.0});

    // Hand over from the curve before and back to the curve after, as a written shape does.
    std::vector<Point> next;
    const double fallback = param.defaultValue;
    bool before = false, after = false;
    for (const auto& p : existing)
    {
        if (p.timeMs < startMs - 1.0)
        {
            next.push_back(p);
            before = true;
        }
        else if (p.timeMs > endMs + 1.0)
            after = true;
    }
    if (before) next.push_back({startMs - 1.0, valueAt(existing, startMs - 1.0, fallback), "linear", 0.0});
    next.insert(next.end(), shape.begin(), shape.end());
    if (after)
    {
        next.push_back({endMs + 1.0, valueAt(existing, endMs + 1.0, fallback), "linear", 0.0});
        for (const auto& p : existing)
            if (p.timeMs > endMs + 1.0) next.push_back(p);
    }
    return next;
}

juce::var toVar(const std::vector<Lane>& lanes)
{
    juce::Array<juce::var> list;
    for (const auto& l : lanes)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("paramId", l.paramId);
        o->setProperty("heightPx", l.heightPx);
        list.add(juce::var(o));
    }
    return list;
}

} // namespace stemcel::native::automation
