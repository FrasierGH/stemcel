#include "BeatStutter.h"

#include "Automation.h"

#include <cmath>

namespace stemcel::native::stutter
{
namespace
{
struct Region
{
    double fromBeat, beats;
    const char* division;
};

// A preset's regions over `length` beats from `start` (its own length when not given).
std::vector<Region> layout(const juce::String& id, double start, double length)
{
    auto build = [start](double total)
    {
        // Half the span at 1/4, a quarter at 1/8, then an eighth each at 1/16 and 1/32.
        return std::vector<Region>{{start, total * 0.5, "1/4"},
                                   {start + total * 0.5, total * 0.25, "1/8"},
                                   {start + total * 0.75, total * 0.125, "1/16"},
                                   {start + total * 0.875, total * 0.125, "1/32"}};
    };
    if (id == "stutter8") return {{start, length > 0.0 ? length : 1.0, "1/8"}};
    if (id == "stutter16") return {{start, length > 0.0 ? length : 1.0, "1/16"}};
    if (id == "stutter32") return {{start, length > 0.0 ? length : 0.5, "1/32"}};
    if (id == "build") return build(length > 0.0 ? length : 4.0);
    if (id == "longBuild") return build(length > 0.0 ? length : 8.0);
    if (id == "fill")
    {
        // The last beat of the bar the start is in (or of the range).
        const double last = length > 0.0 ? start + length - 1.0 : std::floor(start / 4.0) * 4.0 + 3.0;
        return {{last, 0.5, "1/16"}, {last + 0.5, 0.5, "1/32"}};
    }
    if (id == "gated") return {{start, length > 0.0 ? length : 4.0, "1/16"}};
    return {};
}
} // namespace

const std::vector<Preset>& presets()
{
    static const std::vector<Preset> list{
        {"stutter8", "Stutter 1/8"},
        {"stutter16", "Stutter 1/16"},
        {"stutter32", "Stutter 1/32"},
        {"build", "Build-up (1/4 to 1/32, 1 bar)"},
        {"longBuild", "Long build-up (2 bars)"},
        {"fill", "Fill on the last beat"},
        {"gated", "Gated stutter (1/16 gate, 1 bar)"},
    };
    return list;
}

void apply(TimelineActions& actions, const ProjectModel& model, const juce::String& trackId, const juce::String& presetId,
           double startBeat, double endBeat)
{
    const auto* track = model.findTrack(trackId);
    if (track == nullptr) return;
    const double length = endBeat > startBeat ? endBeat - startBeat : -1.0;
    const auto regions = layout(presetId, juce::jmax(0.0, startBeat), length);
    if (regions.empty()) return;
    const double from = regions.front().fromBeat;
    const double to = regions.back().fromBeat + regions.back().beats;
    const auto* preset = [&presetId]() -> const Preset*
    {
        for (const auto& p : presets())
            if (presetId == p.id) return &p;
        return nullptr;
    }();
    actions.engineClient().inUndoGroup(juce::String("Beat Stutter: ") + (preset != nullptr ? preset->label : "stutter"),
                                       [&]
                                       {
                                           // Repeats already in the way go first: regions cannot overlap.
                                           if (const auto* list = track->raw.getProperty("beatRepeats", {}).getArray())
                                               for (const auto& r : *list)
                                               {
                                                   const double s = static_cast<double>(r.getProperty("startBeat", 0.0));
                                                   const double l = static_cast<double>(r.getProperty("lengthBeats", 0.0));
                                                   if (s < to && s + l > from) actions.deleteBeatRepeat(trackId, r.getProperty("id", {}).toString());
                                               }
                                           for (const auto& r : regions) actions.addBeatRepeat(trackId, r.fromBeat, r.beats, r.division);
                                           if (presetId == "gated")
                                               if (const auto* level = automation::find("level"))
                                               {
                                                   const auto curve = model.tempoCurve();
                                                   const double startMs = curve.msAtBeat(from);
                                                   const double endMs = curve.msAtBeat(to);
                                                   const double beatMs = 60000.0 / juce::jmax(1.0, curve.bpmAt(startMs));
                                                   actions.setAutomation(trackId, "level",
                                                                         automation::applyPattern(automation::pointsOf(*track, "level"), *level,
                                                                                                  "gate16", startMs, endMs, beatMs));
                                               }
                                       });
}

} // namespace stemcel::native::stutter
