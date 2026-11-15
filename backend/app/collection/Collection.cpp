#include "Collection.h"

#include "AppPaths.h"
#include "AudioImport.h"
#include "BpmDetector.h"
#include "KeyDetection.h"
#include "Log.h"
#include "LoudnessAnalyzer.h"
#include "Rekordbox.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace stemcel::native::dj
{
namespace
{
juce::File& testFolder()
{
    static juce::File folder;
    return folder;
}

juce::String today()
{
    return juce::Time::getCurrentTime().formatted("%Y-%m-%d");
}

juce::var cueJson(const Cue& c)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("slot", c.slot);
    o->setProperty("pos", c.pos);
    if (c.isLoop()) o->setProperty("end", c.end);
    if (c.name.isNotEmpty()) o->setProperty("name", c.name);
    if (c.colour != 0) o->setProperty("colour", c.colour);
    return juce::var(o);
}

Cue cueFrom(const juce::var& v)
{
    Cue c;
    c.slot = v.getProperty("slot", -1);
    c.pos = v.getProperty("pos", 0.0);
    c.end = v.getProperty("end", -1.0);
    c.name = v.getProperty("name", "").toString();
    c.colour = v.getProperty("colour", 0);
    return c;
}

juce::var trackJson(const Track& t)
{
    auto* o = new juce::DynamicObject();
    const juce::var held(o);
    o->setProperty("id", t.id);
    o->setProperty("path", t.path);
    const auto put = [o](const char* key, const juce::String& value)
    {
        if (value.isNotEmpty()) o->setProperty(key, value);
    };
    put("title", t.title);
    put("artist", t.artist);
    put("album", t.album);
    put("genre", t.genre);
    put("label", t.label);
    put("remixer", t.remixer);
    put("comment", t.comment);
    put("key", t.key);
    put("dateAdded", t.dateAdded);
    put("rekordboxId", t.rekordboxId);
    put("artwork", t.artwork);
    if (t.year > 0) o->setProperty("year", t.year);
    o->setProperty("durationSec", t.durationSec);
    if (t.sampleRate > 0) o->setProperty("sampleRate", t.sampleRate);
    if (t.bitrate > 0) o->setProperty("bitrate", t.bitrate);
    if (t.fileSize > 0) o->setProperty("fileSize", t.fileSize);
    if (t.bpm > 0.0)
    {
        o->setProperty("bpm", t.bpm);
        o->setProperty("anchorSec", t.anchorSec);
    }
    if (!t.beats.empty())
    {
        juce::Array<juce::var> beats;
        beats.ensureStorageAllocated(static_cast<int>(t.beats.size()));
        for (const double b : t.beats) beats.add(std::round(b * 100000.0) / 100000.0);
        o->setProperty("beats", beats);
    }
    if (t.firstBeat > 0) o->setProperty("firstBeat", t.firstBeat);
    if (t.gridLocked) o->setProperty("gridLocked", true);
    if (t.rating > 0) o->setProperty("rating", t.rating);
    if (t.colour > 0) o->setProperty("colour", t.colour);
    if (!t.tags.isEmpty())
    {
        juce::Array<juce::var> tags;
        for (const auto& tag : t.tags) tags.add(tag);
        o->setProperty("tags", tags);
    }
    if (t.playCount > 0) o->setProperty("playCount", t.playCount);
    if (!t.cues.empty())
    {
        juce::Array<juce::var> cues;
        for (const auto& c : t.cues) cues.add(cueJson(c));
        o->setProperty("cues", cues);
    }
    if (t.hasLoudness) o->setProperty("loudnessLufs", t.loudnessLufs);
    o->setProperty("state", t.state == Track::State::ready    ? "ready"
                            : t.state == Track::State::failed ? "failed"
                                                              : "pending");
    put("error", t.error);
    return held;
}

Track trackFrom(const juce::var& v)
{
    Track t;
    t.id = static_cast<juce::int64>(v.getProperty("id", 0));
    t.path = v.getProperty("path", "").toString();
    const auto str = [&v](const char* key) { return v.getProperty(key, "").toString(); };
    t.title = str("title");
    t.artist = str("artist");
    t.album = str("album");
    t.genre = str("genre");
    t.label = str("label");
    t.remixer = str("remixer");
    t.comment = str("comment");
    t.key = str("key");
    t.dateAdded = str("dateAdded");
    t.rekordboxId = str("rekordboxId");
    t.artwork = str("artwork");
    t.year = v.getProperty("year", 0);
    t.durationSec = v.getProperty("durationSec", 0.0);
    t.sampleRate = v.getProperty("sampleRate", 0);
    t.bitrate = v.getProperty("bitrate", 0);
    t.fileSize = static_cast<juce::int64>(v.getProperty("fileSize", 0));
    t.bpm = v.getProperty("bpm", 0.0);
    t.anchorSec = v.getProperty("anchorSec", 0.0);
    if (const auto* beats = v.getProperty("beats", {}).getArray())
        for (const auto& b : *beats) t.beats.push_back(static_cast<double>(b));
    t.firstBeat = v.getProperty("firstBeat", 0);
    t.gridLocked = v.getProperty("gridLocked", false);
    t.rating = v.getProperty("rating", 0);
    t.colour = v.getProperty("colour", 0);
    if (const auto* tags = v.getProperty("tags", {}).getArray())
        for (const auto& tag : *tags) t.tags.add(tag.toString());
    t.playCount = v.getProperty("playCount", 0);
    if (const auto* cues = v.getProperty("cues", {}).getArray())
        for (const auto& c : *cues) t.cues.push_back(cueFrom(c));
    t.hasLoudness = v.hasProperty("loudnessLufs");
    t.loudnessLufs = v.getProperty("loudnessLufs", 0.0);
    const auto state = str("state");
    t.state = state == "ready" ? Track::State::ready : state == "failed" ? Track::State::failed : Track::State::pending;
    t.error = str("error");
    return t;
}

const char* kindName(Playlist::Kind kind)
{
    return kind == Playlist::Kind::folder ? "folder" : kind == Playlist::Kind::history ? "history" : "playlist";
}

// What one background analysis found.
struct Analysed
{
    juce::int64 id = 0;
    juce::String path;
    bool force = false;
    juce::var tags;
    double durationSec = 0.0;
    int sampleRate = 0;
    juce::int64 fileSize = 0;
    double bpm = 0.0;
    double anchorSec = 0.0;
    juce::String key;
    bool hasLoudness = false;
    double lufs = 0.0;
    juce::String artwork;
    juce::String error;
};

Analysed analyseFile(juce::int64 id, const juce::File& file, bool force, const juce::File& artworkDir)
{
    Analysed a;
    a.id = id;
    a.path = file.getFullPathName();
    a.force = force;
    if (!file.existsAsFile())
    {
        a.error = "The file is missing";
        return a;
    }
    auto& rekordbox = sharedRekordbox();
    a.fileSize = file.getSize();
    a.tags = rekordbox.readTags(file);
    const juce::File playable(AudioImporter::playablePathFor(file, rekordbox));
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(playable.existsAsFile() ? formats.createReaderFor(playable) : nullptr);
    if (reader == nullptr || reader->sampleRate <= 0.0)
    {
        a.error = "Stemcel can't decode this file";
        return a;
    }
    a.sampleRate = static_cast<int>(reader->sampleRate);
    a.durationSec = static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
    a.key = detectMusicalKey(*reader);
    const auto grid = BpmDetector::analyse(playable, formats);
    if (grid.bpm > 0.0)
    {
        a.bpm = grid.bpm;
        a.anchorSec = grid.beatAnchorSec;
        // The anchor as the first beat in the track.
        const double period = 60.0 / grid.bpm;
        a.anchorSec -= std::floor(a.anchorSec / period) * period;
    }
    if (std::abs(reader->sampleRate - 44100.0) < 1.0 || std::abs(reader->sampleRate - 48000.0) < 1.0)
    {
        LoudnessAnalyzer loudness(reader->sampleRate);
        const int channels = static_cast<int>(std::min(2u, reader->numChannels));
        juce::AudioBuffer<float> block(std::max(1, channels), 1 << 16);
        for (juce::int64 at = 0; at < reader->lengthInSamples; at += block.getNumSamples())
        {
            const int n = static_cast<int>(std::min<juce::int64>(block.getNumSamples(), reader->lengthInSamples - at));
            reader->read(&block, 0, n, at, true, channels > 1);
            loudness.process(block.getArrayOfReadPointers(), channels, n);
        }
        const auto result = loudness.finalize();
        if (!result.silent && std::isfinite(result.integratedLufs))
        {
            a.hasLoudness = true;
            a.lufs = result.integratedLufs;
        }
    }
    if (static_cast<bool>(a.tags.getProperty("hasPicture", false)))
    {
        artworkDir.createDirectory();
        const auto cover = artworkDir.getChildFile(juce::String(id) + ".img");
        if (rekordbox.extractPicture(file, cover)) a.artwork = cover.getFullPathName();
    }
    return a;
}
} // namespace

juce::String normalisedPath(const juce::String& path)
{
    auto p = path.replaceCharacter('\\', '/');
#if JUCE_WINDOWS || JUCE_MAC
    p = p.toLowerCase();
#endif
    return p;
}

juce::var toPerfCues(const Track& track)
{
    juce::Array<juce::var> hot;
    for (int slot = 0; slot < 8; ++slot)
    {
        const auto* c = track.hotCue(slot);
        if (c == nullptr)
        {
            hot.add(juce::var());
            continue;
        }
        auto* o = new juce::DynamicObject();
        o->setProperty("pos", c->pos);
        if (c->isLoop()) o->setProperty("end", c->end);
        hot.add(juce::var(o));
    }
    juce::Array<juce::var> memory;
    for (const auto& c : track.cues)
    {
        if (c.isHot()) continue;
        auto* o = new juce::DynamicObject();
        o->setProperty("pos", c.pos);
        if (c.isLoop()) o->setProperty("end", c.end);
        memory.add(juce::var(o));
    }
    auto* cues = new juce::DynamicObject();
    cues->setProperty("hot", hot);
    cues->setProperty("memory", memory);
    return juce::var(cues);
}

std::vector<Cue> cuesFromPerf(const juce::var& perfCues, const std::vector<Cue>& previous)
{
    std::vector<Cue> out;
    const auto keep = [&previous](Cue& c)
    {
        for (const auto& p : previous)
            if (p.slot == c.slot && std::abs(p.pos - c.pos) < 0.0005)
            {
                c.name = p.name;
                c.colour = p.colour;
            }
    };
    if (const auto* hot = perfCues.getProperty("hot", {}).getArray())
        for (int slot = 0; slot < std::min(8, hot->size()); ++slot)
        {
            const auto& v = (*hot)[slot];
            if (!v.isObject()) continue;
            Cue c;
            c.slot = slot;
            c.pos = v.getProperty("pos", 0.0);
            c.end = v.getProperty("end", -1.0);
            keep(c);
            out.push_back(c);
        }
    if (const auto* memory = perfCues.getProperty("memory", {}).getArray())
        for (const auto& v : *memory)
        {
            Cue c;
            c.pos = v.getProperty("pos", 0.0);
            c.end = v.getProperty("end", -1.0);
            keep(c);
            out.push_back(c);
        }
    return out;
}

// ---------------------------------------------------------------------------------------
// Track

juce::String Track::displayTitle() const
{
    if (title.isNotEmpty()) return title;
    return juce::File(path).getFileNameWithoutExtension();
}

std::vector<double> Track::beatTimes() const
{
    if (!beats.empty()) return beats;
    std::vector<double> out;
    if (bpm <= 0.0 || durationSec <= 0.0) return out;
    const double period = 60.0 / bpm;
    double t = anchorSec - std::floor(anchorSec / period) * period;
    out.reserve(static_cast<size_t>(durationSec / period) + 2);
    for (; t < durationSec; t += period) out.push_back(t);
    return out;
}

int Track::beatNumberAt(int index) const
{
    if (firstBeat <= 0) return 0;
    // The anchor's own index among the beats (a steady grid starts before it).
    int anchorIndex = 0;
    if (beats.empty() && bpm > 0.0)
    {
        const double period = 60.0 / bpm;
        anchorIndex = static_cast<int>(std::floor(anchorSec / period + 1e-6));
    }
    const int offset = ((index - anchorIndex) % 4 + 4) % 4;
    return (firstBeat - 1 + offset) % 4 + 1;
}

const Cue* Track::hotCue(int slot) const
{
    for (const auto& c : cues)
        if (c.slot == slot) return &c;
    return nullptr;
}

// ---------------------------------------------------------------------------------------
// Collection

Collection& Collection::shared()
{
    static Collection collection;
    return collection;
}

Collection::Collection() = default;

Collection::~Collection()
{
    alive->store(false);
    pool.removeAllJobs(true, 30000);
    if (dirty) saveNow();
}

juce::File Collection::folder()
{
    if (testFolder() != juce::File()) return testFolder();
    return juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(appFolderName()).getChildFile("Collection");
}

void Collection::setFolderForTest(const juce::File& f)
{
    testFolder() = f;
}

void Collection::load()
{
    if (loaded) return;
    loaded = true;
    read(folder().getChildFile("collection.json"));
}

void Collection::reload()
{
    loaded = false;
    trackList.clear();
    playlistList.clear();
    load();
    sendChangeMessage();
}

void Collection::read(const juce::File& file)
{
    trackList.clear();
    playlistList.clear();
    lastId = 0;
    if (file.existsAsFile())
    {
        const auto json = juce::JSON::parse(file);
        lastId = static_cast<juce::int64>(json.getProperty("lastId", 0));
        if (const auto* tracks = json.getProperty("tracks", {}).getArray())
            for (const auto& t : *tracks) trackList.push_back(trackFrom(t));
        if (const auto* lists = json.getProperty("playlists", {}).getArray())
            for (const auto& l : *lists)
            {
                Playlist p;
                p.id = static_cast<juce::int64>(l.getProperty("id", 0));
                p.parent = static_cast<juce::int64>(l.getProperty("parent", 0));
                p.name = l.getProperty("name", "").toString();
                const auto kind = l.getProperty("kind", "playlist").toString();
                p.kind = kind == "folder" ? Playlist::Kind::folder : kind == "history" ? Playlist::Kind::history : Playlist::Kind::playlist;
                p.date = l.getProperty("date", "").toString();
                p.source = l.getProperty("source", "").toString();
                if (const auto* ids = l.getProperty("tracks", {}).getArray())
                    for (const auto& id : *ids) p.tracks.push_back(static_cast<juce::int64>(id));
                playlistList.push_back(std::move(p));
            }
        log::info("collection", juce::String(static_cast<int>(trackList.size())) + " tracks, "
                                    + juce::String(static_cast<int>(playlistList.size())) + " playlists from " + file.getFullPathName());
    }
    for (const auto& t : trackList) lastId = std::max(lastId, t.id);
    for (const auto& p : playlistList) lastId = std::max(lastId, p.id);
    reindex();
    // Anything not analysed yet (the app quit while it waited) goes back in the queue.
    for (const auto& t : trackList)
        if (t.state == Track::State::pending || t.state == Track::State::analysing) queueAnalysis(t.id, false);
}

juce::var Collection::toJson() const
{
    auto* root = new juce::DynamicObject();
    const juce::var held(root);
    root->setProperty("version", 1);
    root->setProperty("lastId", lastId);
    juce::Array<juce::var> tracks;
    tracks.ensureStorageAllocated(static_cast<int>(trackList.size()));
    for (const auto& t : trackList) tracks.add(trackJson(t));
    root->setProperty("tracks", tracks);
    juce::Array<juce::var> lists;
    for (const auto& p : playlistList)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("id", p.id);
        if (p.parent != 0) o->setProperty("parent", p.parent);
        o->setProperty("name", p.name);
        o->setProperty("kind", kindName(p.kind));
        if (p.date.isNotEmpty()) o->setProperty("date", p.date);
        if (p.source.isNotEmpty()) o->setProperty("source", p.source);
        juce::Array<juce::var> ids;
        for (const auto id : p.tracks) ids.add(id);
        o->setProperty("tracks", ids);
        lists.add(juce::var(o));
    }
    root->setProperty("playlists", lists);
    return held;
}

void Collection::saveNow()
{
    stopTimer();
    dirty = false;
    const auto dir = folder();
    dir.createDirectory();
    const auto file = dir.getChildFile("collection.json");
    // Written aside and swapped in, so a crash mid-write never leaves half a collection.
    const auto temp = dir.getChildFile("collection.json.tmp");
    if (!temp.replaceWithText(juce::JSON::toString(toJson(), true)))
    {
        log::warn("collection", "couldn't write " + temp.getFullPathName());
        return;
    }
    if (file.existsAsFile()) file.copyFileTo(dir.getChildFile("collection.previous.json"));
    if (!temp.moveFileTo(file)) log::warn("collection", "couldn't replace " + file.getFullPathName());
}

void Collection::timerCallback()
{
    saveNow();
}

void Collection::changed(bool save)
{
    if (save)
    {
        dirty = true;
        startTimer(1000);
    }
    sendChangeMessage();
}

void Collection::reindex()
{
    byId.clear();
    byPath.clear();
    for (size_t i = 0; i < trackList.size(); ++i)
    {
        byId[trackList[i].id] = i;
        byPath[normalisedPath(trackList[i].path)] = i;
    }
}

const Track* Collection::find(juce::int64 id) const
{
    const auto it = byId.find(id);
    return it == byId.end() ? nullptr : &trackList[it->second];
}

Track* Collection::findMutable(juce::int64 id)
{
    const auto it = byId.find(id);
    return it == byId.end() ? nullptr : &trackList[it->second];
}

const Track* Collection::findByPath(const juce::String& path) const
{
    const auto it = byPath.find(normalisedPath(path));
    return it == byPath.end() ? nullptr : &trackList[it->second];
}

const Playlist* Collection::findPlaylist(juce::int64 id) const
{
    for (const auto& p : playlistList)
        if (p.id == id) return &p;
    return nullptr;
}

Playlist* Collection::findPlaylistMutable(juce::int64 id)
{
    for (auto& p : playlistList)
        if (p.id == id) return &p;
    return nullptr;
}

std::vector<const Playlist*> Collection::childrenOf(juce::int64 parent, bool histories) const
{
    std::vector<const Playlist*> out;
    for (const auto& p : playlistList)
        if (p.parent == parent && (p.kind == Playlist::Kind::history) == histories) out.push_back(&p);
    return out;
}

std::vector<juce::int64> Collection::tracksIn(juce::int64 id) const
{
    std::vector<juce::int64> out;
    std::set<juce::int64> seen;
    std::function<void(juce::int64)> walk = [&](juce::int64 at)
    {
        const auto* p = findPlaylist(at);
        if (p == nullptr) return;
        if (!p->isFolder())
        {
            for (const auto t : p->tracks)
                if (seen.insert(t).second) out.push_back(t);
            return;
        }
        for (const auto& child : playlistList)
            if (child.parent == at) walk(child.id);
    };
    walk(id);
    return out;
}

std::vector<juce::int64> Collection::addFiles(const juce::Array<juce::File>& files, juce::int64 playlist)
{
    load();
    std::vector<juce::int64> ids;
    for (const auto& file : files)
    {
        if (!file.existsAsFile() || !AudioImporter::isAudioFile(file)) continue;
        if (const auto* existing = findByPath(file.getFullPathName()))
        {
            ids.push_back(existing->id);
            continue;
        }
        Track t;
        t.id = nextId();
        t.path = file.getFullPathName();
        t.title = file.getFileNameWithoutExtension();
        t.dateAdded = today();
        t.fileSize = file.getSize();
        trackList.push_back(t);
        byId[t.id] = trackList.size() - 1;
        byPath[normalisedPath(t.path)] = trackList.size() - 1;
        ids.push_back(t.id);
        queueAnalysis(t.id, false);
    }
    if (playlist != 0 && !ids.empty()) addToPlaylist(playlist, ids);
    changed();
    return ids;
}

void Collection::addFolder(const juce::File& dir, juce::int64 playlist, std::function<void(int)> done)
{
    pool.addJob(
        [this, dir, playlist, done, alive = alive]
        {
            auto found = std::make_shared<juce::Array<juce::File>>();
            for (const auto& entry : juce::RangedDirectoryIterator(dir, true, "*", juce::File::findFiles))
                if (AudioImporter::isAudioFile(entry.getFile())) found->add(entry.getFile());
            found->sort();
            juce::MessageManager::callAsync(
                [this, found, playlist, done, alive]
                {
                    if (!alive->load()) return;
                    const auto before = trackList.size();
                    addFiles(*found, playlist);
                    if (done) done(static_cast<int>(trackList.size() - before));
                });
        });
}

void Collection::removeTracks(const std::vector<juce::int64>& ids)
{
    const std::set<juce::int64> gone(ids.begin(), ids.end());
    trackList.erase(std::remove_if(trackList.begin(), trackList.end(), [&](const Track& t) { return gone.count(t.id) > 0; }),
                    trackList.end());
    for (auto& p : playlistList)
        p.tracks.erase(std::remove_if(p.tracks.begin(), p.tracks.end(), [&](juce::int64 t) { return gone.count(t) > 0; }), p.tracks.end());
    reindex();
    changed();
}

void Collection::update(juce::int64 id, const std::function<void(Track&)>& change)
{
    auto* t = findMutable(id);
    if (t == nullptr) return;
    const auto oldPath = normalisedPath(t->path);
    change(*t);
    if (normalisedPath(t->path) != oldPath) reindex();
    changed();
}

void Collection::relocate(juce::int64 id, const juce::File& file)
{
    update(id,
           [&file](Track& t)
           {
               t.path = file.getFullPathName();
               t.fileSize = file.getSize();
               if (t.state == Track::State::failed) t.state = Track::State::pending;
               t.error = {};
           });
    if (const auto* t = find(id); t != nullptr && t->state == Track::State::pending) queueAnalysis(id, false);
}

void Collection::reanalyse(const std::vector<juce::int64>& ids)
{
    for (const auto id : ids)
        if (auto* t = findMutable(id))
        {
            t->state = Track::State::pending;
            t->error = {};
            queueAnalysis(id, true);
        }
    changed();
}

void Collection::setPaused(bool pause)
{
    paused = pause;
    if (paused) return;
    auto held = std::exchange(waiting, {});
    for (const auto& [id, force] : held) queueAnalysis(id, force);
}

void Collection::setLoudness(const juce::String& path, double lufs)
{
    const auto it = byPath.find(normalisedPath(path));
    if (it == byPath.end() || !std::isfinite(lufs)) return;
    auto& t = trackList[it->second];
    if (t.hasLoudness && std::abs(t.loudnessLufs - lufs) < 0.05) return;
    t.hasLoudness = true;
    t.loudnessLufs = lufs;
    changed();
}

void Collection::queueAnalysis(juce::int64 id, bool force)
{
    const auto* t = find(id);
    if (t == nullptr) return;
    if (paused)
    {
        waiting.emplace_back(id, force);
        return;
    }
    ++queued;
    const juce::File file(t->path);
    const auto artworkDir = folder().getChildFile("artwork");
    pool.addJob(
        [this, id, file, force, artworkDir, alive = alive]
        {
            if (!alive->load()) return;
            auto result = std::make_shared<Analysed>(analyseFile(id, file, force, artworkDir));
            juce::MessageManager::callAsync(
                [this, result, alive]
                {
                    if (!alive->load()) return;
                    --queued;
                    auto* t = findMutable(result->id);
                    if (t == nullptr || normalisedPath(t->path) != normalisedPath(result->path))
                    {
                        changed(false);
                        return;
                    }
                    if (result->error.isNotEmpty())
                    {
                        t->state = Track::State::failed;
                        t->error = result->error;
                        changed();
                        return;
                    }
                    const auto& tags = result->tags;
                    const auto fill = [](juce::String& field, const juce::var& value)
                    {
                        if (field.isEmpty() || field == "?") field = value.toString();
                    };
                    const auto tag = [&tags](const char* key) { return tags.getProperty(key, "").toString().trim(); };
                    if (const auto title = tag("title"); title.isNotEmpty() && (t->title.isEmpty() || t->title == juce::File(t->path).getFileNameWithoutExtension()))
                        t->title = title;
                    fill(t->artist, tag("artist"));
                    fill(t->album, tag("album"));
                    fill(t->genre, tag("genre"));
                    fill(t->label, tag("label"));
                    fill(t->comment, tag("comment"));
                    if (t->year <= 0) t->year = tags.getProperty("year", 0);
                    if (t->bitrate <= 0) t->bitrate = tags.getProperty("bitrate", 0);
                    t->sampleRate = static_cast<int>(tags.getProperty("sampleRate", 0)) > 0 ? static_cast<int>(tags.getProperty("sampleRate", 0))
                                                                                          : result->sampleRate;
                    t->durationSec = result->durationSec;
                    t->fileSize = result->fileSize;
                    if (result->force || !t->gridLocked)
                    {
                        if (result->bpm > 0.0)
                        {
                            t->bpm = result->bpm;
                            t->anchorSec = result->anchorSec;
                            t->beats.clear();
                            t->firstBeat = 0;
                            t->gridLocked = false;
                        }
                    }
                    if (result->force || t->key.isEmpty())
                        if (result->key.isNotEmpty()) t->key = result->key;
                    if (t->key.isEmpty()) t->key = normaliseKeyName(tag("key"));
                    if (result->hasLoudness)
                    {
                        t->hasLoudness = true;
                        t->loudnessLufs = result->lufs;
                    }
                    if (result->artwork.isNotEmpty()) t->artwork = result->artwork;
                    t->state = Track::State::ready;
                    t->error = {};
                    changed();
                });
        });
}

juce::int64 Collection::createPlaylist(const juce::String& name, juce::int64 parent, Playlist::Kind kind)
{
    load();
    Playlist p;
    p.id = nextId();
    p.parent = parent;
    p.name = name;
    p.kind = kind;
    playlistList.push_back(p);
    changed();
    return p.id;
}

void Collection::renamePlaylist(juce::int64 id, const juce::String& name)
{
    if (auto* p = findPlaylistMutable(id))
    {
        p->name = name;
        changed();
    }
}

void Collection::deletePlaylist(juce::int64 id)
{
    std::set<juce::int64> gone{id};
    // A folder's children, and theirs.
    for (bool more = true; more;)
    {
        more = false;
        for (const auto& p : playlistList)
            if (gone.count(p.parent) > 0 && gone.insert(p.id).second) more = true;
    }
    playlistList.erase(std::remove_if(playlistList.begin(), playlistList.end(), [&](const Playlist& p) { return gone.count(p.id) > 0; }),
                       playlistList.end());
    changed();
}

void Collection::movePlaylist(juce::int64 id, juce::int64 newParent)
{
    // Never into itself or one of its own folders.
    for (juce::int64 at = newParent; at != 0;)
    {
        if (at == id) return;
        const auto* p = findPlaylist(at);
        at = p != nullptr ? p->parent : 0;
    }
    if (auto* p = findPlaylistMutable(id))
    {
        p->parent = newParent;
        changed();
    }
}

void Collection::addToPlaylist(juce::int64 playlist, const std::vector<juce::int64>& ids, int insertAt)
{
    auto* p = findPlaylistMutable(playlist);
    if (p == nullptr || p->isFolder()) return;
    std::vector<juce::int64> adding;
    for (const auto id : ids)
        if (find(id) != nullptr) adding.push_back(id);
    const auto at = insertAt < 0 || insertAt > static_cast<int>(p->tracks.size()) ? p->tracks.end() : p->tracks.begin() + insertAt;
    p->tracks.insert(at, adding.begin(), adding.end());
    changed();
}

void Collection::removeFromPlaylist(juce::int64 playlist, const std::vector<int>& positions)
{
    auto* p = findPlaylistMutable(playlist);
    if (p == nullptr) return;
    auto sorted = positions;
    std::sort(sorted.rbegin(), sorted.rend());
    for (const int i : sorted)
        if (i >= 0 && i < static_cast<int>(p->tracks.size())) p->tracks.erase(p->tracks.begin() + i);
    changed();
}

void Collection::reorderPlaylist(juce::int64 playlist, const std::vector<int>& positions, int insertAt)
{
    auto* p = findPlaylistMutable(playlist);
    if (p == nullptr || positions.empty()) return;
    std::set<int> moving(positions.begin(), positions.end());
    std::vector<juce::int64> moved, kept;
    int keptBefore = 0;
    for (int i = 0; i < static_cast<int>(p->tracks.size()); ++i)
    {
        if (moving.count(i) > 0)
            moved.push_back(p->tracks[static_cast<size_t>(i)]);
        else
        {
            if (i < insertAt) ++keptBefore;
            kept.push_back(p->tracks[static_cast<size_t>(i)]);
        }
    }
    kept.insert(kept.begin() + keptBefore, moved.begin(), moved.end());
    p->tracks = kept;
    changed();
}

juce::int64 Collection::addHistory(const juce::String& name, const juce::String& date, const juce::String& source,
                                   const std::vector<juce::int64>& ids)
{
    // One history per stick and day: read again, it is replaced, not doubled.
    for (const auto& p : playlistList)
        if (p.kind == Playlist::Kind::history && p.name == name && p.source == source)
        {
            auto* mutableList = findPlaylistMutable(p.id);
            mutableList->tracks = ids;
            mutableList->date = date;
            changed();
            return p.id;
        }
    const auto id = createPlaylist(name, 0, Playlist::Kind::history);
    auto* p = findPlaylistMutable(id);
    p->date = date;
    p->source = source;
    p->tracks = ids;
    changed();
    return id;
}

// ---------------------------------------------------------------------------------------
// From rekordbox

void Collection::importRekordbox(Rekordbox& rekordbox, std::function<void(int, int)> progress,
                                 std::function<void(const juce::String&, bool)> done)
{
    importRekordbox(rekordbox, RekordboxSource{}, {}, std::move(progress), std::move(done));
}

void Collection::importRekordbox(Rekordbox& rekordbox, const RekordboxSource& source, const juce::String& passphrase,
                                 std::function<void(int, int)> progress, std::function<void(const juce::String&, bool)> done)
{
    load();
    struct Read
    {
        bool ok = false;
        juce::String error;
        std::vector<Track> tracks;
        std::vector<RekordboxPlaylist> playlists;
    };
    pool.addJob(
        [this, &rekordbox, source, passphrase, progress, done, alive = alive]
        {
            auto read = std::make_shared<Read>();
            const auto previous = rekordbox.currentSource();
            const auto snapshot = passphrase.isNotEmpty() ? rekordbox.open(source.masterDb, passphrase) : rekordbox.open(source);
            if (!snapshot.ok)
            {
                read->error = snapshot.error;
            }
            else
            {
                read->ok = true;
                read->playlists = snapshot.playlists;
                const int total = static_cast<int>(snapshot.tracks.size());
                int count = 0;
                for (const auto& r : snapshot.tracks)
                {
                    if (!alive->load()) return;
                    Track t;
                    t.rekordboxId = r.id;
                    t.path = juce::File(r.path).getFullPathName();
                    t.title = r.title;
                    t.artist = r.artist;
                    t.album = r.album;
                    t.genre = r.genre;
                    t.label = r.label;
                    t.comment = r.comment;
                    t.dateAdded = r.dateAdded;
                    t.year = r.year;
                    t.durationSec = r.durationMs / 1000.0;
                    t.sampleRate = r.sampleRate;
                    t.bitrate = r.bitrate;
                    t.rating = juce::jlimit(0, 5, r.rating);
                    t.colour = juce::jlimit(0, 8, r.colour);
                    t.playCount = r.playCount;
                    t.key = normaliseKeyName(r.key);
                    t.state = Track::State::ready;
                    const auto detail = rekordbox.trackDetail(r.id);
                    if (detail.ok && detail.beats.size() >= 2)
                    {
                        // A steady grid as its tempo and first beat; a moving one beat by beat.
                        bool steady = true;
                        for (const auto& b : detail.beats) steady = steady && std::abs(b.bpm - detail.beats.front().bpm) < 0.005;
                        t.firstBeat = juce::jlimit(1, 4, detail.beats.front().beatNumber);
                        if (steady && detail.beats.front().bpm > 0.0)
                        {
                            t.bpm = detail.beats.front().bpm;
                            t.anchorSec = detail.beats.front().timeMs / 1000.0;
                        }
                        else
                        {
                            for (const auto& b : detail.beats) t.beats.push_back(b.timeMs / 1000.0);
                            t.bpm = r.bpm;
                            t.anchorSec = t.beats.front();
                        }
                        t.gridLocked = true;
                    }
                    else if (r.bpm <= 0.0)
                    {
                        t.state = Track::State::pending; // rekordbox hasn't analysed it: Stemcel will
                    }
                    if (detail.ok)
                        for (const auto& c : detail.cues)
                        {
                            Cue cue;
                            cue.pos = c.positionMs / 1000.0;
                            cue.end = c.endMs > c.positionMs ? c.endMs / 1000.0 : -1.0;
                            cue.colour = c.colour;
                            if (c.memory)
                                cue.slot = -1;
                            else if (c.hot.length() == 1 && c.hot[0] >= 'A' && c.hot[0] <= 'H')
                                cue.slot = c.hot[0] - 'A';
                            else
                                continue; // hot cues past H: the CDJ shows eight
                            t.cues.push_back(cue);
                        }
                    read->tracks.push_back(std::move(t));
                    if (++count % 25 == 0 || count == total)
                        juce::MessageManager::callAsync(
                            [progress, count, total, alive]
                            {
                                if (alive->load() && progress) progress(count, total);
                            });
                }
            }
            // Whatever the rekordbox tab had open, it has again.
            if (previous.key() != source.key()) rekordbox.open(previous);
            juce::MessageManager::callAsync(
                [this, read, done, alive]
                {
                    if (!alive->load()) return;
                    if (!read->ok)
                    {
                        if (done) done("Couldn't read the rekordbox library: " + read->error, false);
                        return;
                    }
                    std::map<juce::String, juce::int64> idOf; // rekordbox id -> collection id
                    int added = 0, merged = 0;
                    for (auto& t : read->tracks)
                    {
                        if (auto* existing = const_cast<Track*>(findByPath(t.path)))
                        {
                            // Already here: rekordbox's grid and cues where it has none.
                            if (!existing->gridLocked && t.gridLocked)
                            {
                                existing->bpm = t.bpm;
                                existing->anchorSec = t.anchorSec;
                                existing->beats = t.beats;
                                existing->firstBeat = t.firstBeat;
                                existing->gridLocked = true;
                            }
                            if (existing->cues.empty()) existing->cues = t.cues;
                            if (existing->key.isEmpty()) existing->key = t.key;
                            if (existing->rating == 0) existing->rating = t.rating;
                            if (existing->colour == 0) existing->colour = t.colour;
                            if (existing->comment.isEmpty()) existing->comment = t.comment;
                            existing->rekordboxId = t.rekordboxId;
                            idOf[t.rekordboxId] = existing->id;
                            ++merged;
                            continue;
                        }
                        t.id = nextId();
                        if (t.dateAdded.isEmpty()) t.dateAdded = today();
                        idOf[t.rekordboxId] = t.id;
                        const bool analyse = t.state == Track::State::pending;
                        trackList.push_back(std::move(t));
                        byId[trackList.back().id] = trackList.size() - 1;
                        byPath[normalisedPath(trackList.back().path)] = trackList.size() - 1;
                        if (analyse) queueAnalysis(trackList.back().id, false);
                        ++added;
                    }
                    // The playlists, in a "rekordbox" folder (replacing an earlier import's).
                    for (const auto& p : playlistList)
                        if (p.parent == 0 && p.isFolder() && p.name == "rekordbox")
                        {
                            deletePlaylist(p.id);
                            break;
                        }
                    const auto top = createPlaylist("rekordbox", 0, Playlist::Kind::folder);
                    std::map<juce::String, juce::int64> listOf;
                    auto sorted = read->playlists;
                    std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.seq < b.seq; });
                    int lists = 0;
                    // Parents before children: as many passes as the deepest folder.
                    for (bool more = true; more;)
                    {
                        more = false;
                        for (const auto& r : sorted)
                        {
                            if (listOf.count(r.id) > 0) continue;
                            const bool atTop = r.parentId.isEmpty() || r.parentId == "root" || r.parentId == "0";
                            if (!atTop && listOf.count(r.parentId) == 0) continue;
                            const auto parent = atTop ? top : listOf[r.parentId];
                            const auto id = createPlaylist(r.name, parent, r.isFolder() ? Playlist::Kind::folder : Playlist::Kind::playlist);
                            listOf[r.id] = id;
                            if (!r.isFolder())
                            {
                                auto* p = findPlaylistMutable(id);
                                for (const auto& t : r.trackIds)
                                    if (const auto it = idOf.find(t); it != idOf.end()) p->tracks.push_back(it->second);
                                ++lists;
                            }
                            more = true;
                        }
                    }
                    reindex();
                    changed();
                    if (done)
                        done("Brought in " + juce::String(added) + " track(s) and " + juce::String(lists) + " playlist(s) from rekordbox"
                                 + (merged > 0 ? "; " + juce::String(merged) + " were already in the collection" : juce::String()),
                             true);
                });
        });
}

// ---------------------------------------------------------------------------------------
// To a USB stick

juce::var Collection::linkRequest(const juce::String& interfaceName) const
{
    std::vector<juce::int64> lists;
    for (const auto* p : childrenOf(0, false)) lists.push_back(p->id);
    auto request = exportRequest(lists, {}, false);
    auto* o = request.getDynamicObject();
    o->removeProperty("root");
    o->removeProperty("sync");
    o->setProperty("action", "link");
    o->setProperty("libraryDir", folder().getChildFile("link").getFullPathName());
    o->setProperty("interface", interfaceName);
    // Tracks in no playlist are served too, under the library's All Tracks.
    auto* tracks = o->getProperty("tracks").getArray();
    std::set<juce::int64> listed;
    for (const auto& t : *tracks) listed.insert(static_cast<juce::int64>(t.getProperty("id", 0)));
    for (const auto& t : trackList)
        if (listed.count(t.id) == 0 && juce::File(t.path).existsAsFile()) tracks->add(entryFor(t));
    return request;
}

juce::var Collection::exportRequest(const std::vector<juce::int64>& lists, const juce::File& stick, bool sync) const
{
    auto* request = new juce::DynamicObject();
    const juce::var held(request);
    request->setProperty("action", "export");
    request->setProperty("root", stick.getFullPathName());
    request->setProperty("sync", sync);
    request->setProperty("cacheDir", folder().getChildFile("anlz").getFullPathName());

    // Every playlist under the chosen ones, folders included, parents first.
    std::vector<const Playlist*> chosen;
    std::set<juce::int64> taken;
    std::function<void(const Playlist&)> take = [&](const Playlist& p)
    {
        if (!taken.insert(p.id).second) return;
        chosen.push_back(&p);
        if (p.isFolder())
            for (const auto& child : playlistList)
                if (child.parent == p.id) take(child);
    };
    for (const auto id : lists)
        if (const auto* p = findPlaylist(id)) take(*p);

    std::map<juce::int64, int> indexOf;
    juce::Array<juce::var> trackArray, listArray;
    const auto trackIndex = [&](juce::int64 id) -> int
    {
        if (const auto it = indexOf.find(id); it != indexOf.end()) return it->second;
        const auto* t = find(id);
        if (t == nullptr || !juce::File(t->path).existsAsFile()) return -1;
        const int index = trackArray.size();
        trackArray.add(entryFor(*t));
        indexOf[id] = index;
        return index;
    };
    for (const auto* p : chosen)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("id", p->id);
        o->setProperty("name", p->name);
        o->setProperty("parent", taken.count(p->parent) > 0 ? p->parent : 0);
        o->setProperty("folder", p->isFolder());
        juce::Array<juce::var> indices;
        if (!p->isFolder())
            for (const auto id : p->tracks)
                if (const int i = trackIndex(id); i >= 0) indices.add(i);
        o->setProperty("tracks", indices);
        listArray.add(juce::var(o));
    }
    request->setProperty("tracks", trackArray);
    request->setProperty("playlists", listArray);
    return held;
}

juce::var Collection::entryFor(const Track& track)
{
    const auto* t = &track;
    {
        auto* o = new juce::DynamicObject();
        const juce::var entry(o);
        o->setProperty("id", t->id);
        o->setProperty("path", t->path);
        o->setProperty("title", t->displayTitle());
        o->setProperty("artist", t->artist);
        o->setProperty("album", t->album);
        o->setProperty("genre", t->genre);
        o->setProperty("label", t->label);
        o->setProperty("comment", t->comment);
        o->setProperty("key", t->key);
        o->setProperty("dateAdded", t->dateAdded);
        o->setProperty("year", t->year);
        o->setProperty("rating", t->rating);
        o->setProperty("colour", t->colour);
        o->setProperty("durationSec", t->durationSec);
        o->setProperty("bitrate", t->bitrate);
        o->setProperty("sampleRate", t->sampleRate);
        o->setProperty("bpm", t->bpm);
        o->setProperty("firstBeat", t->firstBeat);
        juce::Array<juce::var> beats;
        for (const double b : t->beatTimes()) beats.add(b);
        o->setProperty("beats", beats);
        juce::Array<juce::var> cues;
        for (const auto& c : t->cues) cues.add(cueJson(c));
        o->setProperty("cues", cues);
        if (t->artwork.isNotEmpty() && juce::File(t->artwork).existsAsFile()) o->setProperty("artwork", t->artwork);
        juce::Array<juce::var> tags;
        for (const auto& tag : t->tags) tags.add(tag);
        o->setProperty("tags", tags);
        return entry;
    }
}

} // namespace stemcel::native::dj
