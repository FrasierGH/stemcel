#include "Mashup.h"

#include "AutoMix.h"
#include "Stems.h"
#include "TimelineActions.h"

#include <cmath>

namespace stemcel::native
{
namespace
{
// The instrumental, part by part: these three always (any 4-stem split has them), and
// BS-Roformer SW's guitar and piano too when the library has them.
const juce::StringArray kBeatStems{"Drums", "Bass", "Other"};
const juce::StringArray kExtraBeatStems{"Guitar", "Piano"};
constexpr double kBeatsPerBar = 4.0;
} // namespace

MashupBuilder::MashupBuilder(EngineClient& e, ProjectMirror& m, TimelineActions& a, StemService& s)
    : engine(e), mirror(m), actions(a), stems(s)
{
}

juce::String MashupBuilder::findStem(const ProjectModel& model, const juce::String& itemId, const juce::String& label)
{
    for (const auto& item : model.library)
        if (item.kind == "stem" && item.sourceItemId == itemId && item.name.startsWithIgnoreCase(label)) return item.id;
    return {};
}

void MashupBuilder::start(const juce::String& vocalItemId, const juce::String& beatItemId)
{
    if (busy())
    {
        if (onNotify) onNotify("A mashup is already being prepared", true);
        return;
    }
    vocalItem = vocalItemId;
    beatItem = beatItemId;
    vocalRequested = beatRequested = false;
    timerCallback();
    if (busy()) startTimer(500);
}

void MashupBuilder::timerCallback()
{
    const auto& m = mirror.model();
    if (m.findLibraryItem(vocalItem) == nullptr || m.findLibraryItem(beatItem) == nullptr)
    {
        finish("The mashup's tracks are no longer in the library", true);
        return;
    }
    if (stems.busy()) return; // a separation is under way (ours or the user's)
    const bool haveVocals = findStem(m, vocalItem, "Vocals").isNotEmpty();
    bool haveBeat = true;
    for (const auto& s : kBeatStems) haveBeat = haveBeat && findStem(m, beatItem, s).isNotEmpty();
    if (!haveVocals)
    {
        if (vocalRequested)
        {
            finish("Could not separate the vocals for the mashup", true);
            return;
        }
        vocalRequested = true;
        if (onNotify) onNotify("Mashup: separating the vocals...", false);
        stems.startUvr(vocalItem, {}, "vocalsTop", {}, {"vocals"});
        return;
    }
    if (!haveBeat)
    {
        if (beatRequested)
        {
            finish("Could not separate the instrumental for the mashup", true);
            return;
        }
        beatRequested = true;
        if (onNotify) onNotify("Mashup: separating the instrumental...", false);
        stems.startUvr(beatItem, {}, "six", {}, {"drums", "bass", "guitar", "piano", "other"});
        return;
    }
    place();
}

void MashupBuilder::place()
{
    const auto vocal = vocalItem, beat = beatItem;
    stopTimer();
    vocalItem.clear();
    beatItem.clear();
    juce::String firstTrack, vocalClip;
    double from = 0.0, to = 0.0;
    engine.inUndoGroup("Mashup", [&]
    {
        const auto& start = mirror.model();
        const auto curve = start.tempoCurve();
        from = start.contentEndMs() > 0.0 ? curve.msAtBeat(std::ceil(curve.beatAt(start.contentEndMs()) / kBeatsPerBar) * kBeatsPerBar) : 0.0;
        std::vector<std::pair<juce::String, juce::String>> parts{{"Vocals", findStem(start, vocal, "Vocals")}};
        for (const auto& s : kBeatStems) parts.emplace_back(s, findStem(start, beat, s));
        for (const auto& s : kExtraBeatStems)
            if (const auto id = findStem(start, beat, s); id.isNotEmpty()) parts.emplace_back(s, id);
        std::vector<juce::String> clips;
        for (const auto& [label, stemId] : parts)
        {
            const auto trackId = actions.addTrack();
            mirror.refreshNow();
            actions.renameTrack(trackId, "Mashup " + label);
            if (firstTrack.isEmpty()) firstTrack = trackId;
            if (const auto* item = mirror.model().findLibraryItem(stemId))
            {
                clips.push_back(actions.addClip(trackId, *item, from));
                if (label == "Vocals") vocalClip = clips.back();
            }
            mirror.refreshNow();
        }
        // On the grid, so the vocal phrases sit on the instrumental's bars.
        for (const auto& c : clips) actions.alignClipToBarGrid(c);
        mirror.refreshNow();
        // The vocals into the instrumental's key, when a small shift does it.
        if (const auto* v = mirror.model().findClip(vocalClip); v != nullptr && mirror.model().findClip(clips.back()) != nullptr)
        {
            const auto beatKey = automix::clipKey(mirror.model(), *mirror.model().findClip(clips.back()));
            if (const auto shift = automix::keyMatchShift(automix::clipKey(mirror.model(), *v), beatKey, 3); shift && *shift != 0)
                actions.setClipKeyShift(vocalClip, *shift);
        }
        mirror.refreshNow();
        for (const auto& c : clips)
            if (const auto* clip = mirror.model().findClip(c)) to = juce::jmax(to, clip->endMs());
    });
    const auto* v = mirror.model().findLibraryItem(vocal);
    const auto* b = mirror.model().findLibraryItem(beat);
    finish("Mashup: " + (v != nullptr ? v->displayName() : juce::String("vocals")) + " over "
               + (b != nullptr ? b->displayName() : juce::String("the instrumental")),
           false);
    if (onPlaced && to > from) onPlaced(from, to, firstTrack);
}

void MashupBuilder::finish(const juce::String& message, bool isError)
{
    stopTimer();
    vocalItem.clear();
    beatItem.clear();
    if (onNotify) onNotify(message, isError);
}

} // namespace stemcel::native
