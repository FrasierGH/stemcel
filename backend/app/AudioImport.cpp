#include "AudioImport.h"
#include "SectionAnalysis.h"

#include "AppPaths.h"
#include "KeyDetection.h"
#include "Log.h"
#include "MediaStore.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace stemcel::native
{
namespace
{
const juce::StringArray kAudioExtensions{"wav", "wave", "aif", "aiff", "flac", "mp3", "m4a", "aac", "mp4",
                                         "alac", "ogg", "oga", "wma", "caf"};
constexpr double kMinManualBpm = 20.0;
constexpr double kMaxManualBpm = 300.0;

juce::var object(std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();
    for (const auto& [k, v] : props)
        if (!v.isVoid()) o->setProperty(k, v);
    return juce::var(o);
}

juce::File transcodeCacheDir()
{
    // Shared with the Electron app (main/transcodeCache.ts: os.tmpdir()), which sweeps it.
    // JUCE's tempDirectory is a per-app folder under ~/Library/Caches on macOS.
    return stemcel::systemTempDirectory().getChildFile("stemcel-transcode-cache");
}

juce::File transcodeTarget(const juce::File& source)
{
    const auto key = source.getFullPathName() + "|" + juce::String(source.getSize()) + "|"
                     + juce::String(source.getLastModificationTime().toMilliseconds());
    return transcodeCacheDir().getChildFile("native-" + juce::String::toHexString(key.hashCode64()) + ".wav");
}

juce::String text(const juce::var& o, const char* key)
{
    const auto v = o.getProperty(key, {});
    return v.isVoid() ? juce::String() : v.toString().trim();
}
} // namespace

struct AudioImporter::Prepared
{
    bool ok = false;
    juce::String error;
    juce::File file;
    juce::String playbackPath;
    double durationMs = 0.0;
    int sampleRate = 0;
    int channels = 0;
    juce::String key;
    juce::var metadata; // AudioMetadata shape
    juce::File cover;   // temp file holding the embedded picture, when any
    double bpm = 0.0;   // rekordbox's tempo, applied as a manual tempo
    double beatAnchorSec = 0.0;
    juce::var sections; // rekordbox's phrases, as the item's arrangement sections
};

AudioImporter::AudioImporter(EngineClient& client, ProjectMirror& projectMirror, Rekordbox& rb)
    : engine(client), mirror(projectMirror), rekordbox(rb)
{
}

AudioImporter::~AudioImporter()
{
    alive->store(false);
    pool.removeAllJobs(true, 30000);
}

bool AudioImporter::isAudioFile(const juce::File& file)
{
    return file.existsAsFile() && kAudioExtensions.contains(file.getFileExtension().trimCharactersAtStart(".").toLowerCase());
}

juce::String AudioImporter::audioFileWildcard()
{
    juce::StringArray patterns;
    for (const auto& e : kAudioExtensions) patterns.add("*." + e);
    return patterns.joinIntoString(";");
}

juce::String AudioImporter::playablePath(const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    if (std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file)); reader != nullptr)
        return file.getFullPathName();
    const auto target = transcodeTarget(file);
    if (target.existsAsFile() && target.getSize() > 44)
    {
        target.setLastModificationTime(juce::Time::getCurrentTime()); // keeps it young for the sweep
        return target.getFullPathName();
    }
    transcodeCacheDir().createDirectory();
    if (rekordbox.decodeToWav(file, target)) return target.getFullPathName();
    target.deleteFile();
    return {};
}

void AudioImporter::import(const juce::Array<juce::File>& files, Done done)
{
    std::vector<ImportRequest> requests;
    for (const auto& f : files) requests.push_back({f, {}});
    import(std::move(requests), std::move(done));
}

void AudioImporter::import(std::vector<ImportRequest> requests, Done done)
{
    if (requests.empty())
    {
        if (done) done({});
        return;
    }
    struct Batch
    {
        std::vector<Prepared> results;
        std::atomic<int> remaining{0};
    };
    auto batch = std::make_shared<Batch>();
    batch->results.resize(requests.size());
    batch->remaining = static_cast<int>(requests.size());
    pending += static_cast<int>(requests.size());
    if (onProgress) onProgress();

    for (size_t i = 0; i < requests.size(); ++i)
    {
        pool.addJob(
            [this, batch, i, request = requests[i], done, alive = alive]
            {
                if (!alive->load()) return;
                batch->results[i] = prepare(request);
                if (--batch->remaining == 0)
                    juce::MessageManager::callAsync(
                        [this, batch, done, alive]
                        {
                            if (!alive->load()) return;
                            confirmRateThenCommit(std::move(batch->results), done);
                        });
            });
    }
}

AudioImporter::Prepared AudioImporter::prepare(const ImportRequest& request)
{
    Prepared p;
    p.file = request.file;
    if (!request.file.existsAsFile())
    {
        p.error = "\"" + request.file.getFileName() + "\" couldn't be found.";
        return p;
    }

    p.playbackPath = playablePath(request.file);
    if (p.playbackPath.isEmpty())
    {
        p.error = "\"" + request.file.getFileName() + "\" isn't an audio format Stemcel can read.";
        return p;
    }
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juce::File(p.playbackPath)));
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
    {
        p.error = "\"" + request.file.getFileName() + "\" couldn't be decoded.";
        return p;
    }
    p.sampleRate = static_cast<int>(reader->sampleRate);
    p.channels = static_cast<int>(reader->numChannels);
    p.durationMs = static_cast<double>(reader->lengthInSamples) * 1000.0 / reader->sampleRate;

    // Tags (lofty, via the rekordbox reader) — the true source rate wins over a transcode's.
    const auto tags = rekordbox.readTags(request.file);
    if (static_cast<int>(tags.getProperty("sampleRate", 0)) > 0)
        p.sampleRate = static_cast<int>(tags.getProperty("sampleRate", 0));

    // Key: rekordbox's analysis, else detection (the renderer prefers detection to tags),
    // else the tag.
    p.key = normaliseKeyName(request.hints.key);
    if (p.key.isEmpty()) p.key = detectMusicalKey(*reader);
    if (p.key.isEmpty()) p.key = normaliseKeyName(text(tags, "key"));

    // Tempo from rekordbox: the grid's own first downbeat and tempo.
    if (request.hints.rekordboxId.isNotEmpty())
    {
        const auto detail = rekordbox.trackDetail(request.hints.rekordboxId);
        p.bpm = detail.gridBpm().value_or(request.hints.bpm);
        p.beatAnchorSec = detail.firstDownbeatSec().value_or(0.0);
        if (const auto list = sections::fromRekordbox(detail, p.bpm, p.beatAnchorSec, p.durationMs / 1000.0); !list.empty())
            p.sections = sections::toVar(list);
    }
    else if (request.hints.bpm > 0.0)
    {
        p.bpm = request.hints.bpm;
    }
    if (p.bpm < kMinManualBpm || p.bpm > kMaxManualBpm) p.bpm = 0.0;

    // AudioMetadata for the media store.
    auto* meta = new juce::DynamicObject();
    auto title = text(tags, "title");
    auto artist = text(tags, "artist");
    if (title.isEmpty()) title = request.hints.title;
    if (artist.isEmpty()) artist = request.hints.artist;
    if (title.isNotEmpty()) meta->setProperty("title", title);
    if (artist.isNotEmpty()) meta->setProperty("artist", artist);
    if (const auto album = text(tags, "album"); album.isNotEmpty()) meta->setProperty("album", album);
    if (const auto genre = text(tags, "genre"); genre.isNotEmpty()) meta->setProperty("genre", juce::Array<juce::var>{genre});
    if (static_cast<int>(tags.getProperty("year", 0)) > 0) meta->setProperty("year", tags.getProperty("year", 0));
    if (p.bpm > 0.0) meta->setProperty("bpm", p.bpm);
    else if (static_cast<double>(tags.getProperty("bpm", 0.0)) > 0.0) meta->setProperty("bpm", tags.getProperty("bpm", 0.0));
    if (p.key.isNotEmpty()) meta->setProperty("key", p.key);
    if (static_cast<int>(tags.getProperty("bitrate", 0)) > 0) meta->setProperty("bitrate", tags.getProperty("bitrate", 0));
    meta->setProperty("container", request.file.getFileExtension().trimCharactersAtStart(".").toUpperCase());
    meta->setProperty("durationMs", p.durationMs);
    meta->setProperty("sampleRate", p.sampleRate);
    meta->setProperty("channelCount", p.channels);
    p.metadata = juce::var(meta);

    if (static_cast<bool>(tags.getProperty("hasPicture", false)))
    {
        const auto cover = juce::File::createTempFile(".img");
        if (rekordbox.extractPicture(request.file, cover)) p.cover = cover;
    }
    p.ok = true;
    return p;
}

juce::String AudioImporter::nextItemId() const
{
    // The renderer mints `l<N>`; continue past the highest in the project.
    int next = 1;
    for (const auto& item : mirror.model().library)
        if (item.id.startsWithChar('l') && item.id.substring(1).containsOnly("0123456789"))
            next = juce::jmax(next, item.id.substring(1).getIntValue() + 1);
    return "l" + juce::String(next);
}

void AudioImporter::confirmRateThenCommit(std::vector<Prepared> prepared, Done done)
{
    mirror.refreshNow();
    const int projectRate = static_cast<int>(mirror.model().raw.getProperty("targetSampleRate", defaultProjectSampleRate));
    std::map<int, int> byRate;
    for (const auto& p : prepared)
        if (p.ok && p.sampleRate > 0 && p.sampleRate != projectRate) ++byRate[p.sampleRate];
    if (byRate.empty() || juce::JUCEApplicationBase::getCommandLineParameters().contains("--smoke-test"))
    {
        commit(std::move(prepared), done);
        return;
    }
    // The project can run at 44.1 or 48 kHz; offer the highest supported rate among the files.
    int target = 0;
    juce::StringArray summary;
    for (const auto& [rate, count] : byRate)
    {
        summary.add(juce::String(count) + " at " + juce::String(rate) + " Hz");
        if (rate == 44100 || rate == 48000) target = juce::jmax(target, rate);
        if (rate > 48000) target = 48000;
    }
    auto shared = std::make_shared<std::vector<Prepared>>(std::move(prepared));
    auto options = juce::MessageBoxOptions()
                       .withIconType(juce::MessageBoxIconType::QuestionIcon)
                       .withTitle("Sample rate")
                       .withMessage("This project runs at " + juce::String(projectRate) + " Hz, but the audio is "
                                    + summary.joinIntoString(", ") + ". It will be resampled to play.")
                       .withButton("Keep " + juce::String(projectRate) + " Hz");
    const bool canSwitch = target != 0 && target != projectRate;
    if (canSwitch) options = options.withButton("Switch project to " + juce::String(target) + " Hz");
    options = options.withButton("Cancel");
    juce::AlertWindow::showAsync(options,
                                 [this, shared, done, canSwitch, target, alive = alive](int result)
                                 {
                                     if (!alive->load()) return;
                                     // Buttons: 1 = keep, 2 = switch (when offered), 0 = cancel.
                                     if (result == 0)
                                     {
                                         pending = juce::jmax(0, pending - static_cast<int>(shared->size()));
                                         if (onProgress) onProgress();
                                         if (done) done({});
                                         return;
                                     }
                                     if (canSwitch && result == 2)
                                     {
                                         auto* p = new juce::DynamicObject();
                                         p->setProperty("sampleRate", target);
                                         engine.send("PROJECT_SET_TARGET_SAMPLE_RATE", juce::var(p));
                                     }
                                     commit(std::move(*shared), done);
                                 });
}

void AudioImporter::commit(std::vector<Prepared> prepared, const Done& done)
{
    juce::StringArray itemIds;
    juce::StringArray failures;
    mirror.refreshNow();
    const auto mediaRoot = media::rootFor(mirror.model().filePath);

    engine.inUndoGroup(
        "Import audio",
        [&]
        {
            for (auto& p : prepared)
            {
                --pending;
                if (!p.ok)
                {
                    failures.add(p.error);
                    continue;
                }
                const auto path = p.file.getFullPathName();
                juce::String existing;
                for (const auto& item : mirror.model().library)
                    if ((item.kind == "source" || item.kind == "sample") && item.filePath == path) existing = item.id;
                if (existing.isNotEmpty())
                {
                    itemIds.add(existing);
                    continue;
                }

                const auto itemId = nextItemId();
                const auto mediaId = juce::Uuid().toDashedString();
                media::save(mediaRoot, mediaId, p.metadata, p.cover);
                p.cover.deleteFile();
                engine.send("LIBRARY_ADD", object({{"itemId", itemId},
                                                   {"filePath", path},
                                                   {"fileName", p.file.getFileName()},
                                                   {"kind", "source"},
                                                   {"durationMs", p.durationMs},
                                                   {"sampleRate", p.sampleRate},
                                                   {"channelCount", p.channels},
                                                   {"playbackFilePath", p.playbackPath},
                                                   {"key", p.key.isNotEmpty() ? juce::var(p.key) : juce::var()},
                                                   {"mediaId", mediaId}}));
                if (p.bpm > 0.0)
                    engine.send("LIBRARY_ITEM_SET_MANUAL_TEMPO",
                                object({{"itemId", itemId}, {"bpm", p.bpm}, {"beatAnchorSec", p.beatAnchorSec}}));
                if (p.sections.isArray()) engine.send("LIBRARY_ITEM_SET_SECTIONS", object({{"itemId", itemId}, {"sections", p.sections}}));
                mirror.refreshNow();
                itemIds.add(itemId);
                log::info("import", "imported " + path + " as " + itemId + (p.bpm > 0.0 ? " bpm=" + juce::String(p.bpm, 2) : ""));
            }
            // Inside the group, so placing what was imported is the same undo step.
            if (done) done(itemIds);
        });

    pending = juce::jmax(0, pending);
    if (onProgress) onProgress();
    if (!failures.isEmpty() && onNotify)
        onNotify(failures.size() == 1 ? failures[0] : juce::String(failures.size()) + " files couldn't be imported.", true);
}

} // namespace stemcel::native
