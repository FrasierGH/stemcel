#include "ProjectModel.h"

#include "EngineClient.h"
#include "ProjectSession.h"
#include "ProjectState.h"

namespace stemcel::native
{
namespace
{
double num(const juce::var& o, const char* key, double fallback = 0.0)
{
    const auto v = o.getProperty(key, {});
    return v.isVoid() || v.isUndefined() ? fallback : static_cast<double>(v);
}

bool flag(const juce::var& o, const char* key)
{
    return static_cast<bool>(o.getProperty(key, false));
}

juce::String str(const juce::var& o, const char* key)
{
    const auto v = o.getProperty(key, {});
    return v.isString() ? v.toString() : juce::String();
}

int intOr(const juce::var& o, const char* key, int fallback)
{
    const auto v = o.getProperty(key, {});
    return v.isVoid() || v.isUndefined() ? fallback : static_cast<int>(v);
}

ClipModel parseClip(const juce::var& c, const juce::String& trackId)
{
    ClipModel clip;
    clip.id = str(c, "id");
    clip.trackId = trackId;
    clip.libraryItemId = str(c, "libraryItemId");
    clip.name = str(c, "name");
    clip.offsetMs = num(c, "offsetMs");
    clip.durationMs = num(c, "durationMs");
    clip.inMs = num(c, "inMs");
    clip.effectiveDurationMs = num(c, "effectiveDurationMs", clip.durationMs);
    clip.tempoFollowBpm = num(c, "tempoFollowBpm", 0.0);
    clip.effectiveTempoRatio = num(c, "effectiveTempoRatio", 1.0);
    if (clip.effectiveTempoRatio <= 0.0) clip.effectiveTempoRatio = 1.0;
    clip.effectiveWarpActive = flag(c, "effectiveWarpActive");
    clip.colorIndex = intOr(c, "colorIndex", -1);
    clip.locked = flag(c, "locked");
    clip.reversed = flag(c, "reversed");
    clip.brake = flag(c, "brake");
    clip.backspin = flag(c, "backspin");
    clip.unresolved = flag(c, "unresolved");
    clip.warpEnabled = flag(c, "warpEnabled");
    clip.warpMode = str(c, "warpMode");
    clip.tempoRatio = num(c, "tempoRatio", 1.0);
    clip.semitones = num(c, "semitones");
    clip.cents = num(c, "cents");
    clip.beatOffsetMs = num(c, "beatOffsetMs");
    clip.scratchPatternId = str(c, "scratchPatternId");
    clip.pendingAutoWarp = flag(c, "pendingAutoWarp");
    clip.raw = c;
    clip.gainDb = num(c, "gainDb", 0.0);
    if (const auto* points = c.getProperty("sourceTempo", {}).getArray())
        for (const auto& p : *points) clip.sourceTempo.push_back({num(p, "sourceMs"), num(p, "bpm")});
    if (const auto* points = c.getProperty("envelopePoints", {}).getArray())
        for (const auto& p : *points) clip.envelope.push_back({num(p, "timeMs"), num(p, "gain", 1.0)});
    return clip;
}
} // namespace

ProjectModel ProjectModel::fromState(const juce::var& s)
{
    ProjectModel m;
    m.raw = s;
    m.filePath = str(s, "filePath");
    m.name = str(s, "name");
    m.dirty = flag(s, "dirty");
    m.bpm = num(s, "bpm", 120.0);
    m.bpmSeeded = flag(s, "bpmSeeded");
    m.projectLengthMs = num(s, "projectLengthMs");
    m.pxPerSecond = num(s, "viewPxPerSecond");
    m.scrollX = num(s, "viewScrollX");
    m.selectedTrackId = str(s, "viewSelectedTrack");
    m.snapGrid = str(s, "viewSnapGrid");
    m.masterVolume = num(s, "masterVolume", 1.0);
    m.loopSelection = flag(s, "loopTimelineSelection");
    if (const auto sel = s.getProperty("timelineSelection", {}); sel.isObject())
        m.timelineSelection = std::make_pair(num(sel, "startMs"), num(sel, "endMs"));

    if (const auto* tracks = s.getProperty("tracks", {}).getArray())
    {
        for (const auto& t : *tracks)
        {
            TrackModel track;
            track.raw = t;
            track.id = str(t, "id");
            track.name = str(t, "name");
            track.gain = num(t, "gain", 1.0);
            track.pan = num(t, "pan");
            track.muted = flag(t, "muted");
            track.soloed = flag(t, "soloed");
            track.heightPx = num(t, "heightPx");
            track.colorIndex = intOr(t, "colorIndex", -1);
            if (const auto* clips = t.getProperty("clips", {}).getArray())
                for (const auto& c : *clips) track.clips.push_back(parseClip(c, track.id));
            if (const auto* transitions = t.getProperty("transitions", {}).getArray())
                for (const auto& tr : *transitions)
                    track.transitions.push_back({str(tr, "id"), str(tr, "leftClipId"), str(tr, "rightClipId"),
                                                 tr.getProperty("recipe", {}).getProperty("kind", {}).toString()});
            m.tracks.push_back(std::move(track));
        }
    }

    if (const auto master = s.getProperty("master", {}); master.isObject())
    {
        m.master.raw = master;
        m.master.id = str(master, "id");
        m.master.name = str(master, "name");
    }

    if (const auto* items = s.getProperty("library", {}).getArray())
    {
        for (const auto& i : *items)
        {
            LibraryItemModel item;
            item.raw = i;
            item.id = str(i, "id");
            item.filePath = str(i, "filePath");
            item.playbackFilePath = str(i, "playbackFilePath");
            item.kind = str(i, "kind").isNotEmpty() ? str(i, "kind") : juce::String("source");
            item.name = str(i, "name");
            item.fileName = str(i, "fileName");
            item.durationMs = num(i, "durationMs");
            item.sampleRate = num(i, "sampleRate");
            item.channelCount = intOr(i, "channelCount", 0);
            item.key = str(i, "key");
            item.bpm = num(i, "bpm");
            item.beatAnchorSec = num(i, "beatAnchorSec");
            if (const auto* beats = i.getProperty("beats", {}).getArray())
                for (const auto& b : *beats) item.beats.push_back(static_cast<double>(b));
            item.sourceItemId = str(i, "sourceItemId");
            item.mediaId = str(i, "mediaId");
            item.unresolved = flag(i, "unresolved");
            m.library.push_back(std::move(item));
        }
    }

    if (const auto* markers = s.getProperty("markers", {}).getArray())
        for (const auto& mk : *markers) m.markers.push_back({str(mk, "id"), num(mk, "positionMs")});
    if (const auto* tempo = s.getProperty("tempoMap", {}).getArray())
        for (const auto& p : *tempo) m.tempoMap.push_back({num(p, "timeMs"), num(p, "bpm", 120.0)});

    return m;
}

const TrackModel* ProjectModel::findTrack(const juce::String& id) const
{
    for (const auto& t : tracks)
        if (t.id == id) return &t;
    if (id.isNotEmpty() && id == master.id) return &master;
    return nullptr;
}

const ClipModel* ProjectModel::findClip(const juce::String& id) const
{
    for (const auto& t : tracks)
        for (const auto& c : t.clips)
            if (c.id == id) return &c;
    return nullptr;
}

const LibraryItemModel* ProjectModel::findLibraryItem(const juce::String& id) const
{
    for (const auto& i : library)
        if (i.id == id) return &i;
    return nullptr;
}

double ProjectModel::contentEndMs() const
{
    double end = 0.0;
    for (const auto& t : tracks)
        for (const auto& c : t.clips) end = juce::jmax(end, c.endMs());
    return end;
}

ProjectMirror::ProjectMirror(EngineClient& engineClient) : engine(engineClient)
{
    tree = engine.host().project().getTree();
    tree.addListener(this);
    // Project load/new replace the tree's contents and also broadcast PROJECT_STATE.
    subscription = engine.subscribe("PROJECT_STATE", [this](const juce::var&) { triggerAsyncUpdate(); });
    refreshNow();
}

ProjectMirror::~ProjectMirror()
{
    engine.unsubscribe(subscription);
    tree.removeListener(this);
    cancelPendingUpdate();
}

void ProjectMirror::refreshNow()
{
    cancelPendingUpdate();
    current = ProjectModel::fromState(
        buildProjectStateEnvelope(engine.host().session(), engine.host().project(), false));
    sendSynchronousChangeMessage();
}

void ProjectMirror::handleAsyncUpdate()
{
    refreshNow();
}

} // namespace stemcel::native
