#include "Rekordbox.h"

#include "AppPaths.h"
#include "Log.h"

#include <algorithm>

namespace stemcel::native
{
namespace
{
#if JUCE_WINDOWS
constexpr const char* kLibraryName = "stemcel_rekordbox.dll";
#elif JUCE_MAC
constexpr const char* kLibraryName = "libstemcel_rekordbox.dylib";
#else
constexpr const char* kLibraryName = "libstemcel_rekordbox.so";
#endif

juce::String str(const juce::var& o, const char* key)
{
    const auto v = o.getProperty(key, {});
    return v.isVoid() ? juce::String() : v.toString();
}

double num(const juce::var& o, const char* key)
{
    const auto v = o.getProperty(key, {});
    return v.isVoid() ? 0.0 : static_cast<double>(v);
}

bool isOk(const juce::var& o)
{
    return static_cast<bool>(o.getProperty("ok", false));
}

juce::var failure(const juce::String& message)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("ok", false);
    o->setProperty("error", message);
    return juce::var(o);
}
} // namespace

juce::String RekordboxSource::key() const
{
    switch (kind)
    {
    case Kind::computer: return "computer";
    case Kind::drive: return "drive:" + masterDb.getFullPathName();
    case Kind::device: return "device:" + root.getFullPathName() + "|" + format;
    }
    return {};
}

juce::String RekordboxSource::label() const
{
    switch (kind)
    {
    case Kind::computer: return "This computer";
    case Kind::drive: return name + " (library on drive)";
    case Kind::device:
        return name + juce::String(juce::CharPointer_UTF8(" \xe2\x80\x93 ")) + (format == "oneLibrary" ? "OneLibrary" : "Device Library");
    }
    return {};
}

struct Rekordbox::Functions
{
    using Open = void* (*)(const char*, const char*, char**);
    using Close = void (*)(void*);
    using FreeString = void (*)(char*);
    using Snapshot = char* (*)(const void*);
    using Track = char* (*)(const void*, const char*);
    using Tags = char* (*)(const char*);
    using Picture = int (*)(const char*, const char*);
    using Decode = int (*)(const char*, const char*);
    using Fixture = char* (*)(const char*, const char*);
    using OpenDevice = void* (*)(const char*, const char*, char**);
    using Devices = char* (*)();
    using Location = char* (*)(const void*);
    using Refusal = char* (*)(const void*);
    using Write = char* (*)(const void*, const char*);

    Open open = nullptr;
    Close close = nullptr;
    FreeString freeString = nullptr;
    Snapshot snapshot = nullptr;
    Track track = nullptr;
    Tags tags = nullptr;
    Picture picture = nullptr;
    Decode decode = nullptr;
    Fixture fixture = nullptr;
    // Newer exports: devices and writes. A reader without them still browses this
    // computer's library.
    OpenDevice openDevice = nullptr;
    Devices devices = nullptr;
    Location location = nullptr;
    Refusal refusal = nullptr;
    Write write = nullptr;
    Fixture deviceFixture = nullptr;
};

const RekordboxTrack* RekordboxSnapshot::findTrack(const juce::String& id) const
{
    const auto it = trackIndex.find(id);
    return it == trackIndex.end() ? nullptr : &tracks[it->second];
}

const RekordboxPlaylist* RekordboxSnapshot::findPlaylist(const juce::String& id) const
{
    for (const auto& p : playlists)
        if (p.id == id) return &p;
    return nullptr;
}

std::vector<const RekordboxPlaylist*> RekordboxSnapshot::childrenOf(const juce::String& parentId) const
{
    std::vector<const RekordboxPlaylist*> out;
    for (const auto& p : playlists)
        if (p.parentId == parentId) out.push_back(&p);
    std::stable_sort(out.begin(), out.end(), [](auto* a, auto* b) { return a->seq < b->seq; });
    return out;
}

void RekordboxSnapshot::buildIndex()
{
    trackIndex.clear();
    for (size_t i = 0; i < tracks.size(); ++i) trackIndex[tracks[i].id] = i;
}

std::optional<double> RekordboxTrackDetail::gridBpm() const
{
    for (const auto& b : beats)
        if (b.bpm > 0.0) return b.bpm;
    return std::nullopt;
}

std::optional<double> RekordboxTrackDetail::firstDownbeatSec() const
{
    for (const auto& b : beats)
        if (b.beatNumber == 1) return b.timeMs / 1000.0;
    if (!beats.empty()) return beats.front().timeMs / 1000.0;
    return std::nullopt;
}

Rekordbox::Rekordbox()
{
    const auto file = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile(kLibraryName);
    if (!file.existsAsFile())
    {
        loadError = "The rekordbox reader isn't included in this build.";
        return;
    }
    if (!library.open(file.getFullPathName()))
    {
        loadError = "The rekordbox reader couldn't be loaded.";
        log::warn("rekordbox", "failed to load " + file.getFullPathName());
        return;
    }
    auto f = std::make_unique<Functions>();
    f->open = reinterpret_cast<Functions::Open>(library.getFunction("rbx_open"));
    f->close = reinterpret_cast<Functions::Close>(library.getFunction("rbx_close"));
    f->freeString = reinterpret_cast<Functions::FreeString>(library.getFunction("rbx_free_string"));
    f->snapshot = reinterpret_cast<Functions::Snapshot>(library.getFunction("rbx_snapshot_json"));
    f->track = reinterpret_cast<Functions::Track>(library.getFunction("rbx_track_json"));
    f->tags = reinterpret_cast<Functions::Tags>(library.getFunction("rbx_read_tags_json"));
    f->picture = reinterpret_cast<Functions::Picture>(library.getFunction("rbx_extract_picture"));
    f->decode = reinterpret_cast<Functions::Decode>(library.getFunction("rbx_decode_to_wav"));
    f->fixture = reinterpret_cast<Functions::Fixture>(library.getFunction("rbx_build_fixture"));
    f->openDevice = reinterpret_cast<Functions::OpenDevice>(library.getFunction("rbx_open_device"));
    f->devices = reinterpret_cast<Functions::Devices>(library.getFunction("rbx_devices_json"));
    f->location = reinterpret_cast<Functions::Location>(library.getFunction("rbx_location_json"));
    f->refusal = reinterpret_cast<Functions::Refusal>(library.getFunction("rbx_write_refusal_json"));
    f->write = reinterpret_cast<Functions::Write>(library.getFunction("rbx_write_json"));
    f->deviceFixture = reinterpret_cast<Functions::Fixture>(library.getFunction("rbx_build_device_fixture"));
    if (f->open == nullptr || f->close == nullptr || f->freeString == nullptr || f->snapshot == nullptr
        || f->track == nullptr || f->tags == nullptr || f->picture == nullptr)
    {
        loadError = "The rekordbox reader is out of date.";
        log::warn("rekordbox", "missing exports in " + file.getFullPathName());
        return;
    }
    functions = std::move(f);
    log::info("rekordbox", "reader loaded from " + file.getFullPathName());
}

Rekordbox::~Rekordbox()
{
    std::scoped_lock lock(mutex);
    if (functions != nullptr && handle != nullptr) functions->close(handle);
    handle = nullptr;
}

juce::String Rekordbox::callJson(const std::function<char*()>& call)
{
    char* raw = call();
    if (raw == nullptr) return {};
    juce::String text = juce::String::fromUTF8(raw);
    functions->freeString(raw);
    return text;
}

bool Rekordbox::isOpen() const
{
    std::scoped_lock lock(mutex);
    return handle != nullptr;
}

RekordboxSnapshot Rekordbox::open(const juce::File& masterDb, const juce::String& passphrase)
{
    RekordboxSource source;
    if (masterDb != juce::File())
    {
        // A library rekordbox keeps on a drive (no passphrase: rekordbox's own key), or a
        // test fixture.
        source.kind = RekordboxSource::Kind::drive;
        source.masterDb = masterDb;
        source.name = masterDb.getFileName();
    }
    RekordboxSnapshot snapshot;
    snapshot.source = source;
    if (functions == nullptr)
    {
        snapshot.error = loadError;
        return snapshot;
    }
    std::scoped_lock lock(mutex);
    if (handle != nullptr) functions->close(handle);
    handle = nullptr;

    char* error = nullptr;
    const auto db = masterDb.getFullPathName();
    handle = functions->open(masterDb == juce::File() ? nullptr : db.toRawUTF8(),
                             passphrase.isEmpty() ? nullptr : passphrase.toRawUTF8(), &error);
    if (handle == nullptr)
    {
        snapshot.error = error != nullptr ? juce::String::fromUTF8(error) : juce::String("rekordbox library not found");
        if (error != nullptr) functions->freeString(error);
        log::warn("rekordbox", "open failed: " + snapshot.error);
        return snapshot;
    }
    return readSnapshot(source);
}

RekordboxSnapshot Rekordbox::open(const RekordboxSource& source)
{
    if (source.kind == RekordboxSource::Kind::computer) return open();
    if (source.kind == RekordboxSource::Kind::drive)
    {
        auto snapshot = open(source.masterDb);
        snapshot.source = source;
        std::scoped_lock lock(mutex);
        openSource = source;
        return snapshot;
    }
    RekordboxSnapshot snapshot;
    snapshot.source = source;
    if (functions == nullptr || functions->openDevice == nullptr)
    {
        snapshot.error = functions == nullptr ? loadError : juce::String("This build's rekordbox reader can't read devices.");
        return snapshot;
    }
    std::scoped_lock lock(mutex);
    if (handle != nullptr) functions->close(handle);
    handle = nullptr;
    char* error = nullptr;
    handle = functions->openDevice(source.root.getFullPathName().toRawUTF8(), source.format.toRawUTF8(), &error);
    if (handle == nullptr)
    {
        snapshot.error = error != nullptr ? juce::String::fromUTF8(error) : juce::String("no library on the device");
        if (error != nullptr) functions->freeString(error);
        log::warn("rekordbox", "device open failed: " + snapshot.error);
        return snapshot;
    }
    return readSnapshot(source);
}

std::vector<RekordboxSource> Rekordbox::listSources()
{
    std::vector<RekordboxSource> out(1); // this computer
    if (functions == nullptr || functions->devices == nullptr) return out;
    const auto json = juce::JSON::parse(callJson([&] { return functions->devices(); }));
    // Exports first: on a drive that also keeps an older desktop library, the export is the
    // one in use (that library is left out of the list altogether).
    if (const auto* devices = json.getProperty("devices", {}).getArray())
        for (const auto& d : *devices)
        {
            log::info("rekordbox", "drive " + str(d, "name") + ": " + str(d, "contents"));
            if (const auto* formats = d.getProperty("formats", {}).getArray())
                for (const auto& format : *formats)
                {
                    RekordboxSource source;
                    source.kind = RekordboxSource::Kind::device;
                    source.name = str(d, "name");
                    source.root = juce::File(str(d, "root"));
                    source.format = format.toString();
                    out.push_back(source);
                }
        }
    if (const auto* drives = json.getProperty("driveLibraries", {}).getArray())
        for (const auto& d : *drives)
        {
            RekordboxSource source;
            source.kind = RekordboxSource::Kind::drive;
            source.name = str(d, "name");
            source.masterDb = juce::File(str(d, "masterDb"));
            out.push_back(source);
        }
    return out;
}

// Reads the open handle's whole snapshot; the caller holds the lock.
RekordboxSnapshot Rekordbox::readSnapshot(RekordboxSource source)
{
    RekordboxSnapshot snapshot;
    snapshot.source = source;
    openSource = source;
    openDatabase = juce::File();
    if (functions->location != nullptr)
        if (const auto where = str(juce::JSON::parse(callJson([&] { return functions->location(handle); })), "masterDb");
            where.isNotEmpty())
            openDatabase = juce::File(where);

    const auto started = juce::Time::getMillisecondCounterHiRes();
    const auto json = juce::JSON::parse(callJson([&] { return functions->snapshot(handle); }));
    if (!static_cast<bool>(json.getProperty("ok", false)))
    {
        snapshot.error = str(json, "error");
        return snapshot;
    }
    snapshot.ok = true;
    snapshot.masterDb = str(json, "masterDb");
    snapshot.shareRoot = str(json, "shareRoot");
    if (const auto* tracks = json.getProperty("tracks", {}).getArray())
    {
        snapshot.tracks.reserve(static_cast<size_t>(tracks->size()));
        for (const auto& t : *tracks)
        {
            RekordboxTrack track;
            track.id = str(t, "id");
            track.title = str(t, "title");
            track.artist = str(t, "artist");
            track.album = str(t, "album");
            track.genre = str(t, "genre");
            track.label = str(t, "label");
            track.key = str(t, "key");
            track.comment = str(t, "comment");
            track.dateAdded = str(t, "dateAdded");
            track.path = str(t, "path");
            track.analysisPath = str(t, "analysisPath");
            track.artworkPath = str(t, "artworkPath");
            track.hotCues = str(t, "hotCues");
            track.bpm = num(t, "bpm");
            track.durationMs = num(t, "durationMs");
            track.rating = static_cast<int>(num(t, "rating"));
            track.colour = static_cast<int>(num(t, "colour"));
            track.playCount = static_cast<int>(num(t, "playCount"));
            track.sampleRate = static_cast<int>(num(t, "sampleRate"));
            track.bitrate = static_cast<int>(num(t, "bitrate"));
            track.year = static_cast<int>(num(t, "year"));
            snapshot.tracks.push_back(std::move(track));
        }
    }
    if (const auto* lists = json.getProperty("playlists", {}).getArray())
    {
        for (const auto& p : *lists)
        {
            RekordboxPlaylist list;
            list.id = str(p, "id");
            list.name = str(p, "name");
            list.parentId = str(p, "parentId");
            list.kind = str(p, "kind");
            list.seq = static_cast<int>(num(p, "seq"));
            if (const auto* ids = p.getProperty("trackIds", {}).getArray())
                for (const auto& id : *ids) list.trackIds.add(id.toString());
            snapshot.playlists.push_back(std::move(list));
        }
    }
    snapshot.buildIndex();
    log::info("rekordbox", "opened " + snapshot.masterDb + ": " + juce::String(static_cast<int>(snapshot.tracks.size()))
                               + " tracks, " + juce::String(static_cast<int>(snapshot.playlists.size())) + " playlists in "
                               + juce::String(juce::Time::getMillisecondCounterHiRes() - started, 0) + " ms");
    return snapshot;
}

RekordboxTrackDetail Rekordbox::trackDetail(const juce::String& trackId)
{
    RekordboxTrackDetail detail;
    std::scoped_lock lock(mutex);
    if (functions == nullptr || handle == nullptr)
    {
        detail.error = "no rekordbox library open";
        return detail;
    }
    const auto json = juce::JSON::parse(callJson([&] { return functions->track(handle, trackId.toRawUTF8()); }));
    detail.ok = static_cast<bool>(json.getProperty("ok", false));
    detail.error = str(json, "error");
    detail.mood = str(json, "mood");
    if (const auto* bands = json.getProperty("bands", {}).getArray())
        for (const auto& b : *bands)
            if (const auto* three = b.getArray(); three != nullptr && three->size() >= 3)
                detail.bands.push_back({static_cast<float>((*three)[0]), static_cast<float>((*three)[1]), static_cast<float>((*three)[2])});
    if (const auto* cues = json.getProperty("cues", {}).getArray())
        for (const auto& c : *cues)
        {
            RekordboxCue cue;
            cue.positionMs = num(c, "positionMs");
            const auto end = c.getProperty("endMs", {});
            cue.endMs = end.isVoid() ? -1.0 : static_cast<double>(end);
            cue.hot = str(c, "hot");
            cue.memory = static_cast<bool>(c.getProperty("memory", false));
            cue.colour = static_cast<int>(num(c, "colour"));
            detail.cues.push_back(cue);
        }
    if (const auto* beats = json.getProperty("beats", {}).getArray())
    {
        detail.beats.reserve(static_cast<size_t>(beats->size()));
        for (const auto& b : *beats)
            if (const auto* triple = b.getArray(); triple != nullptr && triple->size() >= 3)
                detail.beats.push_back({static_cast<double>((*triple)[0]), static_cast<int>((*triple)[1]),
                                        static_cast<double>((*triple)[2])});
    }
    if (const auto* phrases = json.getProperty("phrases", {}).getArray())
        for (const auto& p : *phrases)
        {
            RekordboxPhrase phrase;
            phrase.label = str(p, "label");
            const auto s = p.getProperty("startMs", {});
            const auto e = p.getProperty("endMs", {});
            phrase.startMs = s.isVoid() ? -1.0 : static_cast<double>(s);
            phrase.endMs = e.isVoid() ? -1.0 : static_cast<double>(e);
            detail.phrases.push_back(phrase);
        }
    return detail;
}

juce::var Rekordbox::readTags(const juce::File& file)
{
    if (functions == nullptr) return {};
    const auto path = file.getFullPathName();
    char* raw = functions->tags(path.toRawUTF8());
    if (raw == nullptr) return {};
    const auto json = juce::JSON::parse(juce::String::fromUTF8(raw));
    functions->freeString(raw);
    return static_cast<bool>(json.getProperty("ok", false)) ? json : juce::var();
}

bool Rekordbox::extractPicture(const juce::File& audio, const juce::File& out)
{
    if (functions == nullptr) return false;
    out.getParentDirectory().createDirectory();
    return functions->picture(audio.getFullPathName().toRawUTF8(), out.getFullPathName().toRawUTF8()) == 1;
}

bool Rekordbox::decodeToWav(const juce::File& source, const juce::File& wav)
{
    if (functions == nullptr || functions->decode == nullptr) return false;
    wav.getParentDirectory().createDirectory();
    const bool ok = functions->decode(source.getFullPathName().toRawUTF8(), wav.getFullPathName().toRawUTF8()) == 1;
    log::info("rekordbox", "decode " + source.getFullPathName() + (ok ? " -> " + wav.getFullPathName() : " failed"));
    return ok;
}

juce::String Rekordbox::lastChangedKey() const
{
    std::scoped_lock lock(mutex);
    return changedKey;
}

namespace
{
juce::File& testBackupFolder()
{
    static juce::File folder;
    return folder;
}
} // namespace

void Rekordbox::setBackupFolderForTest(const juce::File& folder)
{
    testBackupFolder() = folder;
}

juce::File Rekordbox::backupFolder()
{
    if (testBackupFolder() != juce::File()) return testBackupFolder();
    return juce::File::getSpecialLocation(juce::File::userHomeDirectory)
        .getChildFile(appFolderName())
        .getChildFile("Backups")
        .getChildFile("rekordbox");
}

// Copies the database files of `source` aside, once per library (a stick's two together)
// per app session. Writes
// are refused while rekordbox is running, so the files are at rest while they're copied.
bool Rekordbox::backUp(const RekordboxSource& source, const juce::File& database, juce::String& error)
{
    if (database == juce::File())
    {
        error = "Couldn't tell where the library's database is, so it couldn't be backed up.";
        return false;
    }
    // A stick's two libraries sit together and are copied together, once per stick.
    const auto dir = database.getParentDirectory();
    const auto id = source.isDevice() ? dir.getFullPathName() : database.getFullPathName();
    if (backedUp.count(id) > 0) return true;

    juce::StringArray names;
    if (source.isDevice())
        names = {"export.pdb", "exportExt.pdb", "exportLibrary.db", "exportLibrary.db-wal", "exportLibrary.db-shm"};
    else
        names = {database.getFileName(), database.getFileName() + "-wal", database.getFileName() + "-shm"};

    const auto base = juce::File::createLegalFileName(juce::Time::getCurrentTime().formatted("%Y-%m-%d %H-%M-%S") + " "
                                                      + (source.isDevice() ? source.name + " (USB export)" : source.label()));
    auto folder = backupFolder().getChildFile(base);
    for (int n = 2; folder.exists(); ++n) folder = backupFolder().getChildFile(base + " " + juce::String(n));
    if (const auto made = folder.createDirectory(); made.failed())
    {
        error = "Couldn't make the backup folder " + folder.getFullPathName() + ": " + made.getErrorMessage() + ".";
        return false;
    }
    int copied = 0;
    for (const auto& name : names)
    {
        const auto file = dir.getChildFile(name);
        if (!file.existsAsFile()) continue;
        if (!file.copyFileTo(folder.getChildFile(name)))
        {
            error = "Couldn't copy " + file.getFullPathName() + " to " + folder.getFullPathName() + ".";
            return false;
        }
        ++copied;
    }
    if (copied == 0)
    {
        error = "Found no database files to back up in " + dir.getFullPathName() + ".";
        return false;
    }
    folder.getChildFile("README.txt")
        .replaceWithText("Stemcel copied these files before its first change to " + source.label()
                         + " in this session.\n\nFrom: " + dir.getFullPathName()
                         + "\n\nTo put them back, quit rekordbox and copy them over the files there.\n");
    backedUp.insert(id);
    log::info("rekordbox", "backed up " + juce::String(copied) + " file(s) of " + dir.getFullPathName() + " to "
                               + folder.getFullPathName());
    return true;
}

// Refusal, backup, write; the caller holds the lock.
juce::var Rekordbox::writeTo(void* target, const RekordboxSource& source, const juce::File& database, const juce::var& request)
{
    const auto refusal = juce::JSON::parse(callJson([&] { return functions->refusal(target); }));
    if (!isOk(refusal)) return failure(str(refusal, "error"));
    juce::String error;
    if (!backUp(source, database, error)) return failure(error + " Nothing was changed.");

    const auto text = juce::JSON::toString(request, true);
    auto result = juce::JSON::parse(callJson([&] { return functions->write(target, text.toRawUTF8()); }));
    if (!result.isObject()) return failure("The rekordbox writer gave no answer.");
    if (isOk(result))
    {
        changedKey = source.key();
        changed.sendChangeMessage();
        log::info("rekordbox", source.label() + ": " + text + " -> " + juce::JSON::toString(result, true));
    }
    else
    {
        log::warn("rekordbox", source.label() + ": " + text + " refused: " + str(result, "error"));
    }
    return result;
}

juce::var Rekordbox::write(const juce::var& request)
{
    if (functions == nullptr) return failure(loadError);
    if (functions->write == nullptr || functions->refusal == nullptr) return failure("This build's rekordbox reader can't write.");
    std::scoped_lock lock(mutex);
    if (handle == nullptr) return failure("No rekordbox library is open.");
    return writeTo(handle, openSource, openDatabase, request);
}

juce::var Rekordbox::addToComputerLibrary(const juce::Array<juce::File>& files, const juce::String& playlist)
{
    if (functions == nullptr) return failure(loadError);
    if (functions->write == nullptr || functions->refusal == nullptr || functions->location == nullptr)
        return failure("This build's rekordbox reader can't write.");
    juce::Array<juce::var> paths;
    for (const auto& f : files) paths.add(f.getFullPathName());
    auto* request = new juce::DynamicObject();
    request->setProperty("action", "importFiles");
    request->setProperty("paths", paths);
    if (playlist.isNotEmpty()) request->setProperty("playlist", playlist);
    const juce::var requestVar(request);

    std::scoped_lock lock(mutex);
    if (handle != nullptr && openSource.kind == RekordboxSource::Kind::computer)
        return writeTo(handle, openSource, openDatabase, requestVar);

    // Another library is on show: open this computer's on the side for the one write.
    char* error = nullptr;
    void* computer = functions->open(nullptr, nullptr, &error);
    if (computer == nullptr)
    {
        const auto message = error != nullptr ? juce::String::fromUTF8(error) : juce::String("not found");
        if (error != nullptr) functions->freeString(error);
        return failure("Couldn't open this computer's rekordbox library: " + message);
    }
    const juce::File database(str(juce::JSON::parse(callJson([&] { return functions->location(computer); })), "masterDb"));
    auto result = writeTo(computer, RekordboxSource{}, database, requestVar);
    functions->close(computer);
    return result;
}

juce::var Rekordbox::buildDeviceFixture(const juce::File& dir, const juce::File& audio)
{
    if (functions == nullptr || functions->deviceFixture == nullptr) return {};
    char* raw = functions->deviceFixture(dir.getFullPathName().toRawUTF8(), audio.getFullPathName().toRawUTF8());
    if (raw == nullptr) return {};
    const auto json = juce::JSON::parse(juce::String::fromUTF8(raw));
    functions->freeString(raw);
    return json;
}

juce::var Rekordbox::buildFixture(const juce::File& dir, const juce::File& audio)
{
    if (functions == nullptr || functions->fixture == nullptr) return {};
    char* raw = functions->fixture(dir.getFullPathName().toRawUTF8(), audio.getFullPathName().toRawUTF8());
    if (raw == nullptr) return {};
    const auto json = juce::JSON::parse(juce::String::fromUTF8(raw));
    functions->freeString(raw);
    return json;
}

Rekordbox& sharedRekordbox()
{
    static Rekordbox instance;
    return instance;
}

} // namespace stemcel::native
