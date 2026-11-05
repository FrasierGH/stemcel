#include "BeatSync.h"

#include "SectionAnalysis.h"

#include <algorithm>
#include <cmath>

namespace stemcel::native::beatsync
{
namespace
{
constexpr int kBeatsPerBar = 4;
constexpr int kBarsPerPhrase = 8;

// Timeline ms for a point `sourceMs` into the clip's file, with the clip starting at `offsetMs`.
double toTimeline(const ProjectModel& model, const ClipModel& clip, double offsetMs, double sourceMs)
{
    if (model.clipFollows(clip))
    {
        auto moved = clip;
        moved.offsetMs = offsetMs;
        return model.followTimelineMs(moved, sourceMs - clip.inMs);
    }
    const double ratio = clip.effectiveTempoRatio > 0.0 ? clip.effectiveTempoRatio : 1.0;
    return offsetMs + (sourceMs - clip.inMs) / ratio;
}

double median(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
    return v[v.size() / 2];
}

double nearestIn(const std::vector<double>& sorted, double t)
{
    const auto it = std::lower_bound(sorted.begin(), sorted.end(), t);
    if (it == sorted.end()) return sorted.back();
    if (it == sorted.begin()) return *it;
    return (t - *std::prev(it) < *it - t) ? *std::prev(it) : *it;
}

// The beats of the project grid (4/4 from the timeline's start) between two times.
std::vector<double> gridBeats(const ProjectModel& model, double fromMs, double toMs, std::vector<size_t>* downbeats)
{
    const auto curve = model.tempoCurve();
    std::vector<double> out;
    for (double b = std::floor(curve.beatAt(juce::jmax(0.0, fromMs))) - 1.0; b >= -1.0; b += 1.0)
    {
        if (b < 0.0) continue;
        const double t = curve.msAtBeat(b);
        if (t > toMs) break;
        if (downbeats != nullptr && static_cast<long long>(b) % kBeatsPerBar == 0) downbeats->push_back(out.size());
        out.push_back(t);
    }
    return out;
}

double typicalPeriod(const std::vector<double>& beats)
{
    std::vector<double> gaps;
    for (size_t i = 1; i < beats.size(); ++i) gaps.push_back(beats[i] - beats[i - 1]);
    return gaps.empty() ? 500.0 : median(gaps);
}

// Section starts of an item, on the timeline for a clip of it at `offsetMs`.
struct Mark
{
    juce::String kind;
    double ms = 0.0;
};
std::vector<Mark> sectionMarks(const ProjectModel& model, const ClipModel& clip, double offsetMs)
{
    std::vector<Mark> out;
    const auto* item = model.findLibraryItem(clip.libraryItemId);
    if (item == nullptr) return out;
    for (const auto& s : sections::of(*item))
    {
        const double sourceMs = s.startSec * 1000.0;
        if (sourceMs < clip.inMs - 1.0 || sourceMs > clip.inMs + clip.durationMs) continue;
        out.push_back({s.kind, toTimeline(model, clip, offsetMs, sourceMs)});
    }
    return out;
}
} // namespace

std::vector<double> clipBeatsMs(const ProjectModel& model, const ClipModel& clip, double offsetMs, std::vector<size_t>* downbeats)
{
    std::vector<double> out;
    const auto* item = model.findLibraryItem(clip.libraryItemId);
    if (item == nullptr) return out;
    std::vector<double> sourceSec = item->beats;
    const double endSec = (clip.inMs + clip.durationMs) / 1000.0;
    if (sourceSec.empty() && item->bpm > 0.0)
    {
        const double period = 60.0 / item->bpm;
        for (double t = std::fmod(item->beatAnchorSec, period); t <= endSec; t += period) sourceSec.push_back(t);
    }
    else if (item->bpm > 0.0 && sourceSec.back() + 60.0 / item->bpm <= endSec)
    {
        // The beat tracker listens to the first minute only (BpmDetector::kBeatTrackingSeconds);
        // past its last beat the track's grid carries on at its tempo, to the end of the window.
        const double period = 60.0 / item->bpm;
        const double last = sourceSec.back();
        for (int k = 1; last + k * period <= endSec + 1.0e-6; ++k) sourceSec.push_back(last + k * period);
    }
    if (sourceSec.empty()) return out;
    // Bars count from the beat nearest the anchor (the first downbeat).
    size_t anchor = 0;
    for (size_t i = 1; i < sourceSec.size(); ++i)
        if (std::abs(sourceSec[i] - item->beatAnchorSec) < std::abs(sourceSec[anchor] - item->beatAnchorSec)) anchor = i;
    for (size_t i = 0; i < sourceSec.size(); ++i)
    {
        const double sourceMs = sourceSec[i] * 1000.0;
        if (sourceMs < clip.inMs || sourceMs > clip.inMs + clip.durationMs) continue;
        const auto fromAnchor = static_cast<long long>(i) - static_cast<long long>(anchor);
        if (downbeats != nullptr && ((fromAnchor % kBeatsPerBar) + kBeatsPerBar) % kBeatsPerBar == 0) downbeats->push_back(out.size());
        out.push_back(toTimeline(model, clip, offsetMs, sourceMs));
    }
    return out;
}

std::vector<double> sectionStartsMs(const ProjectModel& model, const ClipModel& clip, double offsetMs, const juce::StringArray& kinds)
{
    std::vector<double> out;
    for (const auto& m : sectionMarks(model, clip, offsetMs))
        if (kinds.contains(m.kind)) out.push_back(m.ms);
    std::sort(out.begin(), out.end());
    return out;
}

const ClipModel* partnerFor(const ProjectModel& model, const ClipModel& dragged, double startMs, const juce::String& trackId)
{
    const double endMs = startMs + dragged.effectiveDurationMs;
    const ClipModel* best = nullptr;
    double bestOverlap = 0.0;
    for (const auto& t : model.tracks)
    {
        if (t.id == trackId) continue;
        for (const auto& c : t.clips)
        {
            if (c.id == dragged.id) continue;
            const double overlap = juce::jmin(endMs, c.endMs()) - juce::jmax(startMs, c.offsetMs);
            if (overlap > bestOverlap)
            {
                bestOverlap = overlap;
                best = &c;
            }
        }
    }
    return best;
}

namespace
{
// The partner's beats, or the project grid's, around the dragged clip.
std::vector<double> targetBeats(const ProjectModel& model, const ClipModel* partner, double fromMs, double toMs, std::vector<size_t>* downbeats)
{
    if (partner != nullptr)
    {
        auto beats = clipBeatsMs(model, *partner, partner->offsetMs, downbeats);
        if (beats.size() >= 2) return beats;
        if (downbeats != nullptr) downbeats->clear();
    }
    return gridBeats(model, fromMs - 60000.0, toMs + 60000.0, downbeats);
}

double fineOffset(const std::vector<double>& mine, const std::vector<double>& theirs)
{
    if (mine.empty() || theirs.empty()) return 0.0;
    // Only my beats inside the span of theirs (the overlap) count.
    std::vector<double> diffs;
    for (const double b : mine)
        if (b >= theirs.front() - 1.0 && b <= theirs.back() + 1.0) diffs.push_back(nearestIn(theirs, b) - b);
    if (diffs.empty()) diffs.push_back(nearestIn(theirs, mine.front()) - mine.front());
    return median(diffs);
}
} // namespace

double nearestBeat(const ProjectModel& model, const ClipModel& dragged, double startMs, const juce::String& trackId)
{
    const auto* partner = partnerFor(model, dragged, startMs, trackId);
    const auto mine = clipBeatsMs(model, dragged, startMs);
    const auto theirs = targetBeats(model, partner, startMs, startMs + dragged.effectiveDurationMs, nullptr);
    return juce::jmax(0.0, startMs + fineOffset(mine, theirs));
}

std::vector<Placement> placements(const ProjectModel& model, const ClipModel& dragged, double startMs, const juce::String& trackId)
{
    std::vector<Placement> out;
    const auto* partner = partnerFor(model, dragged, startMs, trackId);
    std::vector<size_t> myDown, theirDown;
    const auto mine = clipBeatsMs(model, dragged, startMs, &myDown);
    const auto theirs = targetBeats(model, partner, startMs, startMs + dragged.effectiveDurationMs, &theirDown);
    if (mine.empty() || theirs.empty()) return out;
    const double fine = fineOffset(mine, theirs);
    const double period = typicalPeriod(theirs);
    const auto onBeat = [&](double s) { return juce::jmax(0.0, s); };
    out.push_back({"Nearest beat", onBeat(startMs + fine)});

    // Bars and phrases: my nearest downbeat (or phrase start) onto theirs, by whole beats.
    const auto unitMove = [&](int beatsPerUnit) -> std::optional<double>
    {
        std::vector<double> my, their;
        for (size_t k = 0; k < myDown.size(); ++k)
            if (beatsPerUnit == kBeatsPerBar || k % kBarsPerPhrase == 0) my.push_back(mine[myDown[k]] + fine);
        for (size_t k = 0; k < theirDown.size(); ++k)
            if (beatsPerUnit == kBeatsPerBar || k % kBarsPerPhrase == 0) their.push_back(theirs[theirDown[k]]);
        if (my.empty() || their.empty()) return std::nullopt;
        double best = 0.0;
        bool found = false;
        for (const double m : my)
        {
            const double move = std::round((nearestIn(their, m) - m) / period) * period;
            if (!found || std::abs(move) < std::abs(best))
            {
                best = move;
                found = true;
            }
            if (m > their.back()) break;
        }
        return found ? std::optional<double>(startMs + fine + best) : std::nullopt;
    };
    if (const auto bar = unitMove(kBeatsPerBar)) out.push_back({"Nearest bar", onBeat(*bar)});
    if (const auto phrase = unitMove(kBeatsPerBar * kBarsPerPhrase)) out.push_back({"Nearest phrase (8 bars)", onBeat(*phrase)});

    if (partner == nullptr) return out;
    // Arrangement placements, from both items' sections.
    const auto myMarks = sectionMarks(model, dragged, startMs);
    const auto theirMarks = sectionMarks(model, *partner, partner->offsetMs);
    const auto first = [](const std::vector<Mark>& marks, std::initializer_list<const char*> kinds, double after) -> std::optional<double>
    {
        for (const auto& m : marks)
            for (const auto* k : kinds)
                if (m.kind == k && m.ms >= after) return m.ms;
        return std::nullopt;
    };
    // A section move keeps the beats lined up: it is rounded to whole beats from the synced spot.
    const auto aligned = [&](double myMark, double theirMark)
    { return onBeat(startMs + fine + std::round((theirMark - (myMark + fine)) / period) * period); };
    const auto myDrop = first(myMarks, {"drop", "chorus"}, -1.0e12);
    if (myMarks.empty() || theirMarks.empty())
    {
        out.push_back({"Drop options: analyse both tracks' sections first", -1.0});
        return out;
    }
    if (myDrop)
    {
        if (const auto theirDrop = first(theirMarks, {"drop", "chorus"}, startMs))
            out.push_back({"First drop on its next drop", aligned(*myDrop, *theirDrop)});
        if (const auto theirOut = first(theirMarks, {"breakdown", "outro"}, startMs))
            out.push_back({"First drop where its breakdown or outro begins", aligned(*myDrop, *theirOut)});
    }
    if (const auto theirOutro = first(theirMarks, {"outro"}, -1.0e12))
    {
        const double myStart = myMarks.front().ms;
        out.push_back({"Intro over its outro", aligned(myStart, *theirOutro)});
    }
    return out;
}
} // namespace stemcel::native::beatsync
