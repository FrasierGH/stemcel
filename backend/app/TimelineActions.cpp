#include "TimelineActions.h"
#include "WarpModeDefaults.h"

#include "Log.h"

#include <algorithm>
#include <map>
#include <set>
#include <cmath>

namespace stemcel::native
{
namespace
{
constexpr double kBeatsPerBar = 4.0;
constexpr double kClipFitEpsilonMs = 0.5;
constexpr double kGainFloor = 1e-5;
constexpr double kMaxEnvelopeGain = 4.0;

juce::var obj(std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();
    for (const auto& [k, v] : props)
        if (!v.isVoid()) o->setProperty(k, v);
    return juce::var(o);
}

juce::var pointsVar(const std::vector<EnvelopePoint>& points)
{
    juce::Array<juce::var> arr;
    for (const auto& p : points) arr.add(obj({{"timeMs", p.timeMs}, {"gain", p.gain}}));
    return juce::var(arr);
}

double effectiveRatio(const ClipModel& clip)
{
    return clip.effectiveWarpActive && clip.effectiveTempoRatio > 0.0 ? clip.effectiveTempoRatio : 1.0;
}

double beatsPerSnapStep(const juce::String& grid)
{
    if (grid == "bar") return kBeatsPerBar;
    if (grid == "beat") return 1.0;
    if (grid == "half") return 0.5;
    if (grid == "free") return 0.0;
    return 0.25; // quarter (default)
}
} // namespace

// ── envelope ────────────────────────────────────────────────────────────────
namespace envelope
{
std::vector<EnvelopePoint> sanitize(std::vector<EnvelopePoint> points)
{
    std::vector<EnvelopePoint> cleaned;
    for (auto p : points)
    {
        if (!std::isfinite(p.timeMs) || !std::isfinite(p.gain)) continue;
        p.timeMs = juce::jmax(0.0, p.timeMs);
        p.gain = juce::jlimit(0.0, kMaxEnvelopeGain, p.gain);
        cleaned.push_back(p);
    }
    std::sort(cleaned.begin(), cleaned.end(), [](const auto& a, const auto& b) { return a.timeMs < b.timeMs; });
    std::vector<EnvelopePoint> out;
    for (const auto& p : cleaned)
    {
        if (!out.empty() && std::abs(p.timeMs - out.back().timeMs) < 1e-3) continue;
        out.push_back(p);
    }
    return out;
}

double gainAt(const std::vector<EnvelopePoint>& points, double ms)
{
    if (points.empty()) return 1.0;
    if (points.size() == 1 || ms <= points.front().timeMs) return points.front().gain;
    if (ms >= points.back().timeMs) return points.back().gain;
    size_t seg = 0;
    while (seg < points.size() - 2 && ms >= points[seg + 1].timeMs) ++seg;
    const auto& a = points[seg];
    const auto& b = points[seg + 1];
    if (ms <= a.timeMs) return a.gain;
    if (ms >= b.timeMs) return b.gain;
    // Interpolated in dB, as the engine and the renderer do.
    const double aDb = 20.0 * std::log10(juce::jmax(a.gain, kGainFloor));
    const double bDb = 20.0 * std::log10(juce::jmax(b.gain, kGainFloor));
    const double frac = (ms - a.timeMs) / (b.timeMs - a.timeMs);
    return std::pow(10.0, (aDb + (bDb - aDb) * frac) / 20.0);
}

bool isFlatUnity(const std::vector<EnvelopePoint>& points)
{
    return std::all_of(points.begin(), points.end(), [](const auto& p) { return std::abs(p.gain - 1.0) < 1e-4; });
}

std::pair<std::vector<EnvelopePoint>, std::vector<EnvelopePoint>> splitAt(const std::vector<EnvelopePoint>& points,
                                                                          double atMs)
{
    const auto source = sanitize(points);
    if (source.size() < 2 || !std::isfinite(atMs) || atMs <= 0.0) return {};
    const double seam = gainAt(source, atMs);
    std::vector<EnvelopePoint> left;
    std::vector<EnvelopePoint> right{{0.0, seam}};
    for (const auto& p : source)
        if (p.timeMs < atMs - 1e-3) left.push_back(p);
    left.push_back({atMs, seam});
    for (const auto& p : source)
        if (p.timeMs > atMs + 1e-3) right.push_back({p.timeMs - atMs, p.gain});
    left = sanitize(left);
    right = sanitize(right);
    return {isFlatUnity(left) ? std::vector<EnvelopePoint>{} : left,
            isFlatUnity(right) ? std::vector<EnvelopePoint>{} : right};
}
} // namespace envelope

// ── actions ─────────────────────────────────────────────────────────────────
TimelineActions::TimelineActions(EngineClient& engineClient, ProjectMirror& projectMirror)
    : engine(engineClient), mirror(projectMirror)
{
}

void TimelineActions::notify(const juce::String& message, bool isError)
{
    if (onNotify) onNotify(message, isError);
}

juce::String TimelineActions::snapGrid() const
{
    const auto grid = model().snapGrid;
    return grid.isNotEmpty() ? grid : juce::String("quarter");
}

double TimelineActions::snapStepMs() const
{
    const double beats = beatsPerSnapStep(snapGrid());
    const double bpm = model().bpm > 0.0 ? model().bpm : 120.0;
    return beats * 60000.0 / bpm;
}

TimelineActions::Align TimelineActions::alignClipToBarGrid(const juce::String& clipId)
{
    constexpr double kAlignedEpsilonMs = 0.5;
    const auto* clip = model().findClip(clipId);
    if (clip == nullptr || clip->locked) return Align::skip;
    const double projectBpm = model().bpm;
    if (projectBpm <= 0.0) return Align::skip;
    const auto* item = model().findLibraryItem(clip->libraryItemId);
    if (item == nullptr) return Align::skip;

    // The source beat grid (sourceBeatGrid.ts resolveSourceBeatGrid): none for a one-shot.
    const auto* source = item->sourceItemId.isNotEmpty() ? model().findLibraryItem(item->sourceItemId) : nullptr;
    const auto audioType = item->raw.getProperty("audioType", {}).toString();
    if (audioType == "simple" || (audioType.isEmpty() && source != nullptr && source->raw.getProperty("audioType", {}).toString() == "simple"))
        return Align::skip;
    double sourceBpm = 0.0;
    for (const auto* cursor = item; cursor != nullptr && sourceBpm <= 0.0;
         cursor = cursor->sourceItemId.isNotEmpty() ? model().findLibraryItem(cursor->sourceItemId) : nullptr)
        sourceBpm = cursor->bpm;
    const auto& beats = !item->beats.empty() ? item->beats : (source != nullptr ? source->beats : item->beats);
    if (sourceBpm <= 0.0 || beats.empty()) return Align::skip;
    const double anchorMs = (item->beatAnchorSec > 0.0 || item->beats.empty() ? item->beatAnchorSec : beats.front()) * 1000.0
                            + clip->beatOffsetMs;
    const double spacingMs = 60000.0 / sourceBpm;

    // The clip's beats must share the project's spacing, or aligning one beat means nothing.
    const double ratio = clip->effectiveWarpActive ? clip->effectiveTempoRatio : 1.0;
    const double projectBeatMs = 60000.0 / projectBpm;
    if (std::abs(spacingMs / ratio - projectBeatMs) / projectBeatMs > 0.01) return Align::skip;

    // First source beat at or after the window start (firstSourceBeatMsAtOrAfter).
    const double relative = (clip->inMs - anchorMs) / spacingMs;
    const double nearest = std::round(relative);
    const double index = std::abs(relative - nearest) <= 1e-6 ? nearest : std::ceil(relative);
    const double firstBeatSourceMs = anchorMs + index * spacingMs;
    if (firstBeatSourceMs > clip->inMs + clip->durationMs) return Align::skip;
    const double firstBeatMs = clip->offsetMs + (firstBeatSourceMs - clip->inMs) / ratio;
    const double shiftMs = std::round(firstBeatMs / projectBeatMs) * projectBeatMs - firstBeatMs;
    if (std::abs(shiftMs) < kAlignedEpsilonMs) return Align::skip;

    double target = clip->offsetMs + shiftMs;
    while (target < -1e-6) target += projectBeatMs; // one beat forward rather than before the origin
    target = juce::jmax(0.0, target);
    if (std::abs(target - clip->offsetMs) < kAlignedEpsilonMs) return Align::skip;
    // Only into clear space: never bump into a gap (it would misalign) or over a neighbour.
    if (const auto* track = model().findTrack(clip->trackId))
        for (const auto& other : track->clips)
            if (other.id != clipId && target < other.endMs() - 0.5 && target + clip->effectiveDurationMs > other.offsetMs + 0.5)
                return Align::blocked;
    // Copied: moving refreshes the mirror, which would leave `clip` pointing at the old model.
    const juce::String trackId = clip->trackId;
    moveClip(clipId, target, trackId);
    return Align::moved;
}

int TimelineActions::alignItemClipsToGrid(const juce::String& itemId)
{
    std::vector<juce::String> ids;
    for (const auto& track : model().tracks)
        for (const auto& c : track.clips)
            if (c.libraryItemId == itemId && !c.pendingAutoWarp) ids.push_back(c.id);
    if (ids.empty()) return 0;
    int blocked = 0;
    // Its own undo step, apart from the import that triggered it (libraryImportActions.ts).
    engine.inUndoGroup("Align to beat grid",
                       [&]
                       {
                           for (const auto& id : ids)
                               if (alignClipToBarGrid(id) == Align::blocked) ++blocked;
                       });
    return blocked;
}

double TimelineActions::snap(double ms) const
{
    if (snapStepMs() <= 0.0) return juce::jmax(0.0, ms);
    // The nearest grid step in beats, along the tempo map when there is one.
    const double stepBeats = beatsPerSnapStep(snapGrid());
    const auto curve = model().tempoCurve();
    return juce::jmax(0.0, curve.msAtBeat(std::round(curve.beatAt(ms) / stepBeats) * stepBeats));
}

void TimelineActions::replayClipProperties(const ClipModel& source, const juce::String& newClipId, bool includeLock)
{
    if (source.name.isNotEmpty()) engine.send("CLIP_RENAME", obj({{"clipId", newClipId}, {"name", source.name}}));
    // Active (or still-pending) warp, so the new clip plays at the same tempo.
    if (source.warpEnabled || source.pendingAutoWarp)
    {
        engine.send("CLIP_SET_WARP",
                    obj({{"clipId", newClipId},
                         {"warpEnabled", source.warpEnabled ? juce::var(true) : juce::var()},
                         {"warpMode", source.raw.getProperty("warpMode", {})},
                         {"tempoRatio", source.raw.getProperty("tempoRatio", {})},
                         {"semitones", source.raw.getProperty("semitones", {})},
                         {"cents", source.raw.getProperty("cents", {})},
                         {"pendingAutoWarp", source.pendingAutoWarp ? juce::var(true) : juce::var()}}));
    }
    if (source.reversed) engine.send("CLIP_SET_REVERSED", obj({{"clipId", newClipId}, {"reversed", true}}));
    if (std::abs(source.gainDb) > 1.0e-6) engine.send("CLIP_SET_GAIN", obj({{"clipId", newClipId}, {"gainDb", source.gainDb}}));
    if (std::abs(source.beatOffsetMs) > 0.0)
        engine.send("CLIP_SET_BEAT_OFFSET", obj({{"clipId", newClipId}, {"beatOffsetMs", source.beatOffsetMs}}));
    if (includeLock && source.locked) engine.send("CLIP_SET_LOCKED", obj({{"clipId", newClipId}, {"locked", true}}));
}

void TimelineActions::setClipGain(const juce::String& clipId, double gainDb)
{
    engine.send("CLIP_SET_GAIN", obj({{"clipId", clipId}, {"gainDb", juce::jlimit(-24.0, 12.0, gainDb)}}));
}

juce::String TimelineActions::splitClipAt(const juce::String& clipId, double atMs)
{
    const auto* found = model().findClip(clipId);
    if (found == nullptr) return {};
    const ClipModel clip = *found; // the mirror rebuilds while we send
    if (clip.locked)
    {
        notify("Locked clips cannot be split. Unlock the clip first.", true);
        return {};
    }
    if (const auto* item = model().findLibraryItem(clip.libraryItemId); item != nullptr && item->kind == "clip")
    {
        notify("Linked clips cannot be split on the timeline and must be edited in the Clip Editor.", false);
        return {};
    }

    const double ratio = effectiveRatio(clip);
    const double clipEnd = clip.offsetMs + clip.durationMs / ratio;
    if (atMs <= clip.offsetMs + 1.0 || atMs >= clipEnd - 1.0) return {};

    const double splitTimeline = atMs - clip.offsetMs;
    const double splitSource = splitTimeline * ratio;
    // Reverse plays the window backwards: the timeline-left half is the window's tail.
    const double leftInMs = clip.reversed ? clip.inMs + (clip.durationMs - splitSource) : clip.inMs;
    const double rightDuration = clip.durationMs - splitSource;
    const double rightInMs = clip.reversed ? clip.inMs : clip.inMs + splitSource;
    // Named halves rather than structured bindings: C++17 lambdas cannot capture bindings.
    const auto envelopeHalves = envelope::splitAt(clip.envelope, splitTimeline);
    const auto& leftEnvelope = envelopeHalves.first;
    const auto& rightEnvelope = envelopeHalves.second;
    const auto newId = EngineClient::newId("clip");

    engine.inUndoGroup("Split clip",
                       [&]
                       {
                           engine.send("CLIP_TRIM", obj({{"clipId", clipId},
                                                         {"startMs", clip.offsetMs},
                                                         {"inMs", leftInMs},
                                                         {"durationMs", splitSource}}));
                           engine.send("CLIP_ADD", obj({{"trackId", clip.trackId},
                                                        {"clipId", newId},
                                                        {"libraryItemId", clip.libraryItemId},
                                                        {"positionMs", atMs},
                                                        {"inMs", rightInMs},
                                                        {"durationMs", rightDuration},
                                                        {"colorIndex", clip.colorIndex >= 0 ? juce::var(clip.colorIndex)
                                                                                             : juce::var()}}));
                           replayClipProperties(clip, newId, /*includeLock*/ false);
                           // The end-of-clip turntable effect moves to the right half.
                           if (clip.brake)
                           {
                               engine.send("CLIP_SET_BRAKE", obj({{"clipId", clipId}, {"on", false}}));
                               engine.send("CLIP_SET_BRAKE", obj({{"clipId", newId}, {"on", true}}));
                           }
                           if (clip.backspin)
                           {
                               engine.send("CLIP_SET_BACKSPIN", obj({{"clipId", clipId}, {"on", false}}));
                               engine.send("CLIP_SET_BACKSPIN", obj({{"clipId", newId}, {"on", true}}));
                           }
                           if (clip.envelope.size() >= 2)
                           {
                               setClipEnvelope(clipId, leftEnvelope);
                               if (!rightEnvelope.empty()) setClipEnvelope(newId, rightEnvelope);
                           }
                       });
    return newId;
}

juce::String TimelineActions::duplicateClip(const juce::String& clipId)
{
    const auto* found = model().findClip(clipId);
    if (found == nullptr) return {};
    const ClipModel clip = *found;
    const auto* track = model().findTrack(clip.trackId);
    if (track == nullptr) return {};

    // Appends after the last duplicate of this clip, if it is still on the same track.
    const ClipModel* tail = &clip;
    if (const auto it = duplicateTailBySource.find(clipId); it != duplicateTailBySource.end())
        if (const auto* t = model().findClip(it->second); t != nullptr && t->trackId == clip.trackId) tail = t;
    const double newStart = tail->endMs();
    const double length = clip.effectiveDurationMs;
    for (const auto& other : track->clips)
    {
        if (other.id == clipId || other.id == tail->id) continue;
        if (newStart < other.endMs() - kClipFitEpsilonMs && newStart + length > other.offsetMs + kClipFitEpsilonMs)
        {
            notify("Not enough space to duplicate clip after the last duplicate.", true);
            return {};
        }
    }

    const auto newId = duplicateClipTo(clipId, clip.trackId, newStart);
    duplicateTailBySource[clipId] = newId;
    return newId;
}

void TimelineActions::removeClips(const std::vector<juce::String>& clipIds)
{
    std::vector<juce::String> removable;
    for (const auto& id : clipIds)
        if (const auto* c = model().findClip(id); c != nullptr && !c->locked) removable.push_back(id);
    if (removable.size() < clipIds.size()) notify("Locked clips were left in place.", false);
    if (removable.empty()) return;
    engine.inUndoGroup(removable.size() > 1 ? "Delete clips" : "Delete clip",
                       [&]
                       {
                           for (const auto& id : removable) engine.send("CLIP_REMOVE", obj({{"clipId", id}}));
                       });
}

juce::String TimelineActions::duplicateClipTo(const juce::String& clipId, const juce::String& trackId, double positionMs)
{
    const auto* found = model().findClip(clipId);
    if (found == nullptr || model().findTrack(trackId) == nullptr) return {};
    const ClipModel clip = *found;
    const auto newId = EngineClient::newId("clip");
    engine.inUndoGroup("Duplicate clip",
                       [&]
                       {
                           engine.send("CLIP_ADD", obj({{"trackId", trackId},
                                                        {"clipId", newId},
                                                        {"libraryItemId", clip.libraryItemId},
                                                        {"positionMs", juce::jmax(0.0, positionMs)},
                                                        {"inMs", clip.inMs},
                                                        {"durationMs", clip.durationMs},
                                                        {"colorIndex", clip.colorIndex >= 0 ? juce::var(clip.colorIndex)
                                                                                             : juce::var()}}));
                           replayClipProperties(clip, newId, /*includeLock*/ true);
                           if (clip.envelope.size() >= 2) setClipEnvelope(newId, clip.envelope);
                       });
    return newId;
}

int TimelineActions::joinClips(const std::vector<juce::String>& clipIds)
{
    // Selected clips by track, in timeline order.
    std::map<juce::String, std::vector<ClipModel>> byTrack;
    for (const auto& id : clipIds)
        if (const auto* c = model().findClip(id); c != nullptr && !c->locked) byTrack[c->trackId].push_back(*c);
    int joins = 0;
    engine.inUndoGroup("Join clips",
                       [&]
                       {
                           for (auto& [trackId, clips] : byTrack)
                           {
                               std::sort(clips.begin(), clips.end(), [](const ClipModel& a, const ClipModel& b) { return a.offsetMs < b.offsetMs; });
                               for (size_t i = 0; i + 1 < clips.size();)
                               {
                                   auto& a = clips[i];
                                   const auto& b = clips[i + 1];
                                   const bool sameCut = a.libraryItemId == b.libraryItemId && a.reversed == b.reversed
                                                        && std::abs(a.endMs() - b.offsetMs) < 1.0
                                                        && std::abs(a.inMs + a.durationMs - b.inMs) < 1.0
                                                        && std::abs(effectiveRatio(a) - effectiveRatio(b)) < 1e-6
                                                        && std::abs(a.semitones - b.semitones) < 1e-6;
                                   if (!sameCut)
                                   {
                                       ++i;
                                       continue;
                                   }
                                   // The right piece goes; the left one grows over where it was.
                                   const double end = b.endMs();
                                   removeClips({b.id});
                                   trimClip(a.id, a.offsetMs, end);
                                   a.durationMs += b.durationMs;
                                   a.effectiveDurationMs = end - a.offsetMs;
                                   clips.erase(clips.begin() + static_cast<std::ptrdiff_t>(i + 1));
                                   ++joins;
                               }
                           }
                       });
    if (joins == 0) notify("Nothing to join: select clips split from one another, end to end.", false);
    return joins;
}

void TimelineActions::moveClip(const juce::String& clipId, double positionMs, const juce::String& trackId)
{
    engine.send("CLIP_MOVE", obj({{"clipId", clipId},
                                  {"positionMs", juce::jmax(0.0, positionMs)},
                                  {"trackId", trackId},
                                  {"commit", true}}));
}

void TimelineActions::trimClip(const juce::String& clipId, double newStartMs, double newEndMs)
{
    const auto* clip = model().findClip(clipId);
    if (clip == nullptr || clip->locked) return;
    const double ratio = effectiveRatio(*clip);
    newStartMs = juce::jmax(0.0, newStartMs);
    if (newEndMs - newStartMs < 1.0) return;

    // Source window bounds: [inMs, inMs + durationMs) maps to [offsetMs, endMs) - along the
    // tempo map for a clip that follows it, where the ratio changes along the way.
    const auto curve = model().tempoCurve();
    const bool follows = clip->tempoFollowBpm > 0.0 && !model().tempoMap.empty();
    const double sourceStart = follows ? clip->inMs + curve.sourceMsBetween(clip->offsetMs, newStartMs, clip->tempoFollowBpm)
                                       : clip->inMs + (newStartMs - clip->offsetMs) * ratio;
    const double sourceLength = follows ? curve.sourceMsBetween(newStartMs, newEndMs, clip->tempoFollowBpm)
                                        : (newEndMs - newStartMs) * ratio;
    const auto* item = model().findLibraryItem(clip->libraryItemId);
    const double sourceTotal = item != nullptr && item->durationMs > 0.0 ? item->durationMs : 1.0e12;
    const double clampedStart = juce::jlimit(0.0, sourceTotal, sourceStart);
    const double clampedLength = juce::jmin(sourceLength - (clampedStart - sourceStart), sourceTotal - clampedStart);
    if (clampedLength < 1.0) return;
    const double timelineStart = follows ? curve.timelineAfterSource(clip->offsetMs, clampedStart - clip->inMs, clip->tempoFollowBpm)
                                         : clip->offsetMs + (clampedStart - clip->inMs) / ratio;
    engine.send("CLIP_TRIM", obj({{"clipId", clipId},
                                  {"startMs", timelineStart},
                                  {"inMs", clampedStart},
                                  {"durationMs", clampedLength}}));
}

void TimelineActions::setClipLocked(const juce::String& clipId, bool locked)
{
    engine.send("CLIP_SET_LOCKED", obj({{"clipId", clipId}, {"locked", locked}}));
}

void TimelineActions::setClipMasterTempo(const juce::String& clipId, bool on)
{
    engine.send("CLIP_SET_WARP", obj({{"clipId", clipId}, {"warpMode", on ? juce::String(kDefaultWarpMode) : juce::String("repitch")}}));
}

void TimelineActions::setClipKeyShift(const juce::String& clipId, int semitones)
{
    const auto* clip = model().findClip(clipId);
    if (clip == nullptr) return;
    auto payload = obj({{"clipId", clipId}, {"warpEnabled", true}, {"semitones", juce::jlimit(-12, 12, semitones)}, {"cents", 0}});
    if (!clip->warpEnabled && !clip->pendingAutoWarp) payload.getDynamicObject()->setProperty("tempoRatio", 1.0);
    engine.send("CLIP_SET_WARP", payload);
}

void TimelineActions::setClipColor(const juce::String& clipId, int colorIndex)
{
    engine.send("CLIP_COLOR", obj({{"clipId", clipId}, {"colorIndex", colorIndex}}));
}

void TimelineActions::renameClip(const juce::String& clipId, const juce::String& name)
{
    engine.send("CLIP_RENAME", obj({{"clipId", clipId}, {"name", name}}));
}

void TimelineActions::setClipReversed(const juce::String& clipId, bool reversed)
{
    engine.send("CLIP_SET_REVERSED", obj({{"clipId", clipId}, {"reversed", reversed}}));
}

void TimelineActions::setClipEnvelope(const juce::String& clipId, const std::vector<EnvelopePoint>& points)
{
    engine.send("CLIP_SET_ENVELOPE", obj({{"clipId", clipId}, {"points", pointsVar(envelope::sanitize(points))}}));
}

juce::String TimelineActions::addClip(const juce::String& trackId, const LibraryItemModel& libraryItem, double positionMs)
{
    // A copy: `libraryItem` lives in the mirrored model, which a send below can rebuild (the
    // first clip seeds the project tempo, and a listener refreshes the mirror on that).
    const LibraryItemModel item = libraryItem;
    // projectClipLibraryActions.addClipFromLibrary + applyDropTimeWarp.
    const bool savedClip = item.kind == "clip";
    const double inMs = savedClip ? juce::jmax(0.0, static_cast<double>(item.raw.getProperty("sourceInMs", 0.0))) : 0.0;
    const double durationMs = savedClip && static_cast<double>(item.raw.getProperty("sourceDurationMs", 0.0)) > 0.0
                                  ? static_cast<double>(item.raw.getProperty("sourceDurationMs", 0.0))
                                  : item.durationMs;
    const auto newId = EngineClient::newId("clip");
    engine.inUndoGroup("Add clip",
                       [&]
                       {
                           engine.send("CLIP_ADD", obj({{"trackId", trackId},
                                                        {"clipId", newId},
                                                        {"libraryItemId", item.id},
                                                        {"positionMs", juce::jmax(0.0, positionMs)},
                                                        {"inMs", inMs},
                                                        {"durationMs", durationMs}}));
                           if (item.name.trim().isNotEmpty()) engine.send("CLIP_RENAME", obj({{"clipId", newId}, {"name", item.name.trim()}}));

                           // A saved clip brings its own warp and pitch.
                           const auto& r = item.raw;
                           if (savedClip && (r.hasProperty("warpEnabled") || r.hasProperty("tempoRatio") || r.hasProperty("semitones")
                                             || r.hasProperty("cents") || r.hasProperty("warpMode")))
                           {
                               engine.send("CLIP_SET_WARP", obj({{"clipId", newId},
                                                                 {"warpEnabled", r.getProperty("warpEnabled", {})},
                                                                 {"warpMode", r.getProperty("warpMode", {})},
                                                                 {"tempoRatio", r.getProperty("tempoRatio", {})},
                                                                 {"semitones", r.getProperty("semitones", {})},
                                                                 {"cents", r.getProperty("cents", {})}}));
                               return;
                           }
                           // One-shot samples have no tempo to match (their own, or their source's, type).
                           auto isSimple = [this](const LibraryItemModel& i)
                           {
                               const auto type = i.raw.getProperty("audioType", {}).toString();
                               if (type.isNotEmpty()) return type == "simple";
                               if (const auto* src = model().findLibraryItem(i.sourceItemId))
                                   return src->raw.getProperty("audioType", {}).toString() == "simple";
                               return false;
                           };
                           if (isSimple(item) || !autoWarpOnDrop) return;
                           // The tempo owner: the item, or what it was derived from.
                           double sourceBpm = 0.0;
                           const LibraryItemModel* cursor = &item;
                           for (int hops = 0; cursor != nullptr && hops < 8; ++hops)
                           {
                               if (cursor->bpm > 0.0)
                               {
                                   sourceBpm = cursor->bpm;
                                   break;
                               }
                               cursor = cursor->sourceItemId.isNotEmpty() ? model().findLibraryItem(cursor->sourceItemId) : nullptr;
                           }
                           if (sourceBpm <= 0.0)
                           {
                               // Analysis still running: the engine warps it once the tempo lands. This
                               // holds before the project has a tempo too: tracks dropped straight after
                               // import all arrive unanalysed, the first to finish seeds the project
                               // (and so plays at 1.0), and the rest are stretched to it.
                               if (!savedClip) engine.send("CLIP_SET_WARP", obj({{"clipId", newId}, {"pendingAutoWarp", true}}));
                               return;
                           }
                           // A known tempo with no project tempo yet: this clip is the one that seeds it.
                           if (!model().bpmSeeded) return;
                           const double ratio = model().bpm / sourceBpm;
                           // Worth warping only when it moves the clip's end (WARP_NEGLIGIBLE_DRIFT_MS).
                           if (model().bpm <= 0.0 || std::abs(durationMs - durationMs / ratio) < 1.0) return;
                           engine.send("CLIP_SET_WARP", obj({{"clipId", newId}, {"warpEnabled", true}, {"warpMode", kDefaultWarpMode}}));
                       });
    return newId;
}

juce::String TimelineActions::addTrack()
{
    const auto id = EngineClient::newId("track");
    const int index = static_cast<int>(model().tracks.size());
    engine.send("TRACK_ADD",
                obj({{"trackId", id}, {"name", "Track " + juce::String(index + 1)}, {"colorIndex", index % 16}}));
    return id;
}

void TimelineActions::removeTrack(const juce::String& trackId)
{
    engine.send("TRACK_REMOVE", obj({{"trackId", trackId}}));
}

void TimelineActions::renameTrack(const juce::String& trackId, const juce::String& name)
{
    engine.send("TRACK_RENAME", obj({{"trackId", trackId}, {"name", name}}));
}

void TimelineActions::setTrackGain(const juce::String& trackId, double gain)
{
    engine.send("TRACK_GAIN", obj({{"trackId", trackId}, {"gain", gain}}));
}

void TimelineActions::setTrackPan(const juce::String& trackId, double pan)
{
    engine.send("TRACK_SET_PAN", obj({{"trackId", trackId}, {"pan", juce::jlimit(-1.0, 1.0, pan)}}));
}

void TimelineActions::setTrackMuted(const juce::String& trackId, bool muted)
{
    engine.send("TRACK_MUTE", obj({{"trackId", trackId}, {"muted", muted}}));
}

void TimelineActions::setTrackSoloed(const juce::String& trackId, bool soloed)
{
    engine.send("TRACK_SOLO", obj({{"trackId", trackId}, {"soloed", soloed}}));
}

void TimelineActions::setTrackColor(const juce::String& trackId, int colorIndex)
{
    engine.send("TRACK_SET_COLOR", obj({{"trackId", trackId}, {"colorIndex", colorIndex}}));
}

void TimelineActions::setTrackHeight(const juce::String& trackId, double heightPx)
{
    engine.send("TRACK_SET_HEIGHT", obj({{"trackId", trackId}, {"heightPx", heightPx}}));
}

void TimelineActions::moveTrack(const juce::String& trackId, int newIndex)
{
    engine.send("TRACK_REORDER", obj({{"trackId", trackId}, {"newIndex", newIndex}}));
}

void TimelineActions::play()
{
    engine.send("TRANSPORT_PLAY");
}

void TimelineActions::pause()
{
    engine.send("TRANSPORT_PAUSE");
}

void TimelineActions::stop()
{
    engine.send("TRANSPORT_STOP");
}

void TimelineActions::seek(double positionMs)
{
    engine.send("TRANSPORT_SEEK", obj({{"positionMs", juce::jmax(0.0, positionMs)}}));
}

void TimelineActions::addMarker(double positionMs)
{
    engine.send("PROJECT_MARKER_ADD",
                obj({{"markerId", EngineClient::newId("marker")}, {"positionMs", juce::jmax(0.0, positionMs)}}));
}

void TimelineActions::removeMarker(const juce::String& markerId)
{
    engine.send("PROJECT_MARKER_REMOVE", obj({{"markerId", markerId}}));
}

void TimelineActions::moveMarker(const juce::String& markerId, double positionMs)
{
    engine.send("PROJECT_MARKER_MOVE", obj({{"markerId", markerId}, {"positionMs", juce::jmax(0.0, positionMs)}}));
}

void TimelineActions::setTimelineSelection(std::optional<std::pair<double, double>> range, bool loop)
{
    juce::var selection;
    if (range.has_value() && range->second > range->first)
        selection = obj({{"startMs", range->first}, {"endMs", range->second}});
    auto* p = new juce::DynamicObject();
    p->setProperty("timelineSelection", selection);
    p->setProperty("loopTimelineSelection", loop && !selection.isVoid());
    engine.send("PROJECT_SET_VIEW", juce::var(p));
}

void TimelineActions::setView(double pxPerSecond, double scrollXPx, const juce::String& selectedTrackId)
{
    engine.send("PROJECT_SET_VIEW", obj({{"pxPerSecond", pxPerSecond},
                                         {"scrollX", scrollXPx},
                                         {"selectedTrackId", selectedTrackId.isNotEmpty() ? juce::var(selectedTrackId)
                                                                                          : juce::var()}}));
}

void TimelineActions::setSnapGrid(const juce::String& grid)
{
    engine.send("PROJECT_SET_VIEW", obj({{"snapGrid", grid}}));
}

void TimelineActions::setMasterVolume(double gain)
{
    engine.send("PROJECT_SET_MASTER_VOLUME", obj({{"gain", gain}}));
}

void TimelineActions::setTempoMap(const std::vector<TempoPoint>& points)
{
    juce::Array<juce::var> list;
    for (const auto& p : points) list.add(obj({{"timeMs", p.timeMs}, {"bpm", p.bpm}}));
    engine.send("TEMPO_MAP_SET", obj({{"points", list}}));
}

void TimelineActions::setBpm(double bpm, bool autoWarp)
{
    engine.send("PROJECT_SET_BPM", obj({{"bpm", bpm}, {"autoWarp", autoWarp}}));
}

// ── clip transforms, library, samples ───────────────────────────────────────
void TimelineActions::setClipBrake(const juce::String& clipId, bool on)
{
    engine.send("CLIP_SET_BRAKE", obj({{"clipId", clipId}, {"on", on}}));
}

void TimelineActions::setClipBackspin(const juce::String& clipId, bool on)
{
    engine.send("CLIP_SET_BACKSPIN", obj({{"clipId", clipId}, {"on", on}}));
}

juce::String TimelineActions::saveClipToLibrary(const juce::String& clipId)
{
    const auto* clip = model().findClip(clipId);
    if (clip == nullptr || clip->durationMs <= 0.0) return {};
    // A library clip points back at the source file's item (a library clip's own source).
    const auto* direct = model().findLibraryItem(clip->libraryItemId);
    const auto* source = direct;
    if (direct != nullptr && direct->kind == "clip" && direct->sourceItemId.isNotEmpty())
        if (const auto* s = model().findLibraryItem(direct->sourceItemId)) source = s;
    if (source == nullptr) return {};
    const double inMs = juce::jmax(0.0, clip->inMs);
    const double durationMs = clip->durationMs;
    for (const auto& item : model().library)
        if (item.kind == "clip" && item.sourceItemId == source->id
            && std::abs(static_cast<double>(item.raw.getProperty("sourceInMs", -1.0)) - inMs) < 0.5
            && std::abs(static_cast<double>(item.raw.getProperty("sourceDurationMs", -1.0)) - durationMs) < 0.5)
        {
            if (clip->libraryItemId != item.id) engine.send("CLIP_REBIND", obj({{"clipId", clipId}, {"libraryItemId", item.id}}));
            return item.id;
        }

    auto time = [](double ms)
    {
        const int total = static_cast<int>(ms / 1000.0);
        return juce::String(total / 60) + ":" + juce::String(total % 60).paddedLeft('0', 2);
    };
    const auto name = clip->name.trim().isNotEmpty()
                          ? clip->name.trim()
                          : source->displayName() + " (" + time(inMs) + "-" + time(inMs + durationMs) + ")";
    int next = 1;
    for (const auto& item : model().library)
        if (item.id.startsWithChar('l') && item.id.substring(1).containsOnly("0123456789"))
            next = juce::jmax(next, item.id.substring(1).getIntValue() + 1);
    const auto itemId = "l" + juce::String(next);
    const bool warpActive = clip->effectiveWarpActive;
    engine.inUndoGroup("Save clip to library",
                       [&]
                       {
                           engine.send("LIBRARY_ADD", obj({{"itemId", itemId},
                                                           {"filePath", source->filePath},
                                                           {"fileName", source->fileName},
                                                           {"kind", "clip"},
                                                           {"name", name},
                                                           {"durationMs", source->durationMs},
                                                           {"sampleRate", source->sampleRate},
                                                           {"channelCount", source->channelCount},
                                                           {"key", source->key},
                                                           {"sourceItemId", source->id},
                                                           {"sourceClipId", clipId},
                                                           {"sourceInMs", inMs},
                                                           {"sourceDurationMs", durationMs},
                                                           {"mediaId", source->mediaId},
                                                           {"warpEnabled", clip->warpEnabled},
                                                           {"warpMode", clip->warpMode.isNotEmpty() ? juce::var(clip->warpMode) : juce::var()},
                                                           {"tempoRatio", warpActive ? clip->effectiveTempoRatio : clip->tempoRatio},
                                                           {"semitones", clip->semitones},
                                                           {"cents", clip->cents}}));
                           engine.send("CLIP_REBIND", obj({{"clipId", clipId}, {"libraryItemId", itemId}}));
                       });
    return itemId;
}

void TimelineActions::saveClipAsSample(const juce::String& clipId, const juce::String& audioType)
{
    const auto* clip = model().findClip(clipId);
    if (clip == nullptr) return;
    const auto* item = model().findLibraryItem(clip->libraryItemId);
    const auto name = clip->name.trim().isNotEmpty() ? clip->name.trim()
                      : item != nullptr               ? juce::File(item->fileName).getFileNameWithoutExtension()
                                                      : juce::String("Sample");
    engine.send("CLIP_SAVE_AS_SAMPLE", obj({{"clipId", clipId},
                                            {"itemId", "sample-" + juce::Uuid().toDashedString()},
                                            {"sampleName", name},
                                            {"audioType", audioType}}));
}

int TimelineActions::sliceClipAtMarkers(const juce::String& clipId, std::vector<double> markers, const juce::String& audioType)
{
    const auto* clip = model().findClip(clipId);
    if (clip == nullptr) return 0;
    const double clipEnd = clip->inMs + clip->durationMs;
    std::sort(markers.begin(), markers.end());
    std::vector<double> bounds{clip->inMs};
    for (const double m : markers)
        if (m > clip->inMs + 1.0 && m < clipEnd - 1.0) bounds.push_back(m);
    bounds.push_back(clipEnd);
    juce::Array<juce::var> slices;
    for (size_t i = 0; i + 1 < bounds.size(); ++i)
        if (bounds[i + 1] - bounds[i] > 0.0)
            slices.add(obj({{"itemId", "sample-" + juce::Uuid().toDashedString()}, {"inMs", bounds[i]}, {"durationMs", bounds[i + 1] - bounds[i]}}));
    if (slices.isEmpty()) return 0;
    engine.send("CLIP_SLICE_TO_SAMPLES", obj({{"clipId", clipId}, {"audioType", audioType}, {"slices", slices}}));
    return slices.size();
}

int TimelineActions::chopClipToGrid(const juce::String& clipId, double beatsPerSlice, const juce::String& audioType)
{
    const auto* clip = model().findClip(clipId);
    if (clip == nullptr || beatsPerSlice <= 0.0) return 0;
    const auto* item = model().findLibraryItem(clip->libraryItemId);
    // The item's beat grid in source ms, or a rigid one from its tempo (or the project's).
    std::vector<double> beatsMs;
    if (item != nullptr)
        for (const double b : item->beats) beatsMs.push_back(b * 1000.0);
    const double clipEnd = clip->inMs + clip->durationMs;
    if (beatsMs.size() < 2)
    {
        beatsMs.clear();
        const double bpm = item != nullptr && item->bpm > 0.0 ? item->bpm : model().bpm;
        const double anchor = item != nullptr ? item->beatAnchorSec * 1000.0 : 0.0;
        const double step = 60000.0 / juce::jmax(1.0, bpm);
        for (double t = anchor - std::ceil(anchor / step) * step; t <= clipEnd + step; t += step) beatsMs.push_back(t);
    }
    // Positions every `beatsPerSlice` beats along the grid, interpolated between beats.
    auto beatTime = [&beatsMs](double beat)
    {
        const auto i = static_cast<size_t>(juce::jlimit(0.0, static_cast<double>(beatsMs.size() - 2), std::floor(beat)));
        const double frac = beat - static_cast<double>(i);
        return beatsMs[i] + (beatsMs[i + 1] - beatsMs[i]) * frac;
    };
    std::vector<double> bounds{clip->inMs};
    for (double beat = 0.0; beat < static_cast<double>(beatsMs.size() - 1); beat += beatsPerSlice)
    {
        const double t = beatTime(beat);
        if (t > clip->inMs + 1.0 && t < clipEnd - 1.0) bounds.push_back(t);
    }
    bounds.push_back(clipEnd);
    juce::Array<juce::var> slices;
    for (size_t i = 0; i + 1 < bounds.size(); ++i)
        if (bounds[i + 1] - bounds[i] > 0.0)
            slices.add(obj({{"itemId", "sample-" + juce::Uuid().toDashedString()},
                            {"inMs", bounds[i]},
                            {"durationMs", bounds[i + 1] - bounds[i]}}));
    if (slices.isEmpty()) return 0;
    engine.send("CLIP_SLICE_TO_SAMPLES", obj({{"clipId", clipId}, {"audioType", audioType}, {"slices", slices}}));
    return slices.size();
}

// ── crossfades & beat repeat ────────────────────────────────────────────────
void TimelineActions::createTransition(const juce::String& trackId, const juce::String& leftClipId,
                                       const juce::String& rightClipId, const juce::String& recipe)
{
    engine.send("TRANSITION_CREATE", obj({{"trackId", trackId},
                                          {"leftClipId", leftClipId},
                                          {"rightClipId", rightClipId},
                                          {"recipe", recipe.isNotEmpty() ? obj({{"kind", recipe}}) : juce::var()}}));
}

void TimelineActions::deleteTransition(const juce::String& trackId, const juce::String& transitionId)
{
    engine.send("TRANSITION_DELETE", obj({{"trackId", trackId}, {"transitionId", transitionId}}));
}

void TimelineActions::setTransitionRecipe(const juce::String& trackId, const juce::String& transitionId,
                                          const juce::String& recipe)
{
    engine.send("TRANSITION_SET_RECIPE",
                obj({{"trackId", trackId}, {"transitionId", transitionId}, {"recipe", obj({{"kind", recipe}})}}));
}

void TimelineActions::maybeCreateTransitionAfterTrim(const juce::String& clipId, bool rightEdge)
{
    constexpr double minOverlapMs = 10.0;
    constexpr double eps = 1e-6;
    const auto* clip = model().findClip(clipId);
    if (clip == nullptr) return;
    const auto* track = model().findTrack(clip->trackId);
    if (track == nullptr) return;
    for (const auto& other : track->clips)
    {
        if (other.id == clipId) continue;
        const auto& left = rightEdge ? *clip : other;
        const auto& right = rightEdge ? other : *clip;
        // A clean tail/head overlap: no equal starts, no containment.
        if (!(left.offsetMs + eps < right.offsetMs && right.offsetMs + eps < left.endMs() && left.endMs() <= right.endMs() + eps))
            continue;
        if (left.endMs() - right.offsetMs < minOverlapMs) continue;
        bool taken = false;
        for (const auto& t : track->transitions)
            taken = taken || t.leftClipId == left.id || t.rightClipId == right.id;
        if (taken) continue;
        createTransition(track->id, left.id, right.id);
        return;
    }
}

void TimelineActions::addBeatRepeat(const juce::String& trackId, double startBeat, double lengthBeats,
                                    const juce::String& division)
{
    engine.send("TRACK_BEAT_REPEAT_ADD", obj({{"trackId", trackId},
                                              {"startBeat", juce::jmax(0.0, startBeat)},
                                              {"lengthBeats", lengthBeats},
                                              {"division", division}}));
}

void TimelineActions::deleteBeatRepeat(const juce::String& trackId, const juce::String& regionId)
{
    engine.send("TRACK_BEAT_REPEAT_DELETE", obj({{"trackId", trackId}, {"regionId", regionId}}));
}

void TimelineActions::setAutomation(const juce::String& trackId, const juce::String& paramId,
                                    const std::vector<automation::Point>& points, const juce::String& gestureId,
                                    bool gestureEnd)
{
    auto sorted = points;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.timeMs < b.timeMs; });
    auto payload = obj({{"trackId", trackId}, {"paramId", paramId}, {"points", automation::toVar(sorted)}});
    if (gestureId.isNotEmpty()) payload.getDynamicObject()->setProperty("gestureId", gestureId);
    if (gestureEnd) payload.getDynamicObject()->setProperty("gestureEnd", true);
    engine.send("TRACK_SET_AUTOMATION", payload);
}

void TimelineActions::setAutomationLanes(const juce::String& trackId, const std::vector<automation::Lane>& lanes)
{
    engine.send("TRACK_SET_AUTOMATION_LANE_VIEW", obj({{"trackId", trackId}, {"lanes", automation::toVar(lanes)}}));
}

juce::StringArray TimelineActions::automatedParams(const juce::String& trackId) const
{
    juce::StringArray out;
    const auto* track = trackId == kMasterTrackId ? &model().master : model().findTrack(trackId);
    if (track == nullptr) return out;
    if (const auto* curves = track->raw.getProperty("automation", {}).getArray())
        for (const auto& curve : *curves)
            if (const auto* points = curve.getProperty("points", {}).getArray(); points != nullptr && !points->isEmpty())
                out.addIfNotAlreadyThere(curve.getProperty("paramId", {}).toString());
    return out;
}

void TimelineActions::toggleAutomation(const juce::String& trackId)
{
    const auto* track = trackId == kMasterTrackId ? &model().master : model().findTrack(trackId);
    if (track == nullptr) return;
    if (auto shown = automation::lanesOf(*track); !shown.empty())
    {
        hiddenLanes[trackId] = std::move(shown);
        setAutomationLanes(trackId, {});
        return;
    }
    std::vector<automation::Lane> next;
    if (const auto it = hiddenLanes.find(trackId); it != hiddenLanes.end()) next = it->second;
    if (next.empty())
        for (const auto& id : automatedParams(trackId)) next.push_back({id, automation::kDefaultLaneHeight});
    if (next.empty()) next.push_back({"level", automation::kDefaultLaneHeight});
    setAutomationLanes(trackId, next);
}

// ── clipboard ───────────────────────────────────────────────────────────────
bool TimelineActions::copyClips(const std::vector<juce::String>& clipIds)
{
    std::vector<const ClipModel*> clips;
    for (const auto& id : clipIds)
        if (const auto* c = model().findClip(id)) clips.push_back(c);
    if (clips.empty()) return false;
    auto trackIndexOf = [this](const juce::String& trackId)
    {
        for (size_t i = 0; i < model().tracks.size(); ++i)
            if (model().tracks[i].id == trackId) return static_cast<int>(i);
        return 0;
    };
    double anchorMs = clips.front()->offsetMs;
    int anchorTrack = trackIndexOf(clips.front()->trackId);
    for (const auto* c : clips)
    {
        anchorMs = juce::jmin(anchorMs, c->offsetMs);
        anchorTrack = juce::jmin(anchorTrack, trackIndexOf(c->trackId));
    }
    clipboard.clear();
    for (const auto* c : clips)
    {
        std::vector<juce::var> items;
        for (const auto* item = model().findLibraryItem(c->libraryItemId); item != nullptr && items.size() < 8;
             item = item->sourceItemId.isNotEmpty() ? model().findLibraryItem(item->sourceItemId) : nullptr)
            items.insert(items.begin(), item->raw);
        clipboard.push_back({*c, c->offsetMs - anchorMs, trackIndexOf(c->trackId) - anchorTrack, std::move(items)});
    }
    std::sort(clipboard.begin(), clipboard.end(), [](const auto& a, const auto& b)
              { return a.relTrackIndex != b.relTrackIndex ? a.relTrackIndex < b.relTrackIndex : a.relStartMs < b.relStartMs; });
    return true;
}

bool TimelineActions::cutClips(const std::vector<juce::String>& clipIds)
{
    if (!copyClips(clipIds)) return false;
    engine.inUndoGroup(clipIds.size() > 1 ? "Cut clips" : "Cut clip", [&] { removeClips(clipIds); });
    return true;
}

std::vector<juce::String> TimelineActions::paste(const juce::String& trackId, double positionMs)
{
    if (clipboard.empty()) return {};
    int anchorTrack = -1;
    for (size_t i = 0; i < model().tracks.size(); ++i)
        if (model().tracks[i].id == trackId) anchorTrack = static_cast<int>(i);
    if (anchorTrack < 0)
    {
        notify("Select a track to paste onto first.", true);
        return {};
    }
    struct Placement
    {
        const ClipboardItem* item;
        const TrackModel* track;
        double startMs;
        double length;
    };
    std::vector<Placement> placements;
    const int lastTrack = static_cast<int>(model().tracks.size()) - 1;
    for (const auto& item : clipboard)
    {
        const auto& track = model().tracks[static_cast<size_t>(juce::jmin(lastTrack, anchorTrack + item.relTrackIndex))];
        const double length = item.clip.effectiveDurationMs > 0.0 ? item.clip.effectiveDurationMs : item.clip.durationMs;
        placements.push_back({&item, &track, juce::jmax(0.0, positionMs) + item.relStartMs, length});
    }
    // Free space only: never overwrite or push clips.
    for (size_t i = 0; i < placements.size(); ++i)
    {
        const auto& p = placements[i];
        for (const auto& other : p.track->clips)
            if (p.startMs < other.endMs() - kClipFitEpsilonMs && p.startMs + p.length > other.offsetMs + kClipFitEpsilonMs)
            {
                notify(placements.size() > 1 ? "The copied clips don't fit here - they'd overlap clips already on the timeline."
                                             : "Not enough space to paste the clip here.",
                       true);
                return {};
            }
        for (size_t j = 0; j < i; ++j)
            if (placements[j].track == p.track && p.startMs < placements[j].startMs + placements[j].length
                && p.startMs + p.length > placements[j].startMs)
            {
                notify("The copied clips span more tracks than there are below this one. Select a higher track.", true);
                return {};
            }
    }
    std::vector<juce::String> created;
    std::set<juce::String> addedItems;
    engine.inUndoGroup(placements.size() > 1 ? "Paste clips" : "Paste clip",
                       [&]
                       {
                           for (const auto& p : placements)
                           {
                               // From another project: bring its library items along first.
                               // (The model is not refreshed in between: `placements` points into it.)
                               for (const auto& raw : p.item->libraryItems)
                               {
                                   const auto id = raw.getProperty("id", {}).toString();
                                   if (id.isEmpty() || model().findLibraryItem(id) != nullptr || !addedItems.insert(id).second) continue;
                                   auto* add = new juce::DynamicObject();
                                   if (const auto* o = raw.getDynamicObject())
                                       for (const auto& prop : o->getProperties()) add->setProperty(prop.name, prop.value);
                                   add->setProperty("itemId", id);
                                   engine.send("LIBRARY_ADD", juce::var(add));
                               }
                               const auto newId = EngineClient::newId("clip");
                               const auto& c = p.item->clip;
                               engine.send("CLIP_ADD", obj({{"trackId", p.track->id},
                                                            {"clipId", newId},
                                                            {"libraryItemId", c.libraryItemId},
                                                            {"positionMs", p.startMs},
                                                            {"inMs", c.inMs},
                                                            {"durationMs", c.durationMs},
                                                            {"colorIndex", c.colorIndex >= 0 ? juce::var(c.colorIndex) : juce::var()}}));
                               replayClipProperties(c, newId, false);
                               if (!c.envelope.empty()) setClipEnvelope(newId, c.envelope);
                               created.push_back(newId);
                           }
                       });
    return created;
}

} // namespace stemcel::native
