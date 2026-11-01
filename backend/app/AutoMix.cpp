#include "AutoMix.h"

#include "MixTransitions.h"
#include "TimelineActions.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace stemcel::native::automix
{
namespace
{
constexpr double kBeatsPerBar = 4.0;
constexpr double kPhraseBeats = 32.0; // 8 bars: where DJ tracks change section

int pitchClass(const juce::String& note)
{
    static const std::map<juce::String, int> classes{{"C", 0},  {"C#", 1}, {"Db", 1},  {"D", 2},  {"D#", 3}, {"Eb", 3},
                                                     {"E", 4},  {"Fb", 4}, {"E#", 5},  {"F", 5},  {"F#", 6}, {"Gb", 6},
                                                     {"G", 7},  {"G#", 8}, {"Ab", 8},  {"A", 9},  {"A#", 10}, {"Bb", 10},
                                                     {"B", 11}, {"Cb", 11}, {"B#", 0}};
    const auto it = classes.find(note);
    return it != classes.end() ? it->second : -1;
}

double sourceBpm(const ProjectModel& model, const LibraryItemModel* item)
{
    for (; item != nullptr; item = item->sourceItemId.isNotEmpty() ? model.findLibraryItem(item->sourceItemId) : nullptr)
        if (item->bpm > 0.0) return item->bpm;
    return 0.0;
}

juce::String keyOf(const ProjectModel& model, const LibraryItemModel* item)
{
    for (; item != nullptr; item = item->sourceItemId.isNotEmpty() ? model.findLibraryItem(item->sourceItemId) : nullptr)
        if (item->key.isNotEmpty()) return item->key;
    return {};
}

// How far apart two tracks are as neighbours in a mix: keys first, then tempo, then energy.
double neighbourCost(const ProjectModel& model, WaveformStore& waveforms, const LibraryItemModel& a, const LibraryItemModel& b)
{
    const auto ka = camelot(keyOf(model, &a));
    const auto kb = camelot(keyOf(model, &b));
    const double keyCost = ka.valid() && kb.valid() ? keyDistance(ka, kb) : 1.5;
    const double ba = sourceBpm(model, &a), bb = sourceBpm(model, &b);
    const double tempoCost = ba > 0.0 && bb > 0.0 ? std::abs(ba - bb) / ba * 20.0 : 1.0; // 5% = one key step
    const auto ea = energyDb(waveforms, a), eb = energyDb(waveforms, b);
    // Rising energy is how a set builds, so a step down costs more than a step up.
    const double energyCost = ea && eb ? (*eb >= *ea ? (*eb - *ea) * 0.05 : (*ea - *eb) * 0.15) : 0.0;
    return keyCost + tempoCost + energyCost;
}
} // namespace

Camelot camelot(const juce::String& rawKey)
{
    auto key = rawKey.trim();
    if (key.isEmpty()) return {};
    // Already Camelot ("8A", "11B")?
    if (const auto last = key.getLastCharacter(); (last == 'A' || last == 'B') && key.dropLastCharacters(1).containsOnly("0123456789"))
    {
        const int n = key.dropLastCharacters(1).getIntValue();
        if (n >= 1 && n <= 12) return {n, last == 'B'};
    }
    const bool minor = key.containsIgnoreCase("minor") || key.containsIgnoreCase("min") || key.endsWith("m");
    juce::String note = key.substring(0, 1).toUpperCase();
    if (key.length() > 1 && (key[1] == '#' || key[1] == 'b')) note << juce::String::charToString(key[1]);
    int pc = pitchClass(note);
    if (pc < 0) return {};
    if (minor) pc = (pc + 3) % 12; // its relative major shares the number
    // Camelot 8B is C major and each step is a fifth (7 semitones; 7 is its own inverse mod 12).
    return {(pc * 7 % 12 + 7) % 12 + 1, !minor};
}

int keyDistance(Camelot a, Camelot b)
{
    if (!a.valid() || !b.valid()) return 2;
    const int steps = std::min((a.number - b.number + 12) % 12, (b.number - a.number + 12) % 12);
    if (a.major == b.major) return steps;
    return steps == 0 ? 1 : steps + 1; // the relative major/minor, or further
}

juce::String transposeKey(const juce::String& key, int semitones)
{
    static const char* names[] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
    const auto c = camelot(key);
    if (!c.valid()) return {};
    // Back from the wheel to the root: number n is the major root (n - 8) * 7 (mod 12).
    const int majorRoot = ((c.number - 8) * 7 % 12 + 12) % 12;
    const int root = c.major ? majorRoot : (majorRoot + 9) % 12;
    return juce::String(names[((root + semitones) % 12 + 12) % 12]) + (c.major ? " major" : " minor");
}

juce::String itemKey(const ProjectModel& model, const LibraryItemModel* item)
{
    return keyOf(model, item);
}

juce::String clipKey(const ProjectModel& model, const ClipModel& clip)
{
    const auto key = keyOf(model, model.findLibraryItem(clip.libraryItemId));
    const int shift = static_cast<int>(std::lround(clip.semitones));
    return shift == 0 ? key : transposeKey(key, shift);
}

std::optional<int> keyMatchShift(const juce::String& moving, const juce::String& target, int maxShift)
{
    const auto goal = camelot(target);
    if (!camelot(moving).valid() || !goal.valid()) return std::nullopt;
    for (int step = 0; step <= maxShift; ++step)
        for (const int shift : {step, -step})
            if (keyDistance(camelot(transposeKey(moving, shift)), goal) <= 1) return shift;
    return std::nullopt;
}

std::optional<double> energyDb(WaveformStore& waveforms, const LibraryItemModel& item)
{
    const auto peaks = waveforms.get(item.audioPath());
    if (peaks == nullptr || peaks->bucketsPerLane() == 0) return std::nullopt;
    // The summary lane's average swing: a stand-in for loudness that ranks tracks well enough.
    const int buckets = peaks->bucketsPerLane();
    double sum = 0.0;
    for (int i = 0; i < buckets; ++i)
        sum += 0.5 * std::abs(peaks->peaks[static_cast<size_t>(2 * i + 1)] - peaks->peaks[static_cast<size_t>(2 * i)]);
    const double mean = sum / buckets;
    return mean > 1.0e-6 ? 20.0 * std::log10(mean) : -120.0;
}

juce::StringArray styles()
{
    return {"Auto", "Smooth", "Club", "Party"};
}

std::optional<double> windowEnergyDb(WaveformStore& waveforms, const LibraryItemModel& item, double fromMs, double toMs)
{
    const auto peaks = waveforms.get(item.audioPath());
    if (peaks == nullptr || peaks->bucketsPerLane() == 0 || peaks->peaksPerSecond <= 0) return std::nullopt;
    const int buckets = peaks->bucketsPerLane();
    const int first = juce::jlimit(0, buckets, static_cast<int>(fromMs / 1000.0 * peaks->peaksPerSecond));
    const int last = juce::jlimit(0, buckets, static_cast<int>(toMs / 1000.0 * peaks->peaksPerSecond));
    if (last - first < 4) return std::nullopt;
    double sum = 0.0;
    for (int i = first; i < last; ++i)
        sum += 0.5 * std::abs(peaks->peaks[static_cast<size_t>(2 * i + 1)] - peaks->peaks[static_cast<size_t>(2 * i)]);
    const double mean = sum / (last - first);
    return mean > 1.0e-6 ? 20.0 * std::log10(mean) : -120.0;
}

Choice choose(const ProjectModel& model, WaveformStore& waveforms, const LibraryItemModel* out, const LibraryItemModel* in,
              const juce::String& previous, const juce::String& style, int outShift, int inShift, const ClipModel* outClip,
              const ClipModel* inClip)
{
    const auto ko = camelot(transposeKey(keyOf(model, out), outShift));
    const auto ki = camelot(transposeKey(keyOf(model, in), inShift));
    const int keyGap = ko.valid() && ki.valid() ? keyDistance(ko, ki) : -1;
    const double bo = sourceBpm(model, out), bi = sourceBpm(model, in);
    const double tempoGap = bo > 0.0 && bi > 0.0 ? std::abs(bo - bi) / bo : -1.0;
    std::optional<double> energyStep;
    bool atTheJoin = false;
    // Where the clips meet: the outgoing clip's last 8 s against the incoming one's first 8 s.
    constexpr double kEdgeMs = 8000.0;
    if (out != nullptr && in != nullptr && outClip != nullptr && inClip != nullptr)
    {
        const double outEnd = outClip->inMs + outClip->durationMs;
        if (const auto eo = windowEnergyDb(waveforms, *out, juce::jmax(outClip->inMs, outEnd - kEdgeMs), outEnd),
            ei = windowEnergyDb(waveforms, *in, inClip->inMs, inClip->inMs + juce::jmin(kEdgeMs, inClip->durationMs));
            eo && ei)
        {
            energyStep = *ei - *eo;
            atTheJoin = true;
        }
    }
    if (!energyStep.has_value() && out != nullptr && in != nullptr)
        if (const auto eo = energyDb(waveforms, *out), ei = energyDb(waveforms, *in); eo && ei) energyStep = *ei - *eo;

    const bool keysClash = keyGap >= 2;
    const bool bigTempoGap = tempoGap > 0.08;
    const int energy = energyStep.has_value() ? (*energyStep > 1.5 ? 1 : *energyStep < -1.5 ? -1 : 0) : 0;

    const mix::Preset* best = nullptr;
    double bestScore = -1.0e9;
    for (const auto& p : mix::presets())
    {
        double score = 0.0;
        // A tempo gap a stretch would make obvious: a move that hides it, or a ramp when the keys agree.
        if (bigTempoGap) score += p.tempoGapSafe ? 3.0 : (p.extra == mix::Extra::tempoRamp && !keysClash && tempoGap < 0.16 ? 4.0 : -4.0);
        else if (p.extra == mix::Extra::tempoRamp) score -= 2.0; // nothing to ramp
        // Clashing keys: keep the two tracks' harmonies from sounding together for long.
        if (keysClash) score += p.harmonicSafe ? 3.0 : -3.0;
        else if (keyGap >= 0) score += p.harmonicSafe ? 0.0 : 1.5; // in key: a real blend is the nicer move
        // Energy: lift into a bigger track, let down into a quieter one.
        score += p.energy == energy ? 1.0 : (p.energy == -energy && energy != 0 ? -1.0 : 0.0);
        // Style.
        if (style == "Smooth") score += p.category == "Blends" ? 2.0 : p.category == "Filters" ? 0.5 : -1.5;
        else if (style == "Club") score += (p.category == "Filters" || p.id == "bassSwap" || p.id == "eqBlend" || p.id == "echoOut") ? 2.0 : 0.0;
        else if (style == "Party") score += (p.category == "Cuts" || p.category == "FX") ? 2.0 : -0.5;
        // Variety, and the extras' own costs: these are statements, not every-join moves.
        if (p.id == previous) score -= 2.5;
        if (p.extra == mix::Extra::brake || p.extra == mix::Extra::backspin) score -= 1.0;
        // Unknown keys or tempos: prefer what is safe either way.
        if (keyGap < 0 || tempoGap < 0.0) score += (p.harmonicSafe && p.tempoGapSafe) ? 1.0 : 0.0;
        if (score > bestScore)
        {
            bestScore = score;
            best = &p;
        }
    }
    if (best == nullptr) return {"smoothBlend", "default"};
    juce::String why;
    why << (keyGap < 0 ? "key unknown" : keysClash ? "keys clash (" + ko.text() + " > " + ki.text() + ")"
                                                   : "in key (" + ko.text() + " > " + ki.text() + ")");
    if (tempoGap >= 0.0) why << ", tempo " << juce::String(bo, 1) << " > " << juce::String(bi, 1);
    if (energy != 0) why << (energy > 0 ? ", energy up" : ", energy down") << (atTheJoin ? " at the join" : "");
    return {best->id, why};
}

std::vector<juce::String> harmonicOrder(const ProjectModel& model, WaveformStore& waveforms, std::vector<juce::String> itemIds)
{
    if (itemIds.size() < 3) return itemIds;
    std::vector<juce::String> ordered{itemIds.front()};
    std::vector<juce::String> left(itemIds.begin() + 1, itemIds.end());
    while (!left.empty())
    {
        const auto* last = model.findLibraryItem(ordered.back());
        size_t bestIndex = 0;
        double bestCost = 1.0e9;
        for (size_t i = 0; i < left.size(); ++i)
            if (const auto* candidate = model.findLibraryItem(left[i]); last != nullptr && candidate != nullptr)
                if (const double cost = neighbourCost(model, waveforms, *last, *candidate); cost < bestCost)
                {
                    bestCost = cost;
                    bestIndex = i;
                }
        ordered.push_back(left[bestIndex]);
        left.erase(left.begin() + static_cast<std::ptrdiff_t>(bestIndex));
    }
    return ordered;
}

juce::String build(EngineClient& engine, ProjectMirror& mirror, TimelineActions& actions, WaveformStore& waveforms,
                   std::vector<juce::String> itemIds, bool reorder, const juce::String& style, bool matchKeys)
{
    itemIds.erase(std::remove_if(itemIds.begin(), itemIds.end(),
                                 [&](const juce::String& id) { return mirror.model().findLibraryItem(id) == nullptr; }),
                  itemIds.end());
    if (itemIds.size() < 2) return "Select at least two tracks to auto mix.";
    if (reorder) itemIds = harmonicOrder(mirror.model(), waveforms, itemIds);

    int joins = 0, shifted = 0;
    juce::StringArray used;
    engine.inUndoGroup("Auto Mix", [&]
    {
        // After everything already on the timeline, from the next bar.
        const auto& start = mirror.model();
        const auto curve = start.tempoCurve();
        double at = start.contentEndMs() > 0.0 ? curve.msAtBeat(std::ceil(curve.beatAt(start.contentEndMs()) / kBeatsPerBar) * kBeatsPerBar) : 0.0;
        const juce::String decks[2] = {actions.addTrack(), actions.addTrack()};
        mirror.refreshNow();
        actions.renameTrack(decks[0], "Mix A");
        actions.renameTrack(decks[1], "Mix B");

        juce::String previousClip, previousPreset;
        for (size_t i = 0; i < itemIds.size(); ++i)
        {
            const auto* item = mirror.model().findLibraryItem(itemIds[i]);
            if (item == nullptr) continue;
            const auto deck = decks[i % 2];
            const LibraryItemModel* before = nullptr;
            juce::String presetId;
            int keyShift = 0;
            double windowStart = at, windowEnd = at;
            if (previousClip.isNotEmpty())
                if (const auto* prev = mirror.model().findClip(previousClip))
                {
                    before = mirror.model().findLibraryItem(prev->libraryItemId);
                    // Into key with the track before, when a small shift does it (larger ones colour
                    // the sound); worked out first, so the preset is chosen for the keys heard.
                    const int outShift = static_cast<int>(std::lround(prev->semitones));
                    if (matchKeys)
                        keyShift = keyMatchShift(keyOf(mirror.model(), item), clipKey(mirror.model(), *prev), 2).value_or(0);
                    const auto choice = choose(mirror.model(), waveforms, before, item, previousPreset, style, outShift, keyShift);
                    presetId = choice.presetId;
                    const auto* preset = mix::find(presetId);
                    const double lengthBeats = (preset != nullptr ? preset->bars : 8.0) * kBeatsPerBar;
                    // The join starts on a phrase of the outgoing track, as late as its length allows.
                    const auto c = mirror.model().tempoCurve();
                    const double prevStart = c.beatAt(prev->offsetMs);
                    const double prevEnd = c.beatAt(prev->endMs());
                    const double phrases = std::floor((prevEnd - lengthBeats - prevStart) / kPhraseBeats);
                    const double startBeat = phrases >= 1.0 ? prevStart + phrases * kPhraseBeats
                                                            : juce::jmax(prevStart, std::floor((prevEnd - lengthBeats) / kBeatsPerBar) * kBeatsPerBar);
                    windowStart = c.msAtBeat(startBeat);
                    windowEnd = c.msAtBeat(startBeat + lengthBeats);
                    // The outgoing track mixes out at the end of the join.
                    if (windowEnd < prev->endMs()) actions.trimClip(prev->id, prev->offsetMs, windowEnd);
                }
            const auto clipId = actions.addClip(deck, *item, windowStart);
            mirror.refreshNow();
            if (keyShift != 0)
            {
                actions.setClipKeyShift(clipId, keyShift);
                mirror.refreshNow();
                ++shifted;
            }
            if (previousClip.isNotEmpty() && presetId.isNotEmpty())
            {
                if (mix::apply(engine, actions, mirror.model(), previousClip, clipId, presetId, std::make_pair(windowStart, windowEnd)))
                {
                    ++joins;
                    if (const auto* p = mix::find(presetId)) used.addIfNotAlreadyThere(p->name);
                }
                mirror.refreshNow();
            }
            previousClip = clipId;
            previousPreset = presetId;
            if (const auto* placed = mirror.model().findClip(clipId)) at = placed->endMs();
        }
    });
    return "Auto mixed " + juce::String(static_cast<int>(itemIds.size())) + " tracks with " + juce::String(joins)
           + " transitions (" + used.joinIntoString(", ") + ")"
           + (shifted > 0 ? ", " + juce::String(shifted) + " shifted into key" : juce::String());
}

int autoTransition(EngineClient& engine, ProjectMirror& mirror, TimelineActions& actions, WaveformStore& waveforms,
                   const std::vector<juce::String>& clipIds, const juce::String& style)
{
    // Every overlapping (outgoing, incoming) pair among the clips, in timeline order.
    std::vector<std::pair<juce::String, juce::String>> pairs;
    const auto& model = mirror.model();
    for (const auto& track : model.tracks)
        for (const auto& clip : track.clips)
        {
            if (!clipIds.empty() && std::find(clipIds.begin(), clipIds.end(), clip.id) == clipIds.end()) continue;
            for (const auto& [outId, inId] : mix::pairsFor(model, clip.id))
                if (outId == clip.id
                    && (clipIds.empty() || std::find(clipIds.begin(), clipIds.end(), inId) != clipIds.end()))
                    pairs.emplace_back(outId, inId);
        }
    std::sort(pairs.begin(), pairs.end(), [&model](const auto& a, const auto& b)
              { return model.findClip(a.second)->offsetMs < model.findClip(b.second)->offsetMs; });
    int count = 0;
    engine.inUndoGroup("Auto Transition", [&]
    {
        juce::String previous;
        for (const auto& [outId, inId] : pairs)
        {
            const auto& m = mirror.model();
            const auto* out = m.findClip(outId);
            const auto* in = m.findClip(inId);
            if (out == nullptr || in == nullptr) continue;
            const auto choice = choose(m, waveforms, m.findLibraryItem(out->libraryItemId), m.findLibraryItem(in->libraryItemId),
                                       previous, style, static_cast<int>(std::lround(out->semitones)),
                                       static_cast<int>(std::lround(in->semitones)));
            if (mix::apply(engine, actions, m, outId, inId, choice.presetId))
            {
                ++count;
                previous = choice.presetId;
            }
            mirror.refreshNow();
        }
    });
    return count;
}

} // namespace stemcel::native::automix
