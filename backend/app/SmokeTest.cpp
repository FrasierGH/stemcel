#include "SmokeTest.h"

#include "RekordboxBrowser.h"

#include "Db.h"

#include <algorithm>

#include "AutoMix.h"
#include "BeatStutter.h"
#include "Automation.h"
#include "ClipEditor.h"
#include "ExportDialog.h"
#include "LibraryPanel.h"
#include "Mashup.h"
#include "ProjectTabs.h"
#include "MixTransitions.h"
#include "TimelineView.h"
#include "TrackHeaders.h"
#include "TransitionEditor.h"
#include "UvrSeparator.h"
#include "PreferencePages.h"
#include "ScratchEditor.h"
#include "SectionAnalyser.h"
#include "SectionAnalysis.h"
#include "FxPanels.h"
#include "PluginCatalogue.h"
#include "ResizeBar.h"
#include "TrackAutomationSnapshot.h"
#include "TempoReport.h"
#include "Tracklist.h"
#include "WaveformStore.h"
#include "Workspace.h"
#include "Autosave.h"
#include "Log.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <map>

namespace stemcel::native
{
namespace
{
template <typename T>
T* findDescendant(juce::Component* parent)
{
    if (parent == nullptr) return nullptr;
    for (auto* child : parent->getChildren())
    {
        if (auto* match = dynamic_cast<T*>(child)) return match;
        if (auto* deeper = findDescendant<T>(child)) return deeper;
    }
    return nullptr;
}

// The open in-place name field under `parent`, typed into and committed with Return as
// the user would.
bool typeIntoInlineEditor(juce::Component* parent, const juce::String& text)
{
    if (parent == nullptr) return false;
    for (auto* child : parent->getChildren())
    {
        if (auto* editor = dynamic_cast<juce::TextEditor*>(child); editor != nullptr && editor->isVisible())
        {
            editor->setText(text, true);
            return editor->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
        }
        if (typeIntoInlineEditor(child, text)) return true;
    }
    return false;
}
} // namespace
namespace
{
constexpr double kStepTimeoutMs = 30000.0;
constexpr double kToneSeconds = 4.0;
constexpr int kSampleRate = 44100;

juce::var object(std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();
    for (const auto& [k, v] : props) o->setProperty(k, v);
    return juce::var(o);
}

bool writeTone(const juce::File& file, double frequency = 220.0)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
    const auto options = juce::AudioFormatWriterOptions{}.withSampleRate(kSampleRate).withNumChannels(2).withBitsPerSample(16);
    std::unique_ptr<juce::AudioFormatWriter> writer(juce::WavAudioFormat().createWriterFor(stream, options));
    if (writer == nullptr) return false;
    const int samples = static_cast<int>(kToneSeconds * kSampleRate);
    juce::AudioBuffer<float> buffer(2, samples);
    for (int i = 0; i < samples; ++i)
    {
        const auto v = static_cast<float>(0.4 * std::sin(2.0 * juce::MathConstants<double>::pi * frequency * i / kSampleRate));
        buffer.setSample(0, i, v);
        buffer.setSample(1, i, v);
    }
    return writer->writeFromAudioSampleBuffer(buffer, 0, samples);
}

// 32 bars at 120 BPM with an arrangement to find: hats alone (intro), a kick and bass on every
// beat (drop), a mid pad with no kick (breakdown), then the kick again.
bool writeArrangement(const juce::File& file, float level = 1.0f)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
    const auto options = juce::AudioFormatWriterOptions{}.withSampleRate(kSampleRate).withNumChannels(2).withBitsPerSample(16);
    std::unique_ptr<juce::AudioFormatWriter> writer(juce::WavAudioFormat().createWriterFor(stream, options));
    if (writer == nullptr) return false;
    constexpr double kBeat = 0.5;
    const int samples = static_cast<int>(32 * 4 * kBeat * kSampleRate);
    juce::AudioBuffer<float> buffer(2, samples);
    juce::Random noise(7);
    const double twoPi = juce::MathConstants<double>::twoPi;
    for (int i = 0; i < samples; ++i)
    {
        const double t = static_cast<double>(i) / kSampleRate;
        const int bar = static_cast<int>(t / (4 * kBeat));
        const bool drop = (bar >= 8 && bar < 16) || bar >= 24;
        const bool breakdown = bar >= 16 && bar < 24;
        double v = (noise.nextFloat() * 2.0 - 1.0) * 0.03; // hats / air throughout
        if (drop)
        {
            const double inBeat = std::fmod(t, kBeat);
            v += 0.8 * std::exp(-inBeat * 18.0) * std::sin(twoPi * 55.0 * inBeat); // kick
            v += 0.25 * std::sin(twoPi * 45.0 * t);                                  // bass
        }
        if (breakdown) v += 0.2 * std::sin(twoPi * 440.0 * t) + 0.1 * std::sin(twoPi * 660.0 * t);
        const auto f = static_cast<float>(juce::jlimit(-1.0, 1.0, v)) * level;
        buffer.setSample(0, i, f);
        buffer.setSample(1, i, f);
    }
    return writer->writeFromAudioSampleBuffer(buffer, 0, samples);
}

// 40 bars of clicks: 4 steady at 120 BPM, then rising evenly to 132 (a drifting recording).
// `beatsSec` gets each click's time.
bool writeDriftingClicks(const juce::File& file, std::vector<double>& beatsSec)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
    const auto options = juce::AudioFormatWriterOptions{}.withSampleRate(kSampleRate).withNumChannels(2).withBitsPerSample(16);
    std::unique_ptr<juce::AudioFormatWriter> writer(juce::WavAudioFormat().createWriterFor(stream, options));
    if (writer == nullptr) return false;
    beatsSec.clear();
    double t = 0.25;
    constexpr int total = 40 * 4;
    for (int k = 0; k < total; ++k)
    {
        beatsSec.push_back(t);
        t += 60.0 / (k < 16 ? 120.0 : 120.0 + 12.0 * (k - 16) / static_cast<double>(total - 16));
    }
    const int samples = static_cast<int>((t + 0.5) * kSampleRate);
    juce::AudioBuffer<float> buffer(2, samples);
    buffer.clear();
    juce::Random noise(11);
    for (const double b : beatsSec)
    {
        const int start = static_cast<int>(b * kSampleRate);
        for (int i = 0; i < 1500 && start + i < samples; ++i)
        {
            const auto v = static_cast<float>(std::exp(-i / 250.0) * (noise.nextFloat() * 2.0 - 1.0) * 0.8
                                              + 0.6 * std::exp(-i / 400.0) * std::sin(juce::MathConstants<double>::twoPi * 60.0 * i / kSampleRate));
            buffer.setSample(0, start + i, v);
            buffer.setSample(1, start + i, v);
        }
    }
    return writer->writeFromAudioSampleBuffer(buffer, 0, samples);
}

// A kick on every beat, so tempo detection has something unambiguous to find.
bool writeClicks(const juce::File& file, double bpm, double seconds = 30.0)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
    const auto options = juce::AudioFormatWriterOptions{}.withSampleRate(kSampleRate).withNumChannels(2).withBitsPerSample(16);
    std::unique_ptr<juce::AudioFormatWriter> writer(juce::WavAudioFormat().createWriterFor(stream, options));
    if (writer == nullptr) return false;
    const int samples = static_cast<int>(seconds * kSampleRate);
    const double beatSamples = 60.0 / bpm * kSampleRate;
    juce::AudioBuffer<float> buffer(2, samples);
    buffer.clear();
    for (double beat = 0.0; beat < samples; beat += beatSamples)
    {
        const int start = static_cast<int>(beat);
        const int length = juce::jmin(samples - start, static_cast<int>(0.12 * kSampleRate));
        for (int i = 0; i < length; ++i)
        {
            const double t = static_cast<double>(i) / kSampleRate;
            const auto v = static_cast<float>(0.8 * std::exp(-t * 35.0) * std::sin(2.0 * juce::MathConstants<double>::pi * 60.0 * t)
                                              + (i < 200 ? 0.3 * std::exp(-i / 40.0) : 0.0));
            buffer.setSample(0, start + i, v);
            buffer.setSample(1, start + i, v);
        }
    }
    return writer->writeFromAudioSampleBuffer(buffer, 0, samples);
}
} // namespace

bool SmokeTest::isRequested(const juce::String& commandLine, juce::File& workDirOut)
{
    auto tokens = juce::StringArray::fromTokens(commandLine, true);
    const int index = tokens.indexOf("--smoke-test");
    if (index < 0 || index + 1 >= tokens.size()) return false;
    workDirOut = juce::File(tokens[index + 1].unquoted());
    return true;
}

SmokeTest::SmokeTest(EngineClient& client, juce::File dir)
    : engine(client), workDir(std::move(dir)), mirror(client), actions(client, mirror),
      importer(client, mirror, sharedRekordbox()), stems(client, mirror, actions, prefs)
{
    failureSubscription = engine.subscribe("MIXDOWN_FAILED", [this](const juce::var& p)
                                           { report.add("MIXDOWN_FAILED: " + p.getProperty("error", {}).toString()); });
    addSteps();
}

// The mirror is refreshed once per check (runStep), so pointers into it stay valid while
// a check reads them.
const ProjectModel& SmokeTest::model()
{
    return mirror.model();
}

void SmokeTest::addSteps()
{
    const auto projectFile = workDir.getChildFile("Smoke").getChildFile("Smoke.stemcel");
    const auto toneFile = workDir.getChildFile("tone.wav");
    auto clipCount = [this]
    {
        const auto* t = model().findTrack(trackId);
        return t != nullptr ? static_cast<int>(t->clips.size()) : -1;
    };

    steps.push_back({"engine ready", [] {}, "ENGINE_AUDIO_STATUS", [](const juce::var&) { return true; }});
    steps.push_back({"new project", [this] { engine.send("PROJECT_NEW"); }, "PROJECT_STATE",
                     [](const juce::var& p) { return static_cast<bool>(p.getProperty("reset", false)); }});
    steps.push_back({"add track", [this] { trackId = actions.addTrack(); }, {},
                     [this](const juce::var&) { return model().findTrack(trackId) != nullptr; }});
    steps.push_back({"import audio",
                     [this, toneFile]
                     {
                         if (!writeTone(toneFile)) return;
                         itemId = EngineClient::newId("lib");
                         engine.send("LIBRARY_ADD", object({{"itemId", itemId},
                                                            {"filePath", toneFile.getFullPathName()},
                                                            {"fileName", toneFile.getFileName()},
                                                            {"name", "Tone"},
                                                            {"kind", "source"},
                                                            {"durationMs", kToneSeconds * 1000.0},
                                                            {"sampleRate", kSampleRate},
                                                            {"channelCount", 2}}));
                     },
                     {}, [this](const juce::var&) { return model().findLibraryItem(itemId) != nullptr; }});
    steps.push_back({"place clip",
                     [this]
                     {
                         if (const auto* item = model().findLibraryItem(itemId)) clipId = actions.addClip(trackId, *item, 0.0);
                     },
                     {}, [this, clipCount](const juce::var&) { return clipCount() == 1 && model().findClip(clipId) != nullptr; }});
    steps.push_back({"split at 2 s", [this] { rightClipId = actions.splitClipAt(clipId, 2000.0); }, {},
                     [this, clipCount](const juce::var&)
                     {
                         const auto* left = model().findClip(clipId);
                         const auto* right = model().findClip(rightClipId);
                         return clipCount() == 2 && left != nullptr && right != nullptr
                                && std::abs(left->endMs() - 2000.0) < 1.0 && std::abs(right->offsetMs - 2000.0) < 1.0
                                && std::abs(right->inMs - 2000.0) < 1.0;
                     }});
    steps.push_back({"undo split", [this] { engine.send("EDIT_UNDO"); }, {},
                     [clipCount](const juce::var&) { return clipCount() == 1; }});
    steps.push_back({"redo split", [this] { engine.send("EDIT_REDO"); }, {},
                     [clipCount](const juce::var&) { return clipCount() == 2; }});
    steps.push_back({"move clip", [this] { actions.moveClip(rightClipId, 3000.0, trackId); }, {},
                     [this](const juce::var&)
                     {
                         const auto* right = model().findClip(rightClipId);
                         return right != nullptr && std::abs(right->offsetMs - 3000.0) < 1.0;
                     }});
    // The native import pipeline: probe, tags, key, media store, LIBRARY_ADD.
    Step importFile{"import file",
                    [this]
                    {
                        const auto file = workDir.getChildFile("Files").getChildFile("chord.wav");
                        file.getParentDirectory().createDirectory();
                        writeTone(file, 440.0);
                        importer.import(juce::Array<juce::File>{file},
                                        [this](const juce::StringArray& ids) { importedId = ids[0]; });
                    },
                    {},
                    [this](const juce::var&)
                    {
                        const auto* item = model().findLibraryItem(importedId);
                        return item != nullptr && item->filePath.endsWith("chord.wav") && item->sampleRate == kSampleRate
                               && std::abs(item->durationMs - kToneSeconds * 1000.0) < 5.0 && item->mediaId.isNotEmpty();
                    }};
    importFile.poll = true;
    steps.push_back(importFile);

    // rekordbox: a throwaway fixture library (never the installed one), read through the
    // Rust reader, and a track imported with its beat grid.
    steps.push_back({"rekordbox library",
                     [this]
                     {
                         const auto audio = workDir.getChildFile("Files").getChildFile("rekordbox track.wav");
                         writeTone(audio, 330.0);
                         // A build without cargo has no reader; the rest of the journey still runs.
                         if (!sharedRekordbox().isAvailable())
                         {
                             report.add("SKIP rekordbox: " + sharedRekordbox().unavailableReason());
                             rekordboxTrackId = "skipped";
                             return;
                         }
                         const auto fixtureDir = workDir.getChildFile("rekordbox");
                         fixtureDir.deleteRecursively();
                         const auto built = sharedRekordbox().buildFixture(fixtureDir, audio);
                         if (!static_cast<bool>(built.getProperty("ok", false)))
                         {
                             report.add("fixture: " + juce::JSON::toString(built, true) + " "
                                        + sharedRekordbox().unavailableReason());
                             return;
                         }
                         const auto snapshot = sharedRekordbox().open(juce::File(built.getProperty("masterDb", {}).toString()),
                                                                      built.getProperty("passphrase", {}).toString());
                         if (!snapshot.ok)
                         {
                             report.add("open: " + snapshot.error);
                             return;
                         }
                         rekordboxFixtureDb = juce::File(built.getProperty("masterDb", {}).toString());
                         rekordboxFixturePassphrase = built.getProperty("passphrase", {}).toString();
                         const auto* track = snapshot.findTrack(built.getProperty("trackId", {}).toString());
                         if (snapshot.tracks.size() == 40 && !snapshot.playlists.empty() && track != nullptr
                             && juce::File(track->path) == audio && std::abs(track->bpm - 128.0) < 0.01)
                             rekordboxTrackId = track->id;
                     },
                     {}, [this](const juce::var&) { return rekordboxTrackId.isNotEmpty(); }});
    Step importRekordbox{"import rekordbox track",
                         [this]
                         {
                             if (rekordboxTrackId == "skipped") return;
                             ImportRequest request;
                             request.file = workDir.getChildFile("Files").getChildFile("rekordbox track.wav");
                             request.hints.rekordboxId = rekordboxTrackId;
                             request.hints.key = "8A";
                             request.hints.bpm = 128.0;
                             importer.import(std::vector<ImportRequest>{request},
                                             [this](const juce::StringArray& ids) { rekordboxItemId = ids[0]; });
                         },
                         {},
                         [this](const juce::var&)
                         {
                             if (rekordboxTrackId == "skipped") return true;
                             const auto* item = model().findLibraryItem(rekordboxItemId);
                             return item != nullptr && std::abs(item->bpm - 128.0) < 0.01
                                    && std::abs(item->beatAnchorSec - 0.12) < 0.002 && item->key == "A minor";
                         }};
    importRekordbox.poll = true;
    steps.push_back(importRekordbox);

    // rekordbox writes: playlist edits to the fixture library and to both libraries of a
    // throwaway USB export, each database copied aside once before its first change.
    steps.push_back({"rekordbox writes",
                     [this]
                     {
                         if (rekordboxTrackId == "skipped")
                         {
                             rekordboxWritesOk = true;
                             return;
                         }
                         auto& rb = sharedRekordbox();
                         const auto backups = workDir.getChildFile("rekordbox-backups");
                         backups.deleteRecursively();
                         Rekordbox::setBackupFolderForTest(backups);
                         auto fail = [this](const juce::String& what, const juce::var& result)
                         { report.add("rekordbox writes: " + what + " " + juce::JSON::toString(result, true)); };
                         auto ok = [](const juce::var& r) { return static_cast<bool>(r.getProperty("ok", false)); };
                         auto ids = [](const juce::String& id)
                         {
                             juce::Array<juce::var> a;
                             a.add(id);
                             return juce::var(a);
                         };

                         // The fixture library is the one open (the step before imported from it).
                         const auto made = rb.write(object({{"action", "createPlaylist"}, {"name", "Smoke list"}}));
                         if (!ok(made)) return fail("create", made);
                         const auto listId = made.getProperty("id", {}).toString();
                         const auto added = rb.write(object({{"action", "addTracks"}, {"playlistId", listId}, {"trackIds", ids(rekordboxTrackId)}}));
                         if (!ok(added) || static_cast<int>(added.getProperty("changed", 0)) != 1) return fail("add", added);
                         const auto reread = rb.open(rekordboxFixtureDb, rekordboxFixturePassphrase);
                         const auto* list = reread.findPlaylist(listId);
                         if (list == nullptr || list->trackIds != juce::StringArray(rekordboxTrackId))
                             return fail("reread", juce::var(reread.error));

                         // A USB export: OneLibrary and Device Library, edited separately.
                         const auto audio = workDir.getChildFile("Files").getChildFile("rekordbox track.wav");
                         const auto stick = workDir.getChildFile("stick");
                         stick.deleteRecursively();
                         const auto built = rb.buildDeviceFixture(stick, audio);
                         if (!ok(built)) return fail("device fixture", built);
                         for (const auto* format : {"oneLibrary", "deviceLibrary"})
                         {
                             RekordboxSource source;
                             source.kind = RekordboxSource::Kind::device;
                             source.name = "Smoke stick";
                             source.root = stick;
                             source.format = format;
                             const auto device = rb.open(source);
                             if (!device.ok || device.tracks.size() != 1) return fail(juce::String("open ") + format, juce::var(device.error));
                             const auto trackId = device.tracks.front().id;
                             const auto edit = rb.write(object({{"action", "createPlaylist"}, {"name", "Stick edit"}}));
                             if (!ok(edit)) return fail(juce::String("device create ") + format, edit);
                             const auto editId = edit.getProperty("id", {}).toString();
                             const auto put = rb.write(object({{"action", "addTracks"}, {"playlistId", editId}, {"trackIds", ids(trackId)}}));
                             if (!ok(put)) return fail(juce::String("device add ") + format, put);
                             const auto again = rb.open(source);
                             const auto* edited = again.findPlaylist(editId);
                             if (edited == nullptr || edited->name != "Stick edit" || edited->trackIds.size() != 1)
                                 return fail(juce::String("device reread ") + format, juce::var(again.error));
                         }

                         // One backup per library: the fixture's master.db, and the stick's
                         // export.pdb and exportLibrary.db together — not one per write.
                         const auto taken = backups.findChildFiles(juce::File::findDirectories, false);
                         int withFiles = 0;
                         for (const auto& folder : taken)
                             if (folder.getChildFile("master.db").existsAsFile()
                                 || (folder.getChildFile("export.pdb").existsAsFile() && folder.getChildFile("exportLibrary.db").existsAsFile()))
                                 ++withFiles;
                         if (taken.size() != 2 || withFiles != 2)
                             return fail("backups", juce::var(juce::String(taken.size()) + " folders, " + juce::String(withFiles) + " with files"));
                         rekordboxWritesOk = true;
                     },
                     {}, [this](const juce::var&) { return rekordboxWritesOk; }});

    // rekordbox's sections (from the fixture track's 3-band waveform) came with its import.
    steps.push_back({"rekordbox sections", [] {}, {},
                     [this](const juce::var&)
                     {
                         if (rekordboxTrackId == "skipped") return true;
                         const auto* rb = model().findLibraryItem(rekordboxItemId);
                         if (rb == nullptr || sections::of(*rb).empty()) return false;
                         report.add("rekordbox track has " + juce::String(static_cast<int>(sections::of(*rb).size())) + " section(s)");
                         return true;
                     }});
    steps.back().poll = true;
    steps.push_back({"save as",
                     [this, projectFile]
                     {
                         engine.send("PROJECT_RENAME", object({{"name", "Smoke"}}));
                         projectFile.getParentDirectory().createDirectory();
                         engine.send("PROJECT_SAVE_AS", object({{"filePath", projectFile.getFullPathName()}}));
                     },
                     "PROJECT_SAVED", [](const juce::var& p) { return static_cast<bool>(p.getProperty("ok", false)); }});
    steps.push_back({"reopen",
                     [this, projectFile]
                     {
                         engine.send("PROJECT_NEW");
                         engine.send("PROJECT_LOAD", object({{"filePath", projectFile.getFullPathName()}}));
                     },
                     "PROJECT_STATE",
                     [this, projectFile](const juce::var& p)
                     {
                         if (p.getProperty("filePath", {}).toString() != projectFile.getFullPathName()) return false;
                         const auto reopened = ProjectModel::fromState(p);
                         const auto* track = reopened.findTrack(trackId);
                         return reopened.name == "Smoke" && track != nullptr && track->clips.size() == 2
                                && reopened.findClip(rightClipId) != nullptr
                                && std::abs(reopened.findClip(rightClipId)->offsetMs - 3000.0) < 1.0;
                     }});
    // M4: track FX, automation, crossfade on an overlapping trim, beat repeat.
    steps.push_back({"track fx",
                     [this]
                     {
                         engine.send("TRACK_SET_TONE", object({{"trackId", trackId}, {"bassDb", 4.5}}));
                         engine.send("TRACK_SET_SENDS", object({{"trackId", trackId}, {"reverbSend", 0.3}, {"delaySend", 0.0}}));
                     },
                     {},
                     [this](const juce::var&)
                     {
                         const auto* t = model().findTrack(trackId);
                         return t != nullptr && std::abs(static_cast<double>(t->raw.getProperty("toneBassDb", 0.0)) - 4.5) < 0.01
                                && std::abs(static_cast<double>(t->raw.getProperty("sendReverb", 0.0)) - 0.3) < 0.01;
                     }});
    steps.push_back({"automation",
                     [this]
                     {
                         actions.setAutomationLanes(trackId, {{"filter", 80}});
                         actions.setAutomation(trackId, "filter", {{0.0, -0.5}, {2000.0, 0.5}});
                     },
                     {},
                     [this](const juce::var&)
                     {
                         const auto* t = model().findTrack(trackId);
                         if (t == nullptr) return false;
                         const auto points = automation::pointsOf(*t, "filter");
                         return automation::lanesOf(*t).size() == 1 && points.size() == 2
                                && std::abs(automation::valueAt(points, 1000.0, 0.0)) < 0.01;
                     }});
    steps.push_back({"crossfade",
                     [this]
                     {
                         // Pull the right clip's start back over the left clip's tail.
                         const auto* right = model().findClip(rightClipId);
                         if (right == nullptr) return;
                         actions.trimClip(rightClipId, 1500.0, right->endMs());
                         mirror.refreshNow();
                         actions.maybeCreateTransitionAfterTrim(rightClipId, false);
                     },
                     {},
                     [this](const juce::var&)
                     {
                         const auto* t = model().findTrack(trackId);
                         return t != nullptr && t->transitions.size() == 1 && t->transitions[0].leftClipId == clipId
                                && t->transitions[0].rightClipId == rightClipId;
                     }});
    Step channels{"split channels",
                   [this]
                   {
                       tracksBefore = static_cast<int>(model().tracks.size());
                       stems.splitChannels(clipId, {"left", "right"});
                   },
                   {},
                   [this](const juce::var&)
                   {
                       // Two new tracks, each with a stem clip at the source clip's start.
                       if (static_cast<int>(model().tracks.size()) != tracksBefore + 2) return false;
                       int stemsFound = 0;
                       for (const auto& item : model().library) stemsFound += item.kind == "stem" ? 1 : 0;
                       return stemsFound == 2 && model().tracks.back().clips.size() == 1;
                   }};
    channels.poll = true;
    steps.push_back(channels);
    // A recording session opens (on whatever input exists, or reports none) and closes;
    // nothing is recorded.
    steps.push_back({"record session",
                     [this] { engine.send("RECORD_SESSION_OPEN", object({{"protocolVersion", 1}})); },
                     "RECORD_SESSION_STATE",
                     [this](const juce::var& p)
                     {
                         const auto id = p.getProperty("sessionId", {}).toString();
                         if (id.isEmpty()) return false;
                         report.add("record session status=" + p.getProperty("status", {}).toString());
                         engine.send("RECORD_SESSION_CLOSE", object({{"protocolVersion", 1}, {"sessionId", id}}));
                         return true;
                     }});
    // A scratch session prepares the clip as a record, then closes.
    steps.push_back({"scratch session",
                     [this] { engine.send("SCRATCH_SESSION_OPEN", object({{"protocolVersion", 1}, {"clipId", clipId}})); },
                     "SCRATCH_SESSION_STATE",
                     [this](const juce::var& p)
                     {
                         const auto status = p.getProperty("status", {}).toString();
                         // Wait for the prepared record (idle with no length is still opening).
                         if (status == "preparing" || status.isEmpty()
                             || static_cast<juce::int64>(p.getProperty("durationUs", 0)) <= 0)
                             return status == "error" ? (report.add("scratch error: " + p.getProperty("error", {}).toString()), false) : false;
                         report.add("scratch session status=" + status + " durationUs=" + p.getProperty("durationUs", 0).toString());
                         engine.send("SCRATCH_SESSION_CLOSE", object({{"protocolVersion", 1}, {"sessionId", p.getProperty("sessionId", {})}}));
                         return status != "error" && static_cast<juce::int64>(p.getProperty("durationUs", 0)) > 0;
                     }});
    steps.push_back({"copy and paste",
                     [this]
                     {
                         mirror.refreshNow();
                         actions.copyClips({clipId});
                         pastedIds = actions.paste(trackId, model().contentEndMs() + 1000.0);
                     },
                     {},
                     [this](const juce::var&)
                     {
                         if (pastedIds.size() != 1) return false;
                         const auto* pasted = model().findClip(pastedIds[0]);
                         const auto* source = model().findClip(clipId);
                         return pasted != nullptr && source != nullptr && std::abs(pasted->durationMs - source->durationMs) < 1.0
                                && pasted->libraryItemId == source->libraryItemId;
                     }});
    steps.push_back({"track height and order",
                     [this]
                     {
                         actions.setTrackHeight(trackId, 180);
                         mirror.refreshNow();
                         // The split-channel tracks sit below: move the last one to the top.
                         if (model().tracks.size() >= 2) actions.moveTrack(model().tracks.back().id, 0);
                     },
                     {},
                     [this](const juce::var&)
                     {
                         const auto* t = model().findTrack(trackId);
                         return t != nullptr && std::abs(t->heightPx - 180.0) < 0.5 && model().tracks.size() >= 2
                                && model().tracks.front().id != trackId;
                     }});
    steps.push_back({"clip envelope",
                     [this] { actions.setClipEnvelope(clipId, {{0.0, 1.0}, {1000.0, 0.5}, {2000.0, 1.0}}); },
                     {},
                     [this](const juce::var&)
                     {
                         const auto* c = model().findClip(clipId);
                         return c != nullptr && c->envelope.size() == 3 && std::abs(c->envelope[1].gain - 0.5) < 0.01;
                     }});
    steps.push_back({"warp on drop",
                     [this]
                     {
                         mirror.refreshNow();
                         // The rekordbox track is 128 BPM; the project is not (the file import
                         // stands in when the reader is not built).
                         const auto sourceId = rekordboxItemId.isNotEmpty() ? rekordboxItemId : importedId;
                         if (const auto* item = model().findLibraryItem(sourceId))
                             droppedClipId = actions.addClip(actions.addTrack(), *item, 0.0);
                     },
                     {},
                     [this](const juce::var&)
                     {
                         const auto* c = model().findClip(droppedClipId);
                         if (c == nullptr) return false;
                         // What the user hears: the engine must stretch whatever the project says is warped.
                         const bool engineWarps = engine.host().engine().clipHasWarpForTest(droppedClipId);
                         report.add("dropped clip warp=" + juce::String(c->warpEnabled ? "on" : "off") + " ratio="
                                    + juce::String(c->effectiveTempoRatio, 4) + " project=" + juce::String(model().bpm, 2)
                                    + " seeded=" + juce::String(model().bpmSeeded ? "yes" : "no")
                                    + " engine=" + juce::String(engineWarps ? "warping" : "dry"));
                         if (c->effectiveWarpActive && !engineWarps) return false;
                         return !model().bpmSeeded || std::abs(model().bpm - 128.0) < 0.01 || c->effectiveWarpActive;
                     }});
    steps.push_back({"beat repeat",
                     [this] { actions.addBeatRepeat(trackId, 0.0, 2.0, "1/8"); },
                     {},
                     [this](const juce::var&)
                     {
                         const auto* t = model().findTrack(trackId);
                         const auto* regions = t != nullptr ? t->raw.getProperty("beatRepeats", {}).getArray() : nullptr;
                         return regions != nullptr && regions->size() == 1;
                     }});

    // Autosave + crash recovery through the engine, and an offline mixdown.
    const auto autosaveFile = workDir.getChildFile("autosave").getChildFile("autosave.stemcel");
    steps.push_back({"autosave",
                     [this, autosaveFile]
                     {
                         autosaveFile.getParentDirectory().createDirectory();
                         actions.moveClip(rightClipId, 3500.0, trackId); // something unsaved
                         engine.send("PROJECT_AUTOSAVE", object({{"filePath", autosaveFile.getFullPathName()}}));
                     },
                     "PROJECT_AUTOSAVED",
                     [autosaveFile](const juce::var& p)
                     { return static_cast<bool>(p.getProperty("ok", false)) && autosaveFile.existsAsFile(); }});
    // Bucket ids must match the renderer's (SHA-1 of the lower-cased path, 8 bytes).
    steps.push_back({"autosave id", [] {}, {},
                     [](const juce::var&) { return Autosave::projectIdForPath("  ABC ") == "a9993e364706816a"; }});
    steps.push_back({"recover autosave",
                     [this, autosaveFile, projectFile]
                     {
                         engine.send("PROJECT_NEW");
                         engine.send("PROJECT_LOAD_RECOVERY", object({{"autosavePath", autosaveFile.getFullPathName()},
                                                                      {"originalPath", projectFile.getFullPathName()}}));
                     },
                     "PROJECT_STATE",
                     [this, projectFile](const juce::var& p)
                     {
                         const auto recovered = ProjectModel::fromState(p);
                         const auto* clip = recovered.findClip(rightClipId);
                         return recovered.filePath == projectFile.getFullPathName() && clip != nullptr
                                && std::abs(clip->offsetMs - 3500.0) < 1.0;
                     }});
    // The tracklist's reading of file names that carry no tags.
    steps.push_back({"tracklist names",
                     [] {},
                     {},
                     [this](const juce::var&)
                     {
                         const std::vector<std::array<const char*, 3>> cases{
                             {"01 - Daft Punk - One More Time.mp3", "Daft Punk", "One More Time"},
                             {"Fred again.._-_Delilah (pull me out of this) [Official Video].wav", "Fred again..", "Delilah (pull me out of this)"},
                             {"A1 Bicep - Glue (Extended Mix) (320kbps).flac", "Bicep", "Glue (Extended Mix)"},
                             {"Skrillex – Rumble (feat. Fred again..) [HQ].mp3", "Skrillex feat. Fred again..", "Rumble"},
                             {"Kölsch - Grey (Original Mix) [KOMPAKT123].aiff", "Kölsch", "Grey (Original Mix)"},
                             {"Strobe (Club Edit) - deadmau5.mp3", "deadmau5", "Strobe (Club Edit)"},
                             {"Losing It by Fisher.mp3", "Fisher", "Losing It"},
                             {"Stand by Me.mp3", "", "Stand by Me"},
                             {"50 Cent - In Da Club (Lyrics).mp3", "50 Cent", "In Da Club"},
                             {"track_07_final_master.wav", "", "track 07 final master"},
                             {"Title ft. Someone - www.musicsite.com.mp3", "", ""},
                         };
                         juce::StringArray wrong;
                         for (const auto& [name, artist, title] : cases)
                         {
                             const auto g = tracklist::fromFileName(juce::String::fromUTF8(name));
                             // The last case only has to come out without the site.
                             if (juce::String(title).isEmpty())
                             {
                                 if ((g.artist + g.title).containsIgnoreCase("musicsite")) wrong.add(juce::String::fromUTF8(name));
                                 continue;
                             }
                             if (g.artist != juce::String::fromUTF8(artist) || g.title != juce::String::fromUTF8(title))
                                 wrong.add(juce::String::fromUTF8(name) + " -> [" + g.artist + "] [" + g.title + "]");
                         }
                         if (!wrong.isEmpty()) lastTempoLine = "tracklist: " + wrong.joinIntoString("; ");
                         else report.add("tracklist names: " + juce::String(static_cast<int>(cases.size())) + " read");
                         return wrong.isEmpty();
                     }});
    steps.push_back({"export mixdown",
                     [this]
                     {
                         mirror.refreshNow();
                         expectedMixSeconds = model().contentEndMs() / 1000.0;
                         engine.send("MIXDOWN_START", object({{"outputPath", workDir.getChildFile("mixdown.wav").getFullPathName()},
                                                              {"format", "wav"},
                                                              {"sampleRate", 44100},
                                                              {"bitDepth", 24},
                                                              {"lengthMode", "trim-to-last-clip"}}));
                     },
                     "MIXDOWN_DONE",
                     [this](const juce::var&)
                     {
                         juce::AudioFormatManager formats;
                         formats.registerBasicFormats();
                         std::unique_ptr<juce::AudioFormatReader> reader(
                             formats.createReaderFor(workDir.getChildFile("mixdown.wav")));
                         // To the end of the last clip, plus any effect tail (the room send set above).
                         if (reader == nullptr || expectedMixSeconds <= 1.0) return false;
                         const double seconds = static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
                         return seconds >= expectedMixSeconds - 0.01 && seconds < expectedMixSeconds + 0.5;
                     }});

    // Half a second of silence before the first sample, and the tracklist's times after it.
    steps.push_back({"export with lead-in",
                     [this]
                     {
                         mirror.refreshNow();
                         expectedMixSeconds = model().contentEndMs() / 1000.0;
                         engine.send("MIXDOWN_START", object({{"outputPath", workDir.getChildFile("mixdown-leadin.wav").getFullPathName()},
                                                              {"format", "wav"},
                                                              {"sampleRate", 44100},
                                                              {"bitDepth", 24},
                                                              {"leadInMs", 500.0},
                                                              {"lengthMode", "trim-to-last-clip"}}));
                     },
                     "MIXDOWN_DONE",
                     [this](const juce::var&)
                     {
                         juce::AudioFormatManager formats;
                         formats.registerBasicFormats();
                         std::unique_ptr<juce::AudioFormatReader> reader(
                             formats.createReaderFor(workDir.getChildFile("mixdown-leadin.wav")));
                         if (reader == nullptr) return false;
                         const double seconds = static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
                         const bool lengthOk = seconds >= expectedMixSeconds + 0.49 && seconds < expectedMixSeconds + 1.0;
                         juce::AudioBuffer<float> head(2, 22050);
                         reader->read(&head, 0, 22050, 0, true, true);
                         const bool silent = head.getMagnitude(0, 22050) == 0.0f;
                         const auto entries = tracklist::fromProject(model(), 0.0, model().contentEndMs(), 500.0);
                         const auto text = tracklist::toText(entries);
                         const bool listOk = !entries.empty() && entries.front().atMs >= 500.0 && text.startsWith("00:00  ");
                         lastTempoLine = "lead-in " + juce::String(seconds - expectedMixSeconds, 3) + " s extra, silent " +
                                         (silent ? "yes" : "no") + "; tracklist " + text.replace("\n", " | ");
                         if (lengthOk && silent && listOk) report.add(lastTempoLine);
                         return lengthOk && silent && listOk;
                     }});

    // Save what the M4 steps did, so the workspace snapshot shows lanes and regions.
    steps.push_back({"save", [this] { engine.send("PROJECT_SAVE"); }, "PROJECT_SAVED",
                     [](const juce::var& p) { return static_cast<bool>(p.getProperty("ok", false)); }});
    // The Clip Editor on the right clip, drawn offscreen (waveforms need a moment to load).
    Step editorStep{"clip editor",
                    [this]
                    {
                        editorWaveforms = std::make_unique<WaveformStore>(engine.host().peaksCache());
                        editorView = std::make_unique<ClipEditor>(engine, mirror, actions, *editorWaveforms, clipId);
                        // Load peaks now; the snapshot is taken on the next step.
                        editorView->setSize(1100, 640);
                        editorView->createComponentSnapshot(editorView->getLocalBounds());
                    },
                    {},
                    [this](const juce::var&) { return editorView != nullptr; }};
    steps.push_back(editorStep);
    Step editorShot{"clip editor snapshot",
                    [this]
                    {
                        const auto image = editorView->createComponentSnapshot(editorView->getLocalBounds());
                        juce::FileOutputStream out(workDir.getChildFile("clip-editor.png"));
                        if (out.openedOk()) juce::PNGImageFormat().writeImageToStream(image, out);
                        editorView.reset();
                        editorWaveforms.reset();
                    },
                    {},
                    [this](const juce::var&) { return workDir.getChildFile("clip-editor.png").existsAsFile(); }};
    editorShot.delayMs = 1500.0;
    steps.push_back(editorShot);

    Step scratchOpen{"scratch editor",
                     [this]
                     {
                         editorWaveforms = std::make_unique<WaveformStore>(engine.host().peaksCache());
                         editorView = std::make_unique<ScratchEditor>(engine, mirror, *editorWaveforms, clipId, juce::String(), nullptr);
                         editorView->setSize(760, 640);
                         editorView->createComponentSnapshot(editorView->getLocalBounds()); // starts the peaks load
                     },
                     {},
                     [this](const juce::var&) { return editorView != nullptr; }};
    steps.push_back(scratchOpen);
    Step scratchShot{"scratch editor snapshot",
                     [this]
                     {
                         const auto image = editorView->createComponentSnapshot(editorView->getLocalBounds());
                         juce::FileOutputStream out(workDir.getChildFile("scratch-editor.png"));
                         if (out.openedOk()) juce::PNGImageFormat().writeImageToStream(image, out);
                         editorView.reset();
                         editorWaveforms.reset();
                     },
                     {},
                     [this](const juce::var&) { return workDir.getChildFile("scratch-editor.png").existsAsFile(); }};
    scratchShot.delayMs = 2500.0;
    steps.push_back(scratchShot);

    steps.push_back({"open in workspace",
                     [this, projectFile]
                     {
                         if (openInUi) openInUi(projectFile);
                     },
                     "PROJECT_STATE", [](const juce::var& p) { return static_cast<bool>(p.getProperty("reset", false)); }});
    // A picture of the arrange view, for eyes (CI uploads it with the results).
    Step snapshot{"snapshot",
                  [this]
                  {
                      if (auto* window = juce::TopLevelWindow::getTopLevelWindow(0))
                      {
                          const auto image = window->createComponentSnapshot(window->getLocalBounds());
                          juce::FileOutputStream out(workDir.getChildFile("arrange.png"));
                          if (out.openedOk()) juce::PNGImageFormat().writeImageToStream(image, out);
                      }
                  },
                  {}, [this](const juce::var&) { return workDir.getChildFile("arrange.png").existsAsFile(); }};
    snapshot.delayMs = 2500.0;
    steps.push_back(snapshot);
    // The seamless save, on the project the app has open ("open in workspace" opened it as
    // File > Open does).
    steps.push_back({"seamless save",
                     [this, projectFile]
                     {
                         // An edit to the saved project, then the seamless save (two seconds here,
                         // three minutes in use) writes it to its file with no prompt or message.
                         seamlessSavedBefore = projectFile.getLastModificationTime();
                         if (auto* strip = findDescendant<ProjectTabStrip>(juce::TopLevelWindow::getTopLevelWindow(0)))
                             strip->controller.setSeamlessSaveIntervalForTest(2);
                         engine.send("PROJECT_RENAME", object({{"name", "Smoke Seamless"}}));
                     },
                     {},
                     [this, projectFile](const juce::var&)
                     {
                         if (model().dirty || model().name != "Smoke Seamless") return false;
                         if (projectFile.getLastModificationTime() <= seamlessSavedBefore) return false;
                         if (!projectFile.loadFileAsString().contains("Smoke Seamless")) return false;
                         if (auto* strip = findDescendant<ProjectTabStrip>(juce::TopLevelWindow::getTopLevelWindow(0)))
                             strip->controller.setSeamlessSaveIntervalForTest(AppController::kSeamlessSaveSeconds);
                         engine.send("PROJECT_RENAME", object({{"name", "Smoke"}}));
                         report.add("seamless save wrote " + projectFile.getFileName() + " unprompted");
                         return true;
                     }});
    steps.back().poll = true;
    steps.push_back({"save as again",
                     [this, projectFile] { engine.send("PROJECT_SAVE_AS", object({{"filePath", projectFile.getFullPathName()}})); },
                     "PROJECT_SAVED", [](const juce::var& p) { return static_cast<bool>(p.getProperty("ok", false)); }});

    addTempoJourney();
}

// Two tracks at different tempos, dropped the way a user does: straight after import,
// before analysis lands, then again once it has, then warped by hand as the Clip Editor
// saves it. Each check reads the live engine's stretcher, so "looks warped but plays dry"
// fails here rather than in someone's ears.
void SmokeTest::addTempoJourney()
{
    // The clip's expected ratio (project/source) and what the engine is actually running.
    auto warpCheck = [this](const juce::String& id, juce::String& line) -> bool
    {
        const auto* c = model().findClip(id);
        if (c == nullptr) return false;
        const auto* item = model().findLibraryItem(c->libraryItemId);
        const double sourceBpm = item != nullptr ? item->bpm : 0.0;
        const double expected = sourceBpm > 0.0 ? model().bpm / sourceBpm : 0.0;
        const double live = engine.host().engine().clipWarpRatioForTest(id);
        line = "clip src=" + juce::String(sourceBpm, 2) + " project=" + juce::String(model().bpm, 2) + " expected="
               + juce::String(expected, 4) + " state=" + juce::String(c->effectiveWarpActive ? "warped" : "dry") + "@"
               + juce::String(c->effectiveTempoRatio, 4) + " engine=" + juce::String(live, 4)
               + (c->pendingAutoWarp ? " pending" : "");
        // The state, the drawing and the sound must agree.
        return expected > 0.0 && c->effectiveWarpActive && std::abs(c->effectiveTempoRatio - expected) < 0.01
               && std::abs(live - expected) < 0.01;
    };

    steps.push_back({"tempo journey: new project", [this] { engine.send("PROJECT_NEW"); }, "PROJECT_STATE",
                     [](const juce::var& p) { return static_cast<bool>(p.getProperty("reset", false)); }});

    Step dropEarly{"tempo journey: drop two tracks before analysis",
                   [this]
                   {
                       const auto dir = workDir.getChildFile("Tempo");
                       dir.createDirectory();
                       const auto slow = dir.getChildFile("clicks 100.wav");
                       const auto fast = dir.getChildFile("clicks 125.wav");
                       writeClicks(slow, 100.0);
                       writeClicks(fast, 125.0);
                       importer.import(juce::Array<juce::File>{slow, fast},
                                       [this](const juce::StringArray& ids)
                                       {
                                           tempoItemIds = ids;
                                           mirror.refreshNow();
                                           for (const auto& id : ids)
                                               if (const auto* item = model().findLibraryItem(id))
                                               {
                                                   report.add("dropped " + item->fileName + " bpm at drop="
                                                              + juce::String(item->bpm, 2));
                                                   tempoClipIds.add(actions.addClip(actions.addTrack(), *item, 0.0));
                                                   mirror.refreshNow();
                                               }
                                       });
                   },
                   {},
                   [this, warpCheck](const juce::var&)
                   {
                       if (tempoClipIds.size() != 2 || !model().bpmSeeded) return false;
                       // Whichever analysis lands first seeds the project tempo (the order varies by
                       // machine): that clip plays as it is, and the other is stretched to it.
                       tempoSeed = -1;
                       for (int i = 0; i < 2; ++i)
                           if (const auto* c = model().findClip(tempoClipIds[i]))
                               if (const auto* item = model().findLibraryItem(c->libraryItemId); item != nullptr && std::abs(item->bpm - model().bpm) < 0.01)
                                   tempoSeed = i;
                       if (tempoSeed < 0) return false;
                       const auto& seedId = tempoClipIds[tempoSeed];
                       const auto* seeded = model().findClip(seedId);
                       if (seeded == nullptr || seeded->pendingAutoWarp || seeded->effectiveWarpActive
                           || engine.host().engine().clipWarpRatioForTest(seedId) > 1.0001)
                       {
                           lastTempoLine = "seeding clip still pending or warped";
                           return false;
                       }
                       juce::String line;
                       const bool ok = warpCheck(tempoClipIds[1 - tempoSeed], line);
                       lastTempoLine = "other " + line;
                       return ok;
                   }};
    dropEarly.poll = true;
    steps.push_back(dropEarly);

    Step dropLate{"tempo journey: drop again after analysis",
                  [this]
                  {
                      report.add(lastTempoLine);
                      // The track that did not seed the tempo, so it has something to stretch to.
                      if (const auto* item = model().findLibraryItem(tempoItemIds[1 - tempoSeed]))
                          tempoLateClipId = actions.addClip(actions.addTrack(), *item, 0.0);
                  },
                  {},
                  [this, warpCheck](const juce::var&)
                  {
                      juce::String line;
                      const bool ok = warpCheck(tempoLateClipId, line);
                      lastTempoLine = "late " + line;
                      return ok;
                  }};
    dropLate.poll = true;
    steps.push_back(dropLate);

    Step editorWarp{"tempo journey: warp from the clip editor",
                    [this]
                    {
                        report.add(lastTempoLine);
                        // Unwarp, then save "Warp to project tempo" exactly as ClipEditor::save sends it.
                        engine.send("CLIP_SET_WARP", object({{"clipId", tempoLateClipId}, {"warpEnabled", false}}));
                        engine.send("CLIP_SET_WARP", object({{"clipId", tempoLateClipId},
                                                            {"warpEnabled", true},
                                                            {"warpMode", "rhythmic"},
                                                            {"tempoRatio", juce::var()},
                                                            {"semitones", 0},
                                                            {"cents", 0}}));
                    },
                    {},
                    [this, warpCheck](const juce::var&)
                    {
                        juce::String line;
                        const bool ok = warpCheck(tempoLateClipId, line);
                        lastTempoLine = "editor " + line;
                        if (ok) report.add(lastTempoLine);
                        return ok;
                    }};
    editorWarp.poll = true;
    steps.push_back(editorWarp);

    // The arrange view with the warped clips, so their header badges can be looked at.
    Step tempoShot{"tempo journey: snapshot",
                   [this]
                   {
                       if (auto* window = juce::TopLevelWindow::getTopLevelWindow(0))
                       {
                           const auto image = window->createComponentSnapshot(window->getLocalBounds());
                           juce::FileOutputStream out(workDir.getChildFile("tempo.png"));
                           if (out.openedOk()) juce::PNGImageFormat().writeImageToStream(image, out);
                       }
                   },
                   {}, [this](const juce::var&) { return workDir.getChildFile("tempo.png").existsAsFile(); }};
    tempoShot.delayMs = 2500.0;
    steps.push_back(tempoShot);

    // Controller input as the engine maps it (MIDI_CONTROL), through the same actions the
    // workspace runs: the marker pad, the master dial (eased in by soft takeover) and the
    // browse encoder walking the track selection.
    Step midi{"midi controller actions",
              [this]
              {
                  midiLive = std::make_unique<LiveState>(engine);
                  midiActions = std::make_unique<MidiControllerActions>(engine, mirror, actions, midiView, *midiLive, prefs,
                                                                        MidiControllerActions::Hooks{});
                  midiView.selectedTrackId = model().tracks.empty() ? juce::String() : model().tracks.front().id;
                  const auto control = [](std::initializer_list<std::pair<const char*, juce::var>> props)
                  {
                      auto p = object(props);
                      p.getDynamicObject()->setProperty("deviceIdentifier", "smoke-controller");
                      return p;
                  };
                  midiActions->handleControl(control({{"kind", "button"}, {"control", "markerToggle"}, {"deck", 1}, {"pressed", true}}));
                  midiActions->handleControl(control({{"kind", "absolute"}, {"control", "masterVolume"}, {"deck", juce::var()}, {"value", 0.5}}));
                  midiActions->handleControl(control({{"kind", "relative"}, {"control", "browseTracks"}, {"deck", juce::var()}, {"value", 1}}));
              },
              {},
              [this](const juce::var&)
              {
                  const double expected = db::positionToLinear(0.5, db::kMaxMasterDb);
                  const bool marker = std::any_of(model().markers.begin(), model().markers.end(),
                                                  [](const MarkerModel& m) { return std::abs(m.positionMs) < 1.0; });
                  const bool master = std::abs(model().masterVolume - expected) < 1e-3;
                  const bool browsed = model().tracks.size() > 1 && midiView.selectedTrackId == model().tracks[1].id;
                  lastTempoLine = "midi marker=" + juce::String(marker ? "yes" : "no") + " master=" + juce::String(model().masterVolume, 4)
                                  + " (want " + juce::String(expected, 4) + ") browsed=" + juce::String(browsed ? "yes" : "no");
                  if (marker && master && browsed) report.add(lastTempoLine);
                  return marker && master && browsed;
              }};
    midi.poll = true;
    steps.push_back(midi);

    // Align to the beat grid: a 100 BPM clip dropped 130 ms off the 100 BPM project grid
    // moves so its first beat sits on a project beat.
    steps.push_back({"align clip to beat grid",
                     [this]
                     {
                         // Each change rebuilds the mirror, so the item is looked up again after it.
                         const auto itemId = tempoItemIds[tempoSeed]; // at the project tempo, so alignable
                         const auto trackId = actions.addTrack();
                         mirror.refreshNow();
                         if (const auto* item = model().findLibraryItem(itemId))
                             alignClipId = actions.addClip(trackId, *item, 60130.0);
                         mirror.refreshNow();
                         actions.alignItemClipsToGrid(itemId);
                     },
                     {},
                     [this](const juce::var&)
                     {
                         const auto* c = model().findClip(alignClipId);
                         const auto* item = c != nullptr ? model().findLibraryItem(c->libraryItemId) : nullptr;
                         if (c == nullptr || item == nullptr || item->bpm <= 0.0) return false;
                         const double beatMs = 60000.0 / model().bpm;
                         const double spacing = 60000.0 / item->bpm;
                         const double anchor = item->beatAnchorSec * 1000.0;
                         const double firstBeatSource = anchor + std::ceil((c->inMs - anchor) / spacing - 1e-6) * spacing;
                         const double firstBeat = c->offsetMs + (firstBeatSource - c->inMs);
                         const double phase = std::fmod(firstBeat, beatMs);
                         lastTempoLine = "aligned clip at " + juce::String(c->offsetMs, 1) + " ms, first beat " + juce::String(firstBeat, 1)
                                         + " ms, off-grid by " + juce::String(juce::jmin(phase, beatMs - phase), 2) + " ms";
                         report.add(lastTempoLine);
                         return std::abs(c->offsetMs - 60130.0) > 1.0 && juce::jmin(phase, beatMs - phase) < 1.0;
                     }});

    // The new Project and Timeline preference pages, for eyes.
    steps.push_back({"preference pages snapshot",
                     [this]
                     {
                         for (auto [file, page] : {std::pair<const char*, juce::Component*>{"prefs-project.png", new ProjectPreferencesPage(prefs)},
                                                   std::pair<const char*, juce::Component*>{"prefs-timeline.png", new TimelinePreferencesPage(prefs)}})
                         {
                             std::unique_ptr<juce::Component> owned(page);
                             owned->setSize(620, 420);
                             const auto image = owned->createComponentSnapshot(owned->getLocalBounds());
                             juce::FileOutputStream out(workDir.getChildFile(file));
                             if (out.openedOk()) juce::PNGImageFormat().writeImageToStream(image, out);
                         }
                     },
                     {},
                     [this](const juce::var&) { return workDir.getChildFile("prefs-project.png").existsAsFile(); }});

    // The Clip Editor on a library item: a selection saved as a new clip, then that saved
    // clip edited from the library; and a volume shape saved from the timeline editor.
    steps.push_back({"library clip editor",
                     [this]
                     {
                         editorWaveforms = std::make_unique<WaveformStore>(engine.host().peaksCache());
                         {
                             ClipEditor source(engine, mirror, actions, *editorWaveforms, {}, tempoItemIds[1]);
                             source.selInMs = 1000.0;
                             source.selDurMs = 4000.0;
                             source.saveAsNewClip();
                         }
                         mirror.refreshNow();
                         for (const auto& item : model().library)
                             if (item.kind == "clip" && item.sourceItemId == tempoItemIds[1]
                                 && std::abs(static_cast<double>(item.raw.getProperty("sourceInMs", -1.0)) - 1000.0) < 0.5)
                                 savedClipItemId = item.id;
                         if (savedClipItemId.isEmpty()) return;
                         ClipEditor saved(engine, mirror, actions, *editorWaveforms, {}, savedClipItemId);
                         saved.draft.semitones = 2;
                         saved.saveLibraryClip();
                     },
                     {},
                     [this](const juce::var&)
                     {
                         const auto* item = model().findLibraryItem(savedClipItemId);
                         lastTempoLine = "saved clip " + savedClipItemId + " in="
                                         + (item != nullptr ? item->raw.getProperty("sourceInMs", {}).toString() : juce::String("?")) + " dur="
                                         + (item != nullptr ? item->raw.getProperty("sourceDurationMs", {}).toString() : juce::String("?"))
                                         + " semitones=" + (item != nullptr ? item->raw.getProperty("semitones", {}).toString() : juce::String("?"))
                                         + " key=" + (item != nullptr ? item->key : juce::String("?"));
                         report.add(lastTempoLine);
                         // Pitched up two semitones from C minor, the saved clip is in D minor.
                         return item != nullptr && std::abs(static_cast<double>(item->raw.getProperty("sourceDurationMs", 0.0)) - 4000.0) < 0.5
                                && static_cast<int>(item->raw.getProperty("semitones", 0)) == 2 && item->key == "D minor";
                     }});
    steps.push_back({"clip editor volume shape",
                     [this]
                     {
                         ClipEditor editor(engine, mirror, actions, *editorWaveforms, tempoLateClipId);
                         editor.ensureEnvelope();
                         const double end = editor.envelopeDurationMs();
                         editor.draft.envelope.insert(editor.draft.envelope.begin() + 1, EnvelopePoint{end / 2.0, 0.25});
                         editor.save();
                         editorWaveforms.reset();
                     },
                     {},
                     [this](const juce::var&)
                     {
                         const auto* c = model().findClip(tempoLateClipId);
                         lastTempoLine = "volume shape points=" + juce::String(c != nullptr ? static_cast<int>(c->envelope.size()) : -1);
                         report.add(lastTempoLine);
                         return c != nullptr && c->envelope.size() == 3 && std::abs(c->envelope[1].gain - 0.25) < 0.01;
                     }});

    // The arrange view and library tiles at the end of the journey, for eyes.
    Step finalShot{"final snapshot",
                   [this]
                   {
                       if (auto* window = juce::TopLevelWindow::getTopLevelWindow(0))
                       {
                           const auto image = window->createComponentSnapshot(window->getLocalBounds());
                           juce::FileOutputStream out(workDir.getChildFile("final.png"));
                           if (out.openedOk()) juce::PNGImageFormat().writeImageToStream(image, out);
                       }
                   },
                   {}, [this](const juce::var&) { return workDir.getChildFile("final.png").existsAsFile(); }};
    finalShot.delayMs = 1500.0;
    steps.push_back(finalShot);

    // The Record Audio review of a take (the smoke click track standing in for a recording).
    Step reviewShot{"record review snapshot",
                    [this]
                    {
                        reviewDialog = std::make_unique<RecordDialog>(engine, mirror, actions, midiView, prefs);
                        auto* take = new juce::DynamicObject();
                        take->setProperty("recordingId", "smoke-take");
                        take->setProperty("filePath", workDir.getChildFile("Tempo").getChildFile("clicks 100.wav").getFullPathName());
                        take->setProperty("suggestedName", "Recording 1");
                        take->setProperty("durationMs", 30000.0);
                        take->setProperty("sampleRate", kSampleRate);
                        take->setProperty("channelCount", 2);
                        take->setProperty("bpm", 100.0);
                        take->setProperty("anchorMs", 0.0);
                        take->setProperty("preRollMs", 0.0);
                        reviewDialog->ready = juce::var(take);
                        reviewDialog->takeName.setText("Recording 1", false);
                        reviewDialog->updateEnablement();
                        reviewDialog->createComponentSnapshot(reviewDialog->getLocalBounds()); // starts the peaks load
                    },
                    {},
                    [this](const juce::var&) { return reviewDialog != nullptr; }};
    steps.push_back(reviewShot);
    Step reviewShot2{"record review snapshot (peaks)",
                     [this]
                     {
                         const auto image = reviewDialog->createComponentSnapshot(reviewDialog->getLocalBounds());
                         juce::FileOutputStream out(workDir.getChildFile("record-review.png"));
                         if (out.openedOk()) juce::PNGImageFormat().writeImageToStream(image, out);
                         reviewDialog->ready = juce::var(); // nothing to release: it never played
                         reviewDialog.reset();
                     },
                     {},
                     [this](const juce::var&) { return workDir.getChildFile("record-review.png").existsAsFile(); }};
    reviewShot2.delayMs = 1500.0;
    steps.push_back(reviewShot2);

    // UVR's models through audio-separator, when asked for (STEMCEL_SMOKE_UVR=1 with the
    // tool on PATH): BS-Roformer SW on the tone, keeping two of its six stems.
    Step uvrStep{"uvr separation",
                 [this]
                 {
                     if (!juce::SystemStats::getEnvironmentVariable("STEMCEL_SMOKE_UVR", {}).isNotEmpty()
                         || !uvr::findExecutable(prefs).existsAsFile())
                     {
                         report.add("SKIP uvr separation (set STEMCEL_SMOKE_UVR=1 with audio-separator on PATH)");
                         uvrSkipped = true;
                         return;
                     }
                     stems.onNotify = [this](const juce::String& m, bool) { report.add("uvr: " + m); };
                     // A timeline clip: its window goes to a temporary file, and the stems land on new tracks.
                     const auto* c = model().findClip(tempoLateClipId);
                     if (c == nullptr) return;
                     uvrItemId = c->libraryItemId;
                     uvrTracksBefore = static_cast<int>(model().tracks.size());
                     stems.startUvr(uvrItemId, tempoLateClipId, "six", {}, {"drums", "bass"});
                 },
                 {},
                 [this](const juce::var&)
                 {
                     if (uvrSkipped) return true;
                     if (stems.busy()) return false;
                     juce::StringArray found;
                     juce::File folder;
                     for (const auto& item : model().library)
                         if (item.kind == "stem" && item.sourceItemId == uvrItemId && (item.name.startsWith("Drums") || item.name.startsWith("Bass")))
                         {
                             found.add(item.name.upToFirstOccurrenceOf(" ", false, false));
                             folder = juce::File(item.filePath).getParentDirectory();
                         }
                     const int files = folder.isDirectory() ? folder.getNumberOfChildFiles(juce::File::findFiles, "*.wav") : -1;
                     lastTempoLine = "uvr stems=" + found.joinIntoString(",") + " files in folder=" + juce::String(files);
                     const bool placed = static_cast<int>(model().tracks.size()) == uvrTracksBefore + 2;
                     lastTempoLine << " placed=" << (placed ? "yes" : "no");
                     const bool ok = found.size() == 2 && files == 2 && placed;
                     if (ok) report.add(lastTempoLine);
                     return ok;
                 }};
    uvrStep.poll = true;
    uvrStep.timeoutMs = 600000.0;
    steps.push_back(uvrStep);

    // The best-of-both recipe: SW's bass, and vocals from Mel-Band RoFormer run on the same
    // clip window afterwards (SW's vocals are dropped).
    Step bestStep{"uvr best of both",
                  [this]
                  {
                      if (uvrSkipped) return;
                      uvrTracksBefore = static_cast<int>(model().tracks.size());
                      uvrLibraryBefore.clear();
                      for (const auto& item : model().library) uvrLibraryBefore.add(item.id);
                      stems.startUvr(uvrItemId, tempoLateClipId, "best", {}, {"vocals", "bass"});
                  },
                  {},
                  [this](const juce::var&)
                  {
                      if (uvrSkipped) return true;
                      if (stems.busy()) return false;
                      juce::StringArray found;
                      bool vocalPass = false;
                      for (const auto& item : model().library)
                      {
                          if (item.kind != "stem" || item.sourceItemId != uvrItemId || uvrLibraryBefore.contains(item.id)) continue;
                          found.add(item.name.upToFirstOccurrenceOf(" ", false, false));
                          // The vocal model's run writes into its own "-vocals" folder.
                          if (item.name.startsWith("Vocals"))
                              vocalPass = juce::File(item.filePath).getParentDirectory().getFileName().endsWith("-vocals");
                      }
                      found.sort(true);
                      const bool placed = static_cast<int>(model().tracks.size()) == uvrTracksBefore + 2;
                      lastTempoLine = "uvr best stems=" + found.joinIntoString(",") + " vocals from Mel-Band RoFormer="
                                      + (vocalPass ? "yes" : "no") + " placed=" + (placed ? "yes" : "no");
                      const bool ok = found.joinIntoString(",") == "Bass,Vocals" && vocalPass && placed;
                      if (ok) report.add(lastTempoLine);
                      return ok;
                  }};
    bestStep.poll = true;
    bestStep.timeoutMs = 900000.0;
    steps.push_back(bestStep);

    // The Separate Stems dialog, for eyes.
    steps.push_back({"stems dialog snapshot",
                     [this]
                     {
                         auto dialog = stems.makeDialog(tempoItemIds[0], {});
                         if (dialog == nullptr) return;
                         const auto image = dialog->createComponentSnapshot(dialog->getLocalBounds());
                         juce::FileOutputStream out(workDir.getChildFile("stems-dialog.png"));
                         if (out.openedOk()) juce::PNGImageFormat().writeImageToStream(image, out);
                     },
                     {},
                     [this](const juce::var&) { return workDir.getChildFile("stems-dialog.png").existsAsFile(); }});

    // The in-place renames (the renderer's inline fields): a clip's title strip, a track's
    // name (opened by clicking it) and a library tile. The tile's upsert must keep what
    // the item was analysed and derived from.
    steps.push_back({"inline renames",
                     [this]
                     {
                         auto* window = juce::TopLevelWindow::getTopLevelWindow(0);
                         juce::StringArray typed;
                         if (auto* timeline = findDescendant<TimelineView>(window))
                         {
                             timeline->startClipRename(tempoLateClipId);
                             if (typeIntoInlineEditor(timeline, "Renamed clip")) typed.add("clip");
                         }
                         // A track's header row (not the master's, which keeps its name).
                         TrackHeaderRow* row = nullptr;
                         if (auto* headers = findDescendant<TrackHeaders>(window))
                             for (auto* child : headers->getChildren())
                                 if (auto* candidate = dynamic_cast<TrackHeaderRow*>(child); candidate != nullptr && !candidate->isMaster() && row == nullptr)
                                     row = candidate;
                         if (row != nullptr)
                             if (auto* label = findDescendant<juce::Label>(row))
                             {
                                 const auto at = label->getBounds().getCentre().toFloat();
                                 const auto now = juce::Time::getCurrentTime();
                                 row->mouseDown(juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), at, {}, 1.0f, 0.0f, 0.0f,
                                                                 0.0f, 0.0f, row, row, now, at, now, 1, false));
                                 if (typeIntoInlineEditor(label, "Renamed track")) typed.add("track");
                             }
                         if (auto* library = findDescendant<LibraryPanel>(window))
                         {
                             if (const auto* item = model().findLibraryItem(tempoItemIds[0]))
                             {
                                 renameKeyBefore = item->key;
                                 renameFileBefore = item->filePath;
                             }
                             library->renameItem(tempoItemIds[0]);
                             if (typeIntoInlineEditor(library, "Renamed item")) typed.add("item");
                         }
                         lastTempoLine = "inline renames typed=" + typed.joinIntoString(",");
                     },
                     {},
                     [this](const juce::var&)
                     {
                         const auto* clip = model().findClip(tempoLateClipId);
                         const auto* item = model().findLibraryItem(tempoItemIds[0]);
                         bool track = false;
                         for (const auto& t : model().tracks) track = track || t.name == "Renamed track";
                         const bool clipOk = clip != nullptr && clip->name == "Renamed clip";
                         const bool itemOk = item != nullptr && item->name == "Renamed item" && item->key == renameKeyBefore
                                             && item->filePath == renameFileBefore;
                         const auto line = lastTempoLine + " clip=" + (clipOk ? "yes" : "no") + " track=" + (track ? "yes" : "no")
                                           + " item=" + (itemOk ? "yes" : "no");
                         if (clipOk && track && itemOk) report.add(line);
                         else lastTempoLine = line;
                         return clipOk && track && itemOk;
                     }});
    steps.back().poll = true;

    // The export dialog as the project left it, for eyes.
    steps.push_back({"export dialog snapshot",
                     [this]
                     {
                         ExportDialog dialog(engine, prefs, model(), {});
                         const auto image = dialog.createComponentSnapshot(dialog.getLocalBounds());
                         juce::FileOutputStream out(workDir.getChildFile("export-dialog.png"));
                         if (out.openedOk()) juce::PNGImageFormat().writeImageToStream(image, out);
                     },
                     {},
                     [this](const juce::var&) { return workDir.getChildFile("export-dialog.png").existsAsFile(); }});

    // The tempo map: a 20% ramp over the first eight seconds. The warped clip follows it in
    // the model and the engine, and lasts as long as the curve takes to play its source.
    steps.push_back({"tempo map",
                     [this]
                     {
                         const double bpm = model().bpm;
                         actions.setTempoMap({{0.0, bpm}, {8000.0, bpm * 1.2}});
                     },
                     {},
                     [this](const juce::var&)
                     {
                         const auto* clip = model().findClip(tempoLateClipId);
                         if (clip == nullptr || model().tempoMap.size() != 2 || clip->tempoFollowBpm <= 0.0) return false;
                         const double expected = model().tempoCurve().timelineAfterSource(clip->offsetMs, clip->durationMs, clip->tempoFollowBpm)
                                                 - clip->offsetMs;
                         const bool engineFollows = engine.host().engine().clipFollowsTempoMapForTest(tempoLateClipId);
                         lastTempoLine = "tempo map follow=" + juce::String(clip->tempoFollowBpm, 2) + " length=" + juce::String(clip->effectiveDurationMs, 1)
                                         + " expected=" + juce::String(expected, 1) + " engine=" + (engineFollows ? "yes" : "no");
                         const bool ok = std::abs(clip->effectiveDurationMs - expected) < 1.0 && engineFollows;
                         if (ok) report.add(lastTempoLine);
                         return ok;
                     }});
    steps.back().poll = true;
    Step tempoMapShot{"tempo map snapshot",
                      [this]
                      {
                          if (auto* window = juce::TopLevelWindow::getTopLevelWindow(0))
                          {
                              const auto image = window->createComponentSnapshot(window->getLocalBounds());
                              juce::FileOutputStream out(workDir.getChildFile("tempo-map.png"));
                              if (out.openedOk()) juce::PNGImageFormat().writeImageToStream(image, out);
                          }
                      },
                      {},
                      [this](const juce::var&) { return workDir.getChildFile("tempo-map.png").existsAsFile(); }};
    tempoMapShot.delayMs = 1500.0;
    steps.push_back(tempoMapShot);
    steps.push_back({"tempo map undo", [this] { engine.send("EDIT_UNDO"); }, {},
                     [this](const juce::var&)
                     {
                         const auto* clip = model().findClip(tempoLateClipId);
                         return model().tempoMap.empty() && clip != nullptr && clip->tempoFollowBpm == 0.0
                                && !engine.host().engine().clipFollowsTempoMapForTest(tempoLateClipId);
                     }});
    steps.back().poll = true;
    // Mashup: one track's vocals over the other's instrumental. Stems stand in (copies of the
    // sources, as stems in the library) so the separation models are not needed here; the
    // builder finds them, as it would real ones, and places the four parts together.
    steps.push_back({"mashup stems",
                     [this]
                     {
                         const std::pair<int, const char*> parts[] = {{0, "Vocals"}, {1, "Drums"}, {1, "Bass"}, {1, "Other"}};
                         for (const auto& [which, label] : parts)
                             if (const auto* source = model().findLibraryItem(tempoItemIds[which]))
                                 engine.send("LIBRARY_ADD", object({{"itemId", EngineClient::newId("lib")},
                                                                    {"filePath", source->filePath},
                                                                    {"fileName", source->fileName},
                                                                    {"name", juce::String(label) + " - " + source->displayName()},
                                                                    {"kind", "stem"},
                                                                    {"sourceItemId", source->id},
                                                                    {"durationMs", source->durationMs},
                                                                    {"sampleRate", source->sampleRate},
                                                                    {"channelCount", source->channelCount}}));
                     },
                     {},
                     [this](const juce::var&)
                     {
                         mirror.refreshNow();
                         return MashupBuilder::findStem(model(), tempoItemIds[0], "Vocals").isNotEmpty()
                                && MashupBuilder::findStem(model(), tempoItemIds[1], "Other").isNotEmpty();
                     }});
    steps.back().poll = true;
    steps.push_back({"mashup",
                     [this]
                     {
                         mashupTracksBefore = static_cast<int>(model().tracks.size());
                         mashupBuilder = std::make_unique<MashupBuilder>(engine, mirror, actions, stems);
                         mashupBuilder->onNotify = [this](const juce::String& m, bool) { report.add(m); };
                         mashupBuilder->start(tempoItemIds[0], tempoItemIds[1]);
                     },
                     {},
                     [this](const juce::var&)
                     {
                         if (mashupBuilder->busy()) return false;
                         int named = 0;
                         double start = -1.0;
                         bool aligned = true;
                         for (const auto& t : model().tracks)
                             if (t.name.startsWith("Mashup ") && !t.clips.empty())
                             {
                                 ++named;
                                 if (start < 0.0) start = t.clips.front().offsetMs;
                                 // On the bar grid, the four stems of two tracks start within a beat of each other.
                                 aligned = aligned && std::abs(t.clips.front().offsetMs - start) < 60000.0 / model().bpm;
                             }
                         lastTempoLine = "mashup tracks=" + juce::String(named) + " aligned=" + (aligned ? "yes" : "no");
                         const bool ok = named == 4 && aligned && static_cast<int>(model().tracks.size()) == mashupTracksBefore + 4;
                         if (ok) report.add(lastTempoLine);
                         return ok;
                     }});
    steps.back().poll = true;
    // The auto mixer's harmonic arithmetic (no UI): keys onto the Camelot wheel, and which mix.
    steps.push_back({"camelot keys", [] {}, {},
                     [this](const juce::var&)
                     {
                         using automix::camelot;
                         const bool names = camelot("C major").text() == "8B" && camelot("A minor").text() == "8A"
                                            && camelot("F# minor").text() == "11A" && camelot("Db major").text() == "3B"
                                            && camelot("Am").text() == "8A" && camelot("Ebm").text() == "2A" && camelot("12B").text() == "12B"
                                            && !camelot("").valid();
                         const bool distances = automix::keyDistance(camelot("8A"), camelot("9A")) == 1
                                                && automix::keyDistance(camelot("8A"), camelot("8B")) == 1
                                                && automix::keyDistance(camelot("12A"), camelot("1A")) == 1
                                                && automix::keyDistance(camelot("8A"), camelot("3B")) >= 2;
                         const bool shifts = automix::transposeKey("A minor", 2) == "B minor" && automix::transposeKey("C major", -1) == "B major"
                                             && automix::keyMatchShift("C major", "D major", 6) == 2
                                             && automix::keyMatchShift("C major", "G major", 6) == 0
                                             && !automix::keyMatchShift("C major", "", 6).has_value();
                         if (names && distances && shifts)
                             report.add("camelot: C=8B Am=8A F#m=11A Db=3B; 8A-9A, 8A-8B, 12A-1A mix; 8A-3B clashes; C into D is +2");
                         return names && distances && shifts;
                     }});
    // Key shift: two semitones up at the same tempo, then back.
    steps.push_back({"key shift", [this] { actions.setClipKeyShift(tempoLateClipId, 2); }, {},
                     [this](const juce::var&)
                     {
                         const auto* clip = model().findClip(tempoLateClipId);
                         if (clip == nullptr) return false;
                         const auto* item = model().findLibraryItem(clip->libraryItemId);
                         const bool ok = std::lround(clip->semitones) == 2 && item != nullptr
                                         && automix::clipKey(model(), *clip) == automix::transposeKey(item->key, 2);
                         lastTempoLine = "key shift " + (item != nullptr ? item->key : juce::String()) + " -> " + automix::clipKey(model(), *clip);
                         if (ok) report.add(lastTempoLine);
                         return ok;
                     }});
    steps.back().poll = true;
    steps.push_back({"key reset", [this] { actions.setClipKeyShift(tempoLateClipId, 0); }, {},
                     [this](const juce::var&)
                     {
                         const auto* clip = model().findClip(tempoLateClipId);
                         return clip != nullptr && std::lround(clip->semitones) == 0;
                     }});
    steps.back().poll = true;
    // DJ transitions: auto mix the two tempo-journey tracks (125 into 100 BPM: a big tempo
    // gap), then re-pick the join as Echo Out, then remove it.
    steps.push_back({"auto mix",
                     [this]
                     {
                         mixWaveforms = std::make_unique<WaveformStore>(engine.host().peaksCache());
                         report.add(automix::build(engine, mirror, actions, *mixWaveforms, {tempoItemIds[1], tempoItemIds[0]}, false, "Auto"));
                     },
                     {},
                     [this](const juce::var&)
                     {
                         const auto* list = model().raw.getProperty("mixTransitions", {}).getArray();
                         if (list == nullptr) return false;
                         for (const auto& t : *list)
                         {
                             const auto* out = model().findClip(t.getProperty("outClipId", {}).toString());
                             const auto* in = model().findClip(t.getProperty("inClipId", {}).toString());
                             if (out == nullptr || in == nullptr) continue;
                             const double start = static_cast<double>(t.getProperty("startMs", 0.0));
                             const double end = static_cast<double>(t.getProperty("endMs", 0.0));
                             const auto* inTrack = model().findTrack(in->trackId);
                             const auto* outTrack = model().findTrack(out->trackId);
                             bool automated = false;
                             for (const auto* track : {inTrack, outTrack})
                                 if (track != nullptr)
                                     for (const auto& p : automation::params())
                                         automated = automated || !automation::pointsOf(*track, p.id).empty();
                             lastTempoLine = "auto mix preset=" + t.getProperty("preset", {}).toString() + " window=" + juce::String(start, 0) + ".."
                                             + juce::String(end, 0) + " in starts " + juce::String(in->offsetMs, 0) + " out ends "
                                             + juce::String(out->endMs(), 0) + " automated=" + (automated ? "yes" : "no");
                             const bool ok = std::abs(in->offsetMs - start) < 2.0 && std::abs(out->endMs() - end) < 2.0 && automated
                                             && in->trackId != out->trackId;
                             if (ok)
                             {
                                 mixTransitionId = t.getProperty("id", {}).toString();
                                 report.add(lastTempoLine);
                             }
                             return ok;
                         }
                         return false;
                     }});
    steps.back().poll = true;
    // A tempo ramp across the join: the map glides between the two tempos and the outgoing
    // clip, which now follows it, still ends exactly on the window.
    steps.push_back({"ramp transition",
                     [this]
                     {
                         if (const auto* list = model().raw.getProperty("mixTransitions", {}).getArray())
                             for (const auto& t : *list)
                                 if (t.getProperty("id", {}).toString() == mixTransitionId)
                                     mix::apply(engine, actions, model(), t.getProperty("outClipId", {}).toString(),
                                                t.getProperty("inClipId", {}).toString(), "tempoRampBlend");
                     },
                     {},
                     [this](const juce::var&)
                     {
                         const auto* list = model().raw.getProperty("mixTransitions", {}).getArray();
                         if (list == nullptr) return false;
                         for (const auto& t : *list)
                             if (t.getProperty("id", {}).toString() == mixTransitionId)
                             {
                                 const auto* out = model().findClip(t.getProperty("outClipId", {}).toString());
                                 const double end = static_cast<double>(t.getProperty("endMs", 0.0));
                                 const bool ok = out != nullptr && t.getProperty("preset", {}).toString() == "tempoRampBlend"
                                                 && model().tempoMap.size() == 2 && out->tempoFollowBpm > 0.0
                                                 && std::abs(out->endMs() - end) < 2.0;
                                 lastTempoLine = "ramp transition map=" + juce::String(static_cast<int>(model().tempoMap.size()))
                                                 + " out ends " + (out != nullptr ? juce::String(out->endMs(), 1) : juce::String("-"))
                                                 + " window ends " + juce::String(end, 1) + " preset "
                                                 + t.getProperty("preset", {}).toString() + " follow "
                                                 + (out != nullptr ? juce::String(out->tempoFollowBpm, 2) : juce::String("-"));
                                 if (ok) report.add(lastTempoLine);
                                 return ok;
                             }
                         return false;
                     }});
    steps.back().poll = true;
    steps.push_back({"transition preset",
                     [this]
                     {
                         if (const auto* list = model().raw.getProperty("mixTransitions", {}).getArray())
                             for (const auto& t : *list)
                                 if (t.getProperty("id", {}).toString() == mixTransitionId)
                                     mix::apply(engine, actions, model(), t.getProperty("outClipId", {}).toString(),
                                                t.getProperty("inClipId", {}).toString(), "echoOut");
                     },
                     {},
                     [this](const juce::var&)
                     {
                         const auto* list = model().raw.getProperty("mixTransitions", {}).getArray();
                         if (list == nullptr) return false;
                         for (const auto& t : *list)
                             if (t.getProperty("id", {}).toString() == mixTransitionId)
                             {
                                 // Re-picking takes the ramp's tempo points back out.
                                 const bool ok = t.getProperty("preset", {}).toString() == "echoOut"
                                                 && static_cast<double>(model().raw.getProperty("delayMix", 0.0)) > 0.5
                                                 && model().tempoMap.empty();
                                 lastTempoLine = "transition preset=" + t.getProperty("preset", {}).toString() + " delayMix="
                                                 + model().raw.getProperty("delayMix", 0.0).toString();
                                 if (ok) report.add(lastTempoLine);
                                 return ok;
                             }
                         return false;
                     }});
    steps.back().poll = true;
    // Outs and ins: a Low-Pass Out dropped on the outgoing clip, then a Thin Intro on the
    // incoming one, as from the Transitions panel. The join becomes those two halves, the echo
    // preset's throw comes back out, and the filter sweeps on both tracks.
    steps.push_back({"transition halves",
                     [this]
                     {
                         if (const auto* list = model().raw.getProperty("mixTransitions", {}).getArray())
                             for (const auto& t : *list)
                                 if (t.getProperty("id", {}).toString() == mixTransitionId)
                                 {
                                     const auto outId = t.getProperty("outClipId", {}).toString();
                                     const auto inId = t.getProperty("inClipId", {}).toString();
                                     const double middle = (static_cast<double>(t.getProperty("startMs", 0.0)) + static_cast<double>(t.getProperty("endMs", 0.0))) / 2.0;
                                     report.add("drop out: " + mix::drop(engine, actions, model(), outId, middle, "transition:out:lowPassOut"));
                                     mirror.refreshNow();
                                     report.add("drop in: " + mix::drop(engine, actions, model(), inId, middle, "transition:in:highPassIn"));
                                     return;
                                 }
                     },
                     {},
                     [this](const juce::var&)
                     {
                         const auto* list = model().raw.getProperty("mixTransitions", {}).getArray();
                         if (list == nullptr) return false;
                         for (const auto& t : *list)
                             if (t.getProperty("id", {}).toString() == mixTransitionId)
                             {
                                 const auto join = mix::joinOf(t);
                                 const auto* out = model().findClip(t.getProperty("outClipId", {}).toString());
                                 const auto* in = model().findClip(t.getProperty("inClipId", {}).toString());
                                 const auto* outTrack = out != nullptr ? model().findTrack(out->trackId) : nullptr;
                                 const auto* inTrack = in != nullptr ? model().findTrack(in->trackId) : nullptr;
                                 const double start = static_cast<double>(t.getProperty("startMs", 0.0));
                                 const double end = static_cast<double>(t.getProperty("endMs", 0.0));
                                 auto inWindow = [&](const TrackModel* track, const char* param)
                                 {
                                     int n = 0;
                                     if (track != nullptr)
                                         for (const auto& p : automation::pointsOf(*track, param))
                                             if (p.timeMs >= start && p.timeMs <= end) ++n;
                                     return n;
                                 };
                                 const bool ok = t.getProperty("preset", {}).toString() == "custom" && join.out.has_value() && join.in.has_value()
                                                 && join.out->kind == "lowPassOut" && join.in->kind == "highPassIn"
                                                 && inWindow(outTrack, "filter") > 3 && inWindow(inTrack, "filter") > 3
                                                 && inWindow(outTrack, "delaySend") == 0;
                                 lastTempoLine = "transition halves label=" + mix::labelOf(t) + " out filter points=" + juce::String(inWindow(outTrack, "filter"))
                                                 + " in filter points=" + juce::String(inWindow(inTrack, "filter"))
                                                 + " echo points=" + juce::String(inWindow(outTrack, "delaySend"));
                                 if (ok) report.add(lastTempoLine);
                                 return ok;
                             }
                         return false;
                     }});
    steps.back().poll = true;
    // Tweaking a half: the out's bass swaps at three quarters, only 20 dB down, on an S-curve;
    // and the editor drawn for eyes.
    steps.push_back({"transition tweak",
                     [this]
                     {
                         if (const auto* list = model().raw.getProperty("mixTransitions", {}).getArray())
                             for (const auto& t : *list)
                                 if (t.getProperty("id", {}).toString() == mixTransitionId)
                                 {
                                     auto join = mix::joinOf(t);
                                     if (!join.out.has_value()) return;
                                     join.out->low = "swap";
                                     join.out->swapAt = 0.75;
                                     join.out->killDb = -20.0;
                                     join.out->curve = "sCurve";
                                     const auto outId = t.getProperty("outClipId", {}).toString();
                                     const auto inId = t.getProperty("inClipId", {}).toString();
                                     mix::applyJoin(engine, actions, model(), outId, inId, join);
                                     mirror.refreshNow();
                                     TransitionEditor editor(mirror, actions, outId, inId);
                                     const auto image = editor.createComponentSnapshot(editor.getLocalBounds());
                                     juce::FileOutputStream out(workDir.getChildFile("transition-editor.png"));
                                     if (out.openedOk()) juce::PNGImageFormat().writeImageToStream(image, out);
                                     return;
                                 }
                     },
                     {},
                     [this](const juce::var&)
                     {
                         const auto* list = model().raw.getProperty("mixTransitions", {}).getArray();
                         if (list == nullptr) return false;
                         for (const auto& t : *list)
                             if (t.getProperty("id", {}).toString() == mixTransitionId)
                             {
                                 const auto* out = model().findClip(t.getProperty("outClipId", {}).toString());
                                 const auto* track = out != nullptr ? model().findTrack(out->trackId) : nullptr;
                                 const double start = static_cast<double>(t.getProperty("startMs", 0.0));
                                 const double end = static_cast<double>(t.getProperty("endMs", 0.0));
                                 double lowest = 0.0;
                                 if (track != nullptr)
                                     for (const auto& p : automation::pointsOf(*track, "toneBass"))
                                         if (p.timeMs >= start && p.timeMs <= end) lowest = juce::jmin(lowest, p.value);
                                 const bool ok = std::abs(lowest + 20.0) < 0.01 && workDir.getChildFile("transition-editor.png").existsAsFile();
                                 lastTempoLine = "transition tweak bass down to " + juce::String(lowest, 1) + " dB";
                                 if (ok) report.add(lastTempoLine);
                                 return ok;
                             }
                         return false;
                     }});
    steps.back().poll = true;
    // Scrolled to the join, so the picture shows it.
    // A curve drawn by hand in the editor (the In's fader): its points, with their shapes, are
    // what the incoming track's lane gets over the window, and the transition keeps them.
    steps.push_back({"transition drawn by hand",
                     [this]
                     {
                         if (const auto* list = model().raw.getProperty("mixTransitions", {}).getArray())
                             for (const auto& t : *list)
                                 if (t.getProperty("id", {}).toString() == mixTransitionId)
                                 {
                                     TransitionEditor editor(mirror, actions, t.getProperty("outClipId", {}).toString(),
                                                             t.getProperty("inClipId", {}).toString());
                                     editor.drawByHandForTest(false, "level", {{0.0, -60.0, "slowStart", 0.3},
                                                                               {0.5, -6.0, "smooth", 0.0},
                                                                               {1.0, 0.0, "linear", 0.0}});
                                     mirror.refreshNow();
                                     const auto image = editor.createComponentSnapshot(editor.getLocalBounds());
                                     juce::FileOutputStream out(workDir.getChildFile("transition-editor-manual.png"));
                                     if (out.openedOk()) juce::PNGImageFormat().writeImageToStream(image, out);
                                 }
                     },
                     {},
                     [this](const juce::var&)
                     {
                         if (const auto* list = model().raw.getProperty("mixTransitions", {}).getArray())
                             for (const auto& t : *list)
                                 if (t.getProperty("id", {}).toString() == mixTransitionId)
                                 {
                                     const auto* in = model().findClip(t.getProperty("inClipId", {}).toString());
                                     const auto* track = in != nullptr ? model().findTrack(in->trackId) : nullptr;
                                     const double start = static_cast<double>(t.getProperty("startMs", 0.0));
                                     const double end = static_cast<double>(t.getProperty("endMs", 0.0));
                                     const double middle = start + 0.5 * (end - start);
                                     bool found = false;
                                     if (track != nullptr)
                                         for (const auto& p : automation::pointsOf(*track, "level"))
                                             found = found || (std::abs(p.timeMs - middle) < 2.0 && std::abs(p.value + 6.0) < 0.01 && p.curve == "smooth");
                                     const bool kept = t.getProperty("settings", {}).getProperty("in", {}).getProperty("manual", {}).hasProperty("level");
                                     if (found && kept) report.add("transition drawn by hand: in fader point at the middle, -6 dB, smooth");
                                     return found && kept;
                                 }
                         return false;
                     }});
    steps.back().poll = true;
    steps.push_back({"reveal transition",
                     [this]
                     {
                         if (auto* timeline = findDescendant<TimelineView>(juce::TopLevelWindow::getTopLevelWindow(0)))
                             if (const auto* list = model().raw.getProperty("mixTransitions", {}).getArray())
                                 for (const auto& t : *list)
                                     if (t.getProperty("id", {}).toString() == mixTransitionId)
                                         if (const auto* out = model().findClip(t.getProperty("outClipId", {}).toString()))
                                             timeline->revealRange(static_cast<double>(t.getProperty("startMs", 0.0)),
                                                                   static_cast<double>(t.getProperty("endMs", 0.0)), out->trackId);
                     },
                     {}, [](const juce::var&) { return true; }});
    Step mixShot{"transitions snapshot",
                 [this]
                 {
                     if (auto* window = juce::TopLevelWindow::getTopLevelWindow(0))
                     {
                         const auto image = window->createComponentSnapshot(window->getLocalBounds());
                         juce::FileOutputStream out(workDir.getChildFile("transitions.png"));
                         if (out.openedOk()) juce::PNGImageFormat().writeImageToStream(image, out);
                     }
                 },
                 {},
                 [this](const juce::var&) { return workDir.getChildFile("transitions.png").existsAsFile(); }};
    mixShot.delayMs = 1500.0;
    steps.push_back(mixShot);
    // A plugin scan across every hosted format, when asked for (STEMCEL_SMOKE_PLUGIN_SCAN=1:
    // it loads the machine's installed plugins in the scan worker): the list must hold each
    // format found, VST 2 included in a build with it.
    steps.push_back({"plugin scan",
                     [this]
                     {
                         if (juce::SystemStats::getEnvironmentVariable("STEMCEL_SMOKE_PLUGIN_SCAN", {}).isEmpty())
                         {
                             report.add("SKIP plugin scan (set STEMCEL_SMOKE_PLUGIN_SCAN=1)");
                             pluginScanSkipped = true;
                             return;
                         }

                         engine.send("PLUGIN_SCAN", juce::var(new juce::DynamicObject()));
                     },
                     {},
                     [this](const juce::var&)
                     {
                         if (pluginScanSkipped) return true;
                         // The scan's last progress message says it finished; the fresh list follows it.
                         if (!static_cast<bool>(engine.latest("PLUGIN_SCAN_PROGRESS").getProperty("finished", false))) return false;
                         const auto list = engine.latest("PLUGIN_LIST");
                         if (static_cast<bool>(list.getProperty("scanning", true))) return false;
                         std::map<juce::String, int> formats;
                         if (const auto* plugins = list.getProperty("plugins", {}).getArray())
                             for (const auto& p : *plugins) ++formats[p.getProperty("format", "?").toString()];
                         juce::String line = "plugin scan formats:";
                         for (const auto& [format, count] : formats) line << " " << format << "=" << count;
                         report.add(line);
                         return true;
                     }});
    steps.back().poll = true;
    steps.back().timeoutMs = 300000.0;
    // An installed plugin's parameter follows its automation lane while the timeline plays
    // (with the scan above: STEMCEL_SMOKE_PLUGIN_SCAN=1). STEMCEL_SMOKE_PLUGIN_DIR adds a
    // folder of plugins to scan first, for a machine with no effect plugin installed.
    steps.push_back({"installed plugin folder",
                     [this]
                     {
                         const auto dir = juce::SystemStats::getEnvironmentVariable("STEMCEL_SMOKE_PLUGIN_DIR", {});
                         pluginDirScanDone = std::make_shared<std::atomic<bool>>(pluginScanSkipped || dir.isEmpty());
                         if (pluginDirScanDone->load()) return;
                         engine.host().engine().pluginCatalogue().startScan(
                             juce::FileSearchPath(dir), [](plugins::ScanProgress) {},
                             [done = pluginDirScanDone](bool) { done->store(true); });
                     },
                     {},
                     [this](const juce::var&) { return pluginDirScanDone != nullptr && pluginDirScanDone->load(); }});
    steps.back().poll = true;
    steps.back().timeoutMs = 120000.0;
    steps.push_back({"installed plugin automation",
                     [this]
                     {
                         pluginTests.clear();
                         pluginAutoStage = 0;
                         if (pluginScanSkipped) return;
                         mirror.refreshNow();
                         pluginAutoTrackId.clear();
                         for (const auto& t : model().tracks)
                             if (!t.clips.empty() && pluginAutoTrackId.isEmpty()) pluginAutoTrackId = t.id;
                         // Every effect the scan found, all on one track.
                         for (const auto& d : engine.host().engine().pluginCatalogue().getKnownPlugins())
                         {
                             if (d.isInstrument) continue;
                             PluginTest test;
                             test.identifier = d.fileOrIdentifier;
                             test.name = d.name + " (" + d.pluginFormatName + ")";
                             test.plainName = d.name;
                             pluginTests.push_back(test);
                             engine.send("TRACK_ADD_PLUGIN", object({{"trackId", pluginAutoTrackId}, {"identifier", d.fileOrIdentifier}}));
                         }
                     },
                     {},
                     [this](const juce::var&)
                     {
                         if (pluginScanSkipped) return true;
                         if (pluginTests.empty())
                         {
                             report.add("SKIP installed plugin automation: no effect plugin installed");
                             return true;
                         }
                         auto& audio = engine.host().engine();
                         if (pluginAutoStage == 0)
                         {
                             // Each plugin's slot, by name, once the project shows it.
                             mirror.refreshNow();
                             const auto* track = model().findTrack(pluginAutoTrackId);
                             const auto* slots = track != nullptr ? track->raw.getProperty("plugins", {}).getArray() : nullptr;
                             if (slots == nullptr) return false;
                             for (auto& test : pluginTests)
                                 for (const auto& slot : *slots)
                                     if (test.slotId.isEmpty() && slot.getProperty("name", {}).toString() == test.plainName)
                                     {
                                         bool taken = false;
                                         for (const auto& other : pluginTests) taken = taken || other.slotId == slot.getProperty("slotId", {}).toString();
                                         if (!taken) test.slotId = slot.getProperty("slotId", {}).toString();
                                     }
                             for (const auto& test : pluginTests)
                                 if (test.slotId.isEmpty()) return false;
                             // Every parameter to the far end of its range from where it is: exact
                             // for stepped parameters (on/off, choices, integers) as well.
                             const double end = model().contentEndMs() + 60000.0;
                             for (auto& test : pluginTests)
                             {
                                 test.names = audio.getTrackPluginParameterNames(pluginAutoTrackId, test.slotId);
                                 for (int i = 0; i < test.names.size(); ++i)
                                 {
                                     const float before = audio.getTrackPluginParameterValue(pluginAutoTrackId, test.slotId, i).value_or(0.0f);
                                     const float target = before < 0.5f ? 1.0f : 0.0f;
                                     test.targets.push_back(target);
                                     actions.setAutomation(pluginAutoTrackId, automation::pluginParamId(test.slotId, i), {{0.0, target}, {end, target}});
                                 }
                             }
                             actions.seek(0.0);
                             actions.play();
                             pluginAutoPlayedAt = juce::Time::getMillisecondCounter();
                             pluginAutoStage = 1;
                             return false;
                         }
                         if (juce::Time::getMillisecondCounter() - pluginAutoPlayedAt < 1500) return false;
                         actions.stop();
                         bool all = true;
                         for (const auto& test : pluginTests)
                         {
                             juce::StringArray missed;
                             for (int i = 0; i < test.names.size(); ++i)
                             {
                                 const auto now = audio.getTrackPluginParameterValue(pluginAutoTrackId, test.slotId, i);
                                 if (!now.has_value() || std::abs(*now - test.targets[static_cast<size_t>(i)]) > 0.02f)
                                     missed.add(test.names[i] + "=" + (now.has_value() ? juce::String(*now, 3) : juce::String("?")));
                             }
                             const int followed = test.names.size() - missed.size();
                             all = all && missed.isEmpty() && test.names.size() > 0;
                             report.add("installed plugin automation: " + test.name + " " + juce::String(followed) + "/" + juce::String(test.names.size())
                                        + " parameters followed their lanes (" + test.names.joinIntoString(", ") + ")"
                                        + (missed.isEmpty() ? juce::String() : "; missed " + missed.joinIntoString(", ")));
                             // Leave the project as it was for the steps after.
                             for (int i = 0; i < test.names.size(); ++i)
                                 actions.setAutomation(pluginAutoTrackId, automation::pluginParamId(test.slotId, i), {});
                             engine.send("TRACK_REMOVE_PLUGIN", object({{"trackId", pluginAutoTrackId}, {"slotId", test.slotId}}));
                         }
                         // Plugins scanned from the test's own folder leave the user's plugin list as it was.
                         if (const auto dir = juce::SystemStats::getEnvironmentVariable("STEMCEL_SMOKE_PLUGIN_DIR", {}); dir.isNotEmpty())
                             audio.pluginCatalogue().forgetPluginsIn(juce::File(dir));
                         return all;
                     }});
    steps.back().poll = true;
    steps.back().timeoutMs = 60000.0;
    // Drag and drop onto the timeline through its own target callbacks (drag move, then drop),
    // as a library tile, a Files row and the area below the tracks deliver them.
    auto dropOnTimeline = [this](const juce::var& description, bool belowTracks)
    {
        // The main window, whichever top-level window it is just now.
        juce::Component* window = nullptr;
        Workspace* workspace = nullptr;
        for (int i = 0; i < juce::TopLevelWindow::getNumTopLevelWindows() && workspace == nullptr; ++i)
            if ((workspace = findDescendant<Workspace>(juce::TopLevelWindow::getTopLevelWindow(i))) != nullptr)
                window = juce::TopLevelWindow::getTopLevelWindow(i);
        auto* timeline = window != nullptr ? findDescendant<TimelineView>(window) : nullptr;
        if (workspace == nullptr || timeline == nullptr)
        {
            report.add(juce::String("drop: no ") + (window == nullptr ? "window" : workspace == nullptr ? "workspace" : "timeline")
                       + " (windows: " + juce::String(juce::TopLevelWindow::getNumTopLevelWindows()) + ")");
            return;
        }
        // At the top for a track row; scrolled to the end for the empty space under the last track.
        workspace->viewState().scrollY = belowTracks ? 1.0e9 : 0.0;
        timeline->clampScroll();
        workspace->viewState().changed();
        const juce::Point<int> at{timeline->getWidth() / 3,
                                  belowTracks ? timeline->getHeight() - 20 : workspace->viewState().headerHeight() + 30};
        juce::DragAndDropTarget::SourceDetails details{description, nullptr, at};
        report.add("drop " + description.toString() + " at " + at.toString() + " interested="
                   + (timeline->isInterestedInDragSource(details) ? "yes" : "no") + " libraryItem="
                   + (model().findLibraryItem(description.toString().fromFirstOccurrenceOf("libraryItem:", false, false)) != nullptr ? "yes" : "no"));
        if (timeline->isInterestedInDragSource(details))
        {
            timeline->itemDragEnter(details);
            timeline->itemDragMove(details);
            timeline->itemDropped(details);
        }
    };
    auto clipTotal = [this]
    {
        int n = 0;
        for (const auto& t : model().tracks) n += static_cast<int>(t.clips.size());
        return n;
    };
    steps.push_back({"drag library item to timeline",
                     [this, dropOnTimeline, clipTotal]
                     {
                         dragClipsBefore = clipTotal();
                         dragItemId = model().library.empty() ? juce::String() : model().library.front().id;
                         dropOnTimeline("libraryItem:" + dragItemId, false);
                     },
                     {}, [this, clipTotal](const juce::var&) { return clipTotal() == dragClipsBefore + 1; }});
    steps.back().poll = true;
    steps.push_back({"drag library item below tracks",
                     [this, dropOnTimeline, clipTotal]
                     {
                         dragClipsBefore = clipTotal();
                         dragTracksBefore = static_cast<int>(model().tracks.size());
                         dropOnTimeline("libraryItem:" + dragItemId, true);
                     },
                     {},
                     [this, clipTotal](const juce::var&)
                     { return clipTotal() == dragClipsBefore + 1 && static_cast<int>(model().tracks.size()) == dragTracksBefore + 1; }});
    steps.back().poll = true;
    steps.push_back({"drag file to timeline",
                     [this, dropOnTimeline, clipTotal]
                     {
                         dragClipsBefore = clipTotal();
                         const auto file = workDir.getChildFile("Tempo").getChildFile("clicks 100.wav");
                         dropOnTimeline(importDragDescription({ImportRequest{file, {}}}), false);
                     },
                     {}, [this, clipTotal](const juce::var&) { return clipTotal() == dragClipsBefore + 1; }});
    steps.back().poll = true;
    steps.back().timeoutMs = 60000.0;
    // Master automation: a reverb-mix ramp and a level dip on the master's row, as its lane menu
    // writes them; the master row carries the lanes and the engine keeps the curves.
    steps.push_back({"master automation",
                     [this]
                     {
                         actions.setAutomationLanes(kMasterTrackId, {{"reverbMix", automation::kDefaultLaneHeight},
                                                                     {"level", automation::kDefaultLaneHeight},
                                                                     {"glue", automation::kDefaultLaneHeight}});
                         actions.setAutomation(kMasterTrackId, "reverbMix", {{0.0, 0.0, "smooth", 0.0}, {4000.0, 0.8}});
                         actions.setAutomation(kMasterTrackId, "glue", {{0.0, 0.2}, {4000.0, 0.7}});
                         actions.setAutomation(kMasterTrackId, "level", {{0.0, 0.0}, {2000.0, -12.0}, {4000.0, 0.0}});
                     },
                     {},
                     [this](const juce::var&)
                     {
                         const auto lanes = automation::lanesOf(model().master);
                         const auto reverb = automation::pointsOf(model().master, "reverbMix");
                         const auto stored = engine.host().project().getTrackAutomation(kMasterTrackId, "level");
                         const auto glue = engine.host().project().getTrackAutomation(kMasterTrackId, "glue");
                         lastTempoLine = "master automation lanes=" + juce::String(static_cast<int>(lanes.size())) + " reverb points="
                                         + juce::String(static_cast<int>(reverb.size())) + " level stored=" + juce::String(stored.size()) + " glue stored=" + juce::String(glue.size());
                         const bool ok = lanes.size() == 3 && reverb.size() == 2 && reverb.front().curve == "smooth" && stored.size() == 3
                                         && glue.size() == 2;
                         if (ok) report.add(lastTempoLine);
                         return ok;
                     }});
    steps.back().poll = true;
    // The A button: a track with no lanes opens on its gain; hiding and showing again brings
    // back what was shown. A picture of the open lanes, for eyes.
    steps.push_back({"automation toggle",
                     [this]
                     {
                         if (model().tracks.size() < 2) return;
                         automationTrackId = model().tracks[1].id;
                         actions.setAutomationLanes(automationTrackId, {});
                         actions.toggleAutomation(automationTrackId);
                     },
                     {},
                     [this](const juce::var&)
                     {
                         const auto* track = model().findTrack(automationTrackId);
                         if (track == nullptr) return false;
                         const auto lanes = automation::lanesOf(*track);
                         switch (automationStage)
                         {
                         case 0:
                             if (lanes.size() != 1 || lanes.front().paramId != "level") return false;
                             actions.setAutomationLanes(automationTrackId, {{"level", automation::kDefaultLaneHeight},
                                                                             {"pan", automation::kDefaultLaneHeight}});
                             automationStage = 1;
                             return false;
                         case 1:
                             if (lanes.size() != 2) return false;
                             if (auto* window = juce::TopLevelWindow::getTopLevelWindow(0))
                             {
                                 const auto image = window->createComponentSnapshot(window->getLocalBounds());
                                 juce::FileOutputStream out(workDir.getChildFile("automation-lanes.png"));
                                 if (out.openedOk()) juce::PNGImageFormat().writeImageToStream(image, out);
                             }
                             actions.toggleAutomation(automationTrackId);
                             automationStage = 2;
                             return false;
                         case 2:
                             if (!lanes.empty()) return false;
                             actions.toggleAutomation(automationTrackId);
                             automationStage = 3;
                             return false;
                         default:
                             if (lanes.size() != 2 || lanes[1].paramId != "pan") return false;
                             actions.setAutomationLanes(automationTrackId, {});
                             report.add("automation toggle: gain lane, hidden, restored with pan");
                             return true;
                         }
                     }});
    steps.back().poll = true;
    // Beat stutter: a build-up from bar 3 lays four repeats down, the last at 1/32.
    steps.push_back({"beat stutter",
                     [this]
                     {
                         if (const auto* c = model().findClip(tempoLateClipId))
                         {
                             stutterTrackId = c->trackId;
                             stutter::apply(actions, model(), stutterTrackId, "build", 8.0);
                         }
                     },
                     {},
                     [this](const juce::var&)
                     {
                         const auto* track = model().findTrack(stutterTrackId);
                         if (track == nullptr) return false;
                         juce::StringArray divisions;
                         if (const auto* list = track->raw.getProperty("beatRepeats", {}).getArray())
                             for (const auto& r : *list)
                                 if (static_cast<double>(r.getProperty("startBeat", 0.0)) >= 8.0 - 1.0e-6
                                     && static_cast<double>(r.getProperty("startBeat", 0.0)) < 12.0)
                                     divisions.add(r.getProperty("division", {}).toString());
                         lastTempoLine = "beat stutter divisions=" + divisions.joinIntoString(",");
                         const bool ok = divisions.size() == 4 && divisions.contains("1/32");
                         if (ok) report.add(lastTempoLine);
                         return ok;
                     }});
    steps.back().poll = true;
    // An automation pattern: a sine over a bar of the filter, its segments smooth.
    steps.push_back({"automation pattern",
                     [this]
                     {
                         const auto* track = model().findTrack(stutterTrackId);
                         const auto* param = automation::find("filter");
                         if (track == nullptr || param == nullptr) return;
                         const auto curve = model().tempoCurve();
                         actions.setAutomation(stutterTrackId, "filter",
                                               automation::applyPattern(automation::pointsOf(*track, "filter"), *param, "sine1",
                                                                        curve.msAtBeat(16.0), curve.msAtBeat(20.0),
                                                                        60000.0 / curve.bpmAt(curve.msAtBeat(16.0))));
                     },
                     {},
                     [this](const juce::var&)
                     {
                         const auto* track = model().findTrack(stutterTrackId);
                         if (track == nullptr) return false;
                         int smooth = 0;
                         const auto points = automation::pointsOf(*track, "filter");
                         for (const auto& p : points) smooth += p.curve == "smooth" ? 1 : 0;
                         lastTempoLine = "automation pattern points=" + juce::String(static_cast<int>(points.size())) + " smooth=" + juce::String(smooth);
                         const bool ok = smooth >= 8;
                         if (ok) report.add(lastTempoLine);
                         return ok;
                     }});
    steps.back().poll = true;
    // The mixer, docked in place of the bottom panel, for eyes; then back.
    Step mixerShot{"mixer snapshot",
                   [this]
                   {
                       auto* window = juce::TopLevelWindow::getTopLevelWindow(0);
                       auto* workspace = window != nullptr ? findDescendant<Workspace>(window) : nullptr;
                       if (workspace == nullptr) return;
                       if (!workspace->isMixerShown()) workspace->toggleMixer();
                       const auto image = window->createComponentSnapshot(window->getLocalBounds());
                       juce::FileOutputStream out(workDir.getChildFile("mixer.png"));
                       if (out.openedOk()) juce::PNGImageFormat().writeImageToStream(image, out);
                       workspace->toggleMixer();
                   },
                   {},
                   [this](const juce::var&) { return workDir.getChildFile("mixer.png").existsAsFile(); }};
    steps.push_back(mixerShot);
    // The Transitions tab, for eyes.
    Step panelShot{"transitions panel snapshot",
                   [this]
                   {
                       auto* window = juce::TopLevelWindow::getTopLevelWindow(0);
                       auto* panel = window != nullptr ? findDescendant<LibraryPanel>(window) : nullptr;
                       if (panel == nullptr) return;
                       panel->showTab(3);
                       const auto image = window->createComponentSnapshot(window->getLocalBounds());
                       juce::FileOutputStream out(workDir.getChildFile("transitions-panel.png"));
                       if (out.openedOk()) juce::PNGImageFormat().writeImageToStream(image, out);
                       panel->showTab(0);
                   },
                   {},
                   [this](const juce::var&) { return workDir.getChildFile("transitions-panel.png").existsAsFile(); }};
    steps.push_back(panelShot);
    // The rekordbox tab showing the smoke stick's OneLibrary (picked in the library list),
    // with the playlist the writes step made there, for eyes.
    Step rekordboxShot{"rekordbox panel snapshot",
                       [this]
                       {
                           auto* window = juce::TopLevelWindow::getTopLevelWindow(0);
                           auto* panel = window != nullptr ? findDescendant<LibraryPanel>(window) : nullptr;
                           if (panel == nullptr || rekordboxTrackId == "skipped") return;
                           panel->showTab(2);
                           if (auto* browser = findDescendant<RekordboxBrowser>(window))
                           {
                               RekordboxSource source;
                               source.kind = RekordboxSource::Kind::device;
                               source.name = "Smoke stick";
                               source.root = workDir.getChildFile("stick");
                               source.format = "oneLibrary";
                               browser->chooseSource(source);
                           }
                       },
                       {},
                       [this](const juce::var&)
                       {
                           if (rekordboxTrackId == "skipped") return true;
                           auto* window = juce::TopLevelWindow::getTopLevelWindow(0);
                           auto* browser = window != nullptr ? findDescendant<RekordboxBrowser>(window) : nullptr;
                           const auto* shown = browser != nullptr ? browser->currentSnapshot() : nullptr;
                           if (shown == nullptr || !shown->source.isDevice()) return false;
                           const auto image = window->createComponentSnapshot(window->getLocalBounds());
                           juce::FileOutputStream out(workDir.getChildFile("rekordbox-panel.png"));
                           if (out.openedOk()) juce::PNGImageFormat().writeImageToStream(image, out);
                           if (auto* panel = findDescendant<LibraryPanel>(window)) panel->showTab(0);
                           return true;
                       }};
    rekordboxShot.poll = true;
    steps.push_back(rekordboxShot);
    // Project tabs: copy a clip, open a new tab, paste it there (its library item comes too),
    // then go back to the first tab, which returns as it was, unsaved changes and all.
    steps.push_back({"tabs: new tab",
                     [this]
                     {
                         tabsTracksBefore = static_cast<int>(model().tracks.size());
                         actions.copyClips({tempoLateClipId});
                         if (const auto* clip = model().findClip(tempoLateClipId)) tabsPastedItem = clip->libraryItemId;
                         if (auto* strip = findDescendant<ProjectTabStrip>(juce::TopLevelWindow::getTopLevelWindow(0)))
                             strip->controller.newProject();
                     },
                     {},
                     [this](const juce::var&)
                     {
                         auto* strip = findDescendant<ProjectTabStrip>(juce::TopLevelWindow::getTopLevelWindow(0));
                         return strip != nullptr && strip->controller.projectTabs().size() == 2 && strip->controller.activeTabIndex() == 1
                                && model().tracks.empty();
                     }});
    steps.back().poll = true;
    steps.push_back({"tabs: paste across",
                     [this]
                     {
                         const auto track = actions.addTrack();
                         mirror.refreshNow();
                         actions.paste(track, 0.0);
                     },
                     {},
                     [this](const juce::var&)
                     {
                         mirror.refreshNow();
                         const bool ok = model().findLibraryItem(tabsPastedItem) != nullptr && !model().tracks.empty()
                                         && !model().tracks.front().clips.empty();
                         if (ok) report.add("tabs: pasted a clip and its library item into a new tab");
                         return ok;
                     }});
    steps.back().poll = true;
    steps.push_back({"tabs: switch back",
                     [this]
                     {
                         if (auto* strip = findDescendant<ProjectTabStrip>(juce::TopLevelWindow::getTopLevelWindow(0)))
                             strip->controller.switchToTab(0);
                     },
                     {},
                     [this](const juce::var&)
                     {
                         auto* strip = findDescendant<ProjectTabStrip>(juce::TopLevelWindow::getTopLevelWindow(0));
                         if (strip == nullptr) return false;
                         const auto& tabs = strip->controller.projectTabs();
                         const bool ok = strip->controller.activeTabIndex() == 0 && static_cast<int>(model().tracks.size()) == tabsTracksBefore
                                         && model().findClip(tempoLateClipId) != nullptr && tabs.size() == 2 && tabs[1].parked.existsAsFile()
                                         && !tabs[0].parked.existsAsFile();
                         lastTempoLine = "tabs: back with " + juce::String(static_cast<int>(model().tracks.size())) + " tracks (had "
                                         + juce::String(tabsTracksBefore) + "), tab 2 parked=" + (tabs.size() == 2 && tabs[1].parked.existsAsFile() ? "yes" : "no");
                         if (ok) report.add(lastTempoLine);
                         return ok;
                     }});
    steps.back().poll = true;
    // A recommended transition between clips on two tracks that do not overlap yet: the
    // incoming clip moves into place and the preset applies, drawn across both tracks.
    steps.push_back({"transition into a later clip",
                     [this]
                     {
                         // Every model pointer is fetched again after a refresh: it rebuilds the model.
                         auto* timeline = findDescendant<TimelineView>(juce::TopLevelWindow::getTopLevelWindow(0));
                         if (timeline == nullptr || model().findLibraryItem(tempoItemIds[0]) == nullptr
                             || model().findLibraryItem(tempoItemIds[1]) == nullptr)
                             return;
                         // After everything else, so no other clip is nearer.
                         const double after = model().contentEndMs() + 60000.0;
                         const auto outTrack = actions.addTrack();
                         nearOutClipId = actions.addClip(outTrack, *model().findLibraryItem(tempoItemIds[0]), after);
                         mirror.refreshNow();
                         const auto* outClip = model().findClip(nearOutClipId);
                         if (outClip == nullptr) return;
                         const auto curve = model().tempoCurve();
                         const double inAt = curve.msAtBeat(curve.beatAt(outClip->endMs()) + 16.0);
                         const auto inTrack = actions.addTrack();
                         nearInClipId = actions.addClip(inTrack, *model().findLibraryItem(tempoItemIds[1]), inAt);
                         mirror.refreshNow();
                         // The timeline reads the workspace's copy of the model: bring it up to date.
                         if (auto* workspace = findDescendant<Workspace>(juce::TopLevelWindow::getTopLevelWindow(0)))
                             workspace->projectMirror().refreshNow();
                         const auto pairs = timeline->nearbyPairs(nearOutClipId);
                         if (std::find(pairs.begin(), pairs.end(), std::pair{nearOutClipId, nearInClipId}) == pairs.end())
                         {
                             report.add("transition into a later clip: not found among " + juce::String(static_cast<int>(pairs.size())) + " nearby");
                             return;
                         }
                         timeline->applyTransitionPlacing(nearOutClipId, nearInClipId, "smoothBlend");
                     },
                     {},
                     [this](const juce::var&)
                     {
                         const auto* out = model().findClip(nearOutClipId);
                         const auto* in = model().findClip(nearInClipId);
                         if (out == nullptr || in == nullptr || !mix::overlap(*out, *in) || mix::recordFor(model(), nearOutClipId, nearInClipId) == nullptr)
                             return false;
                         auto* window = juce::TopLevelWindow::getTopLevelWindow(0);
                         auto* workspace = window != nullptr ? findDescendant<Workspace>(window) : nullptr;
                         auto* timeline = window != nullptr ? findDescendant<TimelineView>(window) : nullptr;
                         if (workspace != nullptr && timeline != nullptr)
                         {
                             auto& view = workspace->viewState();
                             for (const auto& row : layoutRows(model()))
                                 if (row.trackId == out->trackId) view.scrollY = juce::jmax(0, row.top - 20);
                             view.scrollXPx = juce::jmax(0.0, in->offsetMs / 1000.0 * view.pxPerSecond - 300.0);
                             timeline->clampScroll();
                             view.changed();
                             const auto image = window->createComponentSnapshot(window->getLocalBounds());
                             juce::FileOutputStream file(workDir.getChildFile("transition-band.png"));
                             if (file.openedOk()) juce::PNGImageFormat().writeImageToStream(image, file);
                         }
                         report.add("transition placed: in moved to " + juce::String(in->offsetMs, 0) + " ms, out ends " + juce::String(out->endMs(), 0));
                         return true;
                     }});
    steps.back().poll = true;
    // A transition follows its clips and takes a new window: shortening the outgoing clip pulls
    // the window's end (which sat on the clip's end) in with it, then the start dragged a bar
    // later is written there.
    steps.push_back({"transition follows and drags",
                     [this]
                     {
                         mirror.refreshNow();
                         txStage = 0;
                         const auto* record = mix::recordFor(model(), nearOutClipId, nearInClipId);
                         const auto* out = model().findClip(nearOutClipId);
                         if (record == nullptr || out == nullptr) return;
                         const double barMs = 240000.0 / model().bpm;
                         txOldStart = out->offsetMs;
                         txOldEnd = out->endMs();
                         txExpectEnd = txOldEnd - 2.0 * barMs;
                         actions.trimClip(nearOutClipId, out->offsetMs, txExpectEnd);
                         mirror.refreshNow();
                         juce::StringArray removed;
                         mix::retimeAfterClipEdit(engine, actions, model(), nearOutClipId, txOldStart, txOldEnd, removed);
                     },
                     {},
                     [this](const juce::var&)
                     {
                         mirror.refreshNow();
                         const auto* record = mix::recordFor(model(), nearOutClipId, nearInClipId);
                         if (record == nullptr) return false;
                         const double start = record->getProperty("startMs", 0.0), end = record->getProperty("endMs", 0.0);
                         const double barMs = 240000.0 / model().bpm;
                         auto* window = juce::TopLevelWindow::getTopLevelWindow(0);
                         auto* workspace = window != nullptr ? findDescendant<Workspace>(window) : nullptr;
                         auto* timeline = window != nullptr ? findDescendant<TimelineView>(window) : nullptr;
                         if (workspace == nullptr || timeline == nullptr) return false;
                         if (txStage == 0)
                         {
                             if (std::abs(end - txExpectEnd) > 1.0) return false;
                             txFollowLine = "end followed the clip to " + juce::String(end, 0) + " ms";
                             // On screen, so its label can be dragged.
                             workspace->projectMirror().refreshNow();
                             if (const auto* out = model().findClip(nearOutClipId)) timeline->revealRange(start, end, out->trackId);
                             txStartedAt = juce::Time::getMillisecondCounter();
                             txStage = 1;
                             return false;
                         }
                         if (txStage == 1)
                         {
                             if (juce::Time::getMillisecondCounter() - txStartedAt < 300) return false;
                             // Its start edge on the outgoing clip's row (on the incoming row that edge is the
                             // clip's own, for trimming), dragged right by a bar's width: the window starts a bar later.
                             const auto area = timeline->transitionArea(record->getProperty("id", {}).toString(), true);
                             if (!area) return false;
                             const auto label = std::optional<juce::Point<float>>(juce::Point<float>{area->getX() + 1.0f, area->getCentreY()});
                             const auto& view = workspace->viewState();
                             const auto dx = static_cast<float>(barMs / 1000.0 * view.pxPerSecond);
                             const auto now = juce::Time::getCurrentTime();
                             auto event = [&](juce::Point<float> at, bool dragged)
                             {
                                 return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), at,
                                                         juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier), 1.0f, 0.0f, 0.0f, 0.0f,
                                                         0.0f, timeline, timeline, now, *label, now, 1, dragged);
                             };
                             const juce::Point<float> to{label->x + dx, label->y};
                             txExpectStart = start + barMs;
                             timeline->mouseDown(event(*label, false));
                             timeline->mouseDrag(event(to, true));
                             timeline->mouseUp(event(to, true));
                             txStage = 2;
                             return false;
                         }
                         if (std::abs(start - txExpectStart) > 1.0) return false;
                         report.add("transition " + txFollowLine + "; its start edge dragged a bar later to " + juce::String(start, 0) + " ms");
                         return true;
                     }});
    steps.back().poll = true;
    // Every parameter there is to tweak can be automated: each knob of Track FX and Project FX
    // has a lane, every track and master lane is stored and understood by the audio engine, and
    // every parameter a transition writes is one of them.
    steps.push_back({"every parameter automatable",
                     [this]
                     {
                         juce::StringArray problems;
                         auto engineKnows = [](const juce::String& id)
                         {
                             AutomationParam p{};
                             return automationParamFromString(id, p);
                         };
                         auto isTrackParam = [](const juce::String& id)
                         {
                             for (const auto& p : automation::params())
                                 if (id == p.id) return true;
                             return false;
                         };
                         auto isMasterParam = [](const juce::String& id)
                         {
                             for (const auto& p : automation::masterParams())
                                 if (id == p.id) return true;
                             return false;
                         };
                         // The panels as the app shows them.
                         int knobCount = 0;
                         auto* window = juce::TopLevelWindow::getTopLevelWindow(0);
                         auto* library = window != nullptr ? findDescendant<LibraryPanel>(window) : nullptr;
                         auto* trackFx = library != nullptr ? dynamic_cast<TrackFxView*>(library->trackFxPanel()) : nullptr;
                         auto* projectFx = library != nullptr ? dynamic_cast<ProjectFxView*>(library->projectFxPanel()) : nullptr;
                         if (trackFx == nullptr || projectFx == nullptr) problems.add("FX panels not found");
                         else
                         {
                             for (const auto& [knob, lane] : trackFx->knobAutomation())
                             {
                                 ++knobCount;
                                 if (!isTrackParam(lane) || !engineKnows(lane)) problems.add("track knob " + knob + " -> '" + lane + "'");
                             }
                             for (const auto& [knob, lane] : projectFx->knobAutomation())
                             {
                                 ++knobCount;
                                 if (!isMasterParam(lane) || !engineKnows(lane)) problems.add("project knob " + knob + " -> '" + lane + "'");
                             }
                         }
                         // Every track lane and every master lane, written and read back from the project.
                         auto& project = engine.host().project();
                         mirror.refreshNow();
                         const auto laneTrack = model().tracks.empty() ? juce::String() : model().tracks.front().id;
                         for (const auto& p : automation::params())
                         {
                             actions.setAutomation(laneTrack, p.id, {{0.0, p.min}, {1000.0, p.max}});
                             if (!engineKnows(p.id) || project.getTrackAutomation(laneTrack, p.id).size() != 2) problems.add(juce::String("track lane ") + p.id);
                             actions.setAutomation(laneTrack, p.id, {});
                         }
                         for (const auto& p : automation::masterParams())
                         {
                             actions.setAutomation(kMasterTrackId, p.id, {{0.0, p.min}, {1000.0, p.max}});
                             if (!engineKnows(p.id) || project.getTrackAutomation(kMasterTrackId, p.id).size() != 2) problems.add(juce::String("master lane ") + p.id);
                             actions.setAutomation(kMasterTrackId, p.id, {});
                         }
                         // What transitions write: every preset's halves and every kind of half.
                         std::set<juce::String> written;
                         for (const auto& preset : mix::presets())
                         {
                             for (const auto& s : preset.out) written.insert(s.param);
                             for (const auto& s : preset.in) written.insert(s.param);
                         }
                         for (const auto& kind : mix::halfKinds())
                             for (const auto& s : mix::shapesFor(kind.defaults, kind.outgoing)) written.insert(s.param);
                         for (const auto& id : written)
                             if (!isTrackParam(id) || !engineKnows(id)) problems.add("transition writes '" + id + "'");
                         // The engine has no parameter the lanes leave out.
                         const int engineParams = static_cast<int>(AutomationParam::count_);
                         const int laneParams = static_cast<int>(automation::params().size() + automation::masterParams().size());
                         // "level" is a lane on both a track and the master: one engine parameter.
                         if (laneParams - 1 != engineParams)
                             problems.add("engine has " + juce::String(engineParams) + " parameters, lanes " + juce::String(laneParams - 1));
                         juce::StringArray writtenList;
                         for (const auto& id : written) writtenList.add(id);
                         everyParamLine = juce::String(knobCount) + " FX knobs, " + juce::String(static_cast<int>(automation::params().size()))
                                          + " track lanes, " + juce::String(static_cast<int>(automation::masterParams().size()))
                                          + " master lanes, transitions write {" + writtenList.joinIntoString(", ") + "}; "
                                          + (problems.isEmpty() ? juce::String("all automatable") : "problems: " + problems.joinIntoString("; "));
                         everyParamOk = problems.isEmpty() && knobCount > 0;
                     },
                     {},
                     [this](const juce::var&)
                     {
                         report.add(everyParamLine);
                         return everyParamOk;
                     }});
    // Glue's makeup and a clip's own gain reach the project, and a duplicate keeps the gain.
    steps.push_back({"glue makeup and clip gain",
                     [this]
                     {
                         mirror.refreshNow();
                         gainClipId.clear();
                         for (const auto& t : model().tracks)
                             if (!t.clips.empty() && gainClipId.isEmpty()) gainClipId = t.clips.front().id;
                         engine.send("PROJECT_SET_MIX_GLUE", object({{"makeupDb", 4.5}}));
                         actions.setClipGain(gainClipId, -6.0);
                         mirror.refreshNow();
                         gainCopyId = actions.duplicateClip(gainClipId);
                     },
                     {},
                     [this](const juce::var&)
                     {
                         mirror.refreshNow();
                         const auto* clip = model().findClip(gainClipId);
                         const auto* copy = model().findClip(gainCopyId);
                         const double makeup = static_cast<double>(model().raw.getProperty("mixGlueMakeupDb", 0.0));
                         const bool ok = clip != nullptr && copy != nullptr && std::abs(clip->gainDb + 6.0) < 0.01
                                         && std::abs(copy->gainDb + 6.0) < 0.01 && std::abs(makeup - 4.5) < 0.01;
                         if (ok)
                         {
                             report.add("glue makeup +4.5 dB; clip gain -6 dB, and its duplicate's too");
                             // Back to how they were for the steps after.
                             engine.send("PROJECT_SET_MIX_GLUE", object({{"makeupDb", 0.0}}));
                             actions.setClipGain(gainClipId, 0.0);
                             actions.removeClips({gainCopyId});
                         }
                         return ok;
                     }});
    steps.back().poll = true;
    // The panes resize by their drag bars: the library 60 px taller, the track headers 40 px
    // wider, each saved, then both dragged back so the user's own sizes are left as they were.
    steps.push_back({"resize bars",
                     [this]
                     {
                         auto* window = juce::TopLevelWindow::getTopLevelWindow(0);
                         auto* workspace = window != nullptr ? findDescendant<Workspace>(window) : nullptr;
                         auto* headers = window != nullptr ? findDescendant<TrackHeaders>(window) : nullptr;
                         auto* library = window != nullptr ? findDescendant<LibraryPanel>(window) : nullptr;
                         if (workspace == nullptr || headers == nullptr || library == nullptr) return;
                         std::vector<ResizeBar*> bars;
                         for (auto* child : workspace->getChildren())
                             if (auto* bar = dynamic_cast<ResizeBar*>(child)) bars.push_back(bar);
                         if (bars.size() != 2) return;
                         auto drag = [](ResizeBar* bar, int dx, int dy)
                         {
                             const auto now = juce::Time::getCurrentTime();
                             const juce::Point<float> from{2.0f, 2.0f}, to{2.0f + static_cast<float>(dx), 2.0f + static_cast<float>(dy)};
                             auto event = [&](juce::Point<float> at, bool dragged)
                             {
                                 return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), at, {}, 1.0f, 0.0f, 0.0f, 0.0f,
                                                         0.0f, bar, bar, now, from, now, 1, dragged);
                             };
                             bar->mouseDown(event(from, false));
                             bar->mouseDrag(event(to, true));
                             bar->mouseUp(event(to, true));
                         };
                         // bottomBar is added first, then headerBar.
                         const int libraryBefore = library->getHeight(), headersBefore = headers->getWidth();
                         drag(bars[0], 0, -60);
                         drag(bars[1], 40, 0);
                         const int libraryAfter = library->getHeight(), headersAfter = headers->getWidth();
                         // What the app saved, read back from its preferences file.
                         AppPreferences saved;
                         saved.load();
                         const int savedLibrary = static_cast<int>(saved.get("ui", "nativeLibraryHeight", 0));
                         const int savedHeaders = static_cast<int>(saved.get("ui", "trackHeaderWidth", 0));
                         // Back to where they were (and saved so).
                         drag(bars[0], 0, 60);
                         drag(bars[1], -40, 0);
                         resizeLine = "library " + juce::String(libraryBefore) + " -> " + juce::String(libraryAfter) + " px (saved "
                                      + juce::String(savedLibrary) + "), headers " + juce::String(headersBefore) + " -> "
                                      + juce::String(headersAfter) + " px (saved " + juce::String(savedHeaders) + "), restored to "
                                      + juce::String(library->getHeight()) + " / " + juce::String(headers->getWidth());
                         resizeOk = libraryAfter == libraryBefore + 60 && headersAfter == headersBefore + 40 && savedLibrary == libraryAfter
                                    && savedHeaders == headersAfter && library->getHeight() == libraryBefore && headers->getWidth() == headersBefore;
                     },
                     {},
                     [this](const juce::var&)
                     {
                         report.add(resizeLine);
                         return resizeOk;
                     }});
    // Fit shows the whole project at a zoom the floor allows, and a scroll away from the
    // playhead while following stays where it was put (follow pauses instead of snapping back).
    steps.push_back({"fit and follow",
                     [this]
                     {
                         auto* window = juce::TopLevelWindow::getTopLevelWindow(0);
                         auto* workspace = window != nullptr ? findDescendant<Workspace>(window) : nullptr;
                         auto* timeline = window != nullptr ? findDescendant<TimelineView>(window) : nullptr;
                         if (workspace == nullptr || timeline == nullptr) return;
                         workspace->projectMirror().refreshNow();
                         timeline->zoomToFit();
                         auto& view = workspace->viewState();
                         fitEndX = view.msToX(timeline->fitLengthMs());
                         fitFloor = timeline->minPxPerSecond();
                         fitWidth = timeline->getWidth();
                         // Following, playing from the start; the scroll comes once it plays.
                         view.followPlayback = true;
                         view.pxPerSecond = 100.0;
                         view.scrollXPx = 0.0;
                         view.changed();
                         followStage = 0;
                         actions.seek(0.0);
                         actions.play();
                     },
                     {},
                     [this](const juce::var&)
                     {
                         auto* window = juce::TopLevelWindow::getTopLevelWindow(0);
                         auto* workspace = window != nullptr ? findDescendant<Workspace>(window) : nullptr;
                         auto* timeline = window != nullptr ? findDescendant<TimelineView>(window) : nullptr;
                         if (workspace == nullptr || timeline == nullptr) return false;
                         auto& view = workspace->viewState();
                         if (followStage == 0)
                         {
                             if (!workspace->liveState().playing) return false;
                             // A wheel scroll far to the right, away from the playhead.
                             juce::MouseWheelDetails wheel{};
                             wheel.deltaX = -4.0f;
                             const auto now = juce::Time::getCurrentTime();
                             const juce::Point<float> at{200.0f, 100.0f};
                             timeline->mouseWheelMove(juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), at, {}, 1.0f, 0.0f,
                                                                       0.0f, 0.0f, 0.0f, timeline, timeline, now, at, now, 1, false),
                                                      wheel);
                             scrolledTo = view.scrollXPx;
                             followStartedAt = juce::Time::getMillisecondCounter();
                             followStage = 1;
                             return false;
                         }
                         if (juce::Time::getMillisecondCounter() - followStartedAt < 700) return false;
                         const bool stayed = std::abs(view.scrollXPx - scrolledTo) < 1.0 && scrolledTo > 500.0;
                         const bool paused = view.followSuspended;
                         actions.pause();
                         const bool fits = fitEndX <= fitWidth && fitEndX > fitWidth * 0.5 && fitFloor <= 1.25;
                         report.add("fit end at " + juce::String(fitEndX, 0) + " of " + juce::String(fitWidth) + " px, floor "
                                    + juce::String(fitFloor, 3) + " px/s; scrolled to " + juce::String(scrolledTo, 0) + " during playback and stayed "
                                    + (stayed ? "yes" : "no") + ", follow paused " + (paused ? "yes" : "no"));
                         return fits && stayed && paused;
                     }});
    steps.back().poll = true;
    steps.back().timeoutMs = 20000.0;
    // Shortcuts on the live timeline: Alt-drag copies a clip, the middle button pans, N turns
    // snapping off, and Cmd/Ctrl+J joins a clip split in two.
    steps.push_back({"shortcuts view",
                     [this]
                     {
                         shortcutStartMs = juce::Time::getMillisecondCounter();
                         auto* window = juce::TopLevelWindow::getTopLevelWindow(0);
                         auto* workspace = window != nullptr ? findDescendant<Workspace>(window) : nullptr;
                         if (workspace == nullptr) return;
                         workspace->projectMirror().refreshNow();
                         const auto* clip = workspace->projectMirror().model().findClip(nearOutClipId);
                         if (clip == nullptr) return;
                         // The clip 300 px wide, from x = 100.
                         auto& view = workspace->viewState();
                         view.pxPerSecond = 300.0 / juce::jmax(0.5, clip->effectiveDurationMs / 1000.0);
                         view.scrollXPx = clip->offsetMs / 1000.0 * view.pxPerSecond - 100.0;
                         for (const auto& row : layoutRows(workspace->projectMirror().model()))
                             if (row.trackId == clip->trackId) view.scrollY = juce::jmax(0, row.top - 20);
                         view.changed();
                     },
                     {},
                     [this](const juce::var&) { return juce::Time::getMillisecondCounter() - shortcutStartMs > 400; }});
    steps.back().poll = true;
    steps.push_back({"alt-drag, pan and snap key",
                     [this]
                     {
                         auto* window = juce::TopLevelWindow::getTopLevelWindow(0);
                         auto* workspace = window != nullptr ? findDescendant<Workspace>(window) : nullptr;
                         auto* timeline = window != nullptr ? findDescendant<TimelineView>(window) : nullptr;
                         if (workspace == nullptr || timeline == nullptr) return;
                         auto& wsMirror = workspace->projectMirror();
                         wsMirror.refreshNow();
                         const auto* clip = wsMirror.model().findClip(nearOutClipId);
                         if (clip == nullptr) return;
                         auto& view = workspace->viewState();
                         float y = -1.0f;
                         for (const auto& row : layoutRows(wsMirror.model()))
                             if (row.trackId == clip->trackId)
                             {
                                 y = static_cast<float>(row.top + view.headerHeight() - view.scrollY + row.clipHeight / 2);
                                 shortcutClipsBefore = static_cast<int>(wsMirror.model().findTrack(row.trackId)->clips.size());
                             }
                         if (y < 0.0f) return;
                         shortcutOriginMs = clip->offsetMs;
                         mirror.refreshNow();
                         snapBefore = actions.snapGrid();
                         const auto x = static_cast<float>(view.msToX(clip->offsetMs)) + 50.0f;
                         auto event = [timeline](juce::Point<float> at, juce::Point<float> from, juce::ModifierKeys mods, bool dragged)
                         {
                             const auto now = juce::Time::getCurrentTime();
                             return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), at, mods, 1.0f, 0.0f, 0.0f, 0.0f,
                                                     0.0f, timeline, timeline, now, from, now, 1, dragged);
                         };
                         // Alt-drag 400 px right: a copy past the clip's end, the clip where it was.
                         const juce::Point<float> from{x, y}, to{x + 400.0f, y};
                         const juce::ModifierKeys alt(juce::ModifierKeys::altModifier | juce::ModifierKeys::leftButtonModifier);
                         timeline->mouseDown(event(from, from, alt, false));
                         timeline->mouseDrag(event(to, from, alt, true));
                         timeline->mouseUp(event(to, from, juce::ModifierKeys(juce::ModifierKeys::altModifier), true));
                         // The middle button 120 px right: the view moves 120 px back.
                         timeline->clampScroll();
                         panScrollBefore = view.scrollXPx;
                         const juce::ModifierKeys middle(juce::ModifierKeys::middleButtonModifier);
                         const juce::Point<float> grab{180.0f, y}, release{300.0f, y};
                         timeline->mouseDown(event(grab, grab, middle, false));
                         timeline->mouseDrag(event(release, grab, middle, true));
                         panScrollAfter = view.scrollXPx;
                         timeline->mouseUp(event(release, grab, {}, true));
                         workspace->keyPressed(juce::KeyPress('n', {}, 'n'));
                     },
                     {},
                     [this](const juce::var&)
                     {
                         mirror.refreshNow();
                         const auto* clip = model().findClip(nearOutClipId);
                         if (clip == nullptr) return false;
                         const auto* track = model().findTrack(clip->trackId);
                         shortcutCopyId.clear();
                         for (const auto& c : track->clips)
                             if (c.id != clip->id && c.libraryItemId == clip->libraryItemId && c.offsetMs >= clip->endMs() - 1.0) shortcutCopyId = c.id;
                         const bool copied = static_cast<int>(track->clips.size()) == shortcutClipsBefore + 1 && shortcutCopyId.isNotEmpty()
                                             && std::abs(clip->offsetMs - shortcutOriginMs) < 0.5;
                         const bool panned = std::abs(panScrollAfter - panScrollBefore + 120.0) < 1.0;
                         const auto grid = actions.snapGrid();
                         const bool snapToggled = snapBefore != "free" ? grid == "free" : grid != "free";
                         lastTempoLine = juce::String("shortcuts: copy ") + (copied ? "yes" : "no") + ", pan "
                                         + juce::String(panScrollAfter - panScrollBefore, 1) + " px, snap " + snapBefore + " -> " + grid;
                         if (copied && panned && snapToggled)
                         {
                             report.add(lastTempoLine);
                             actions.setSnapGrid(snapBefore);
                         }
                         return copied && panned && snapToggled;
                     }});
    steps.back().poll = true;
    steps.push_back({"join clips",
                     [this]
                     {
                         mirror.refreshNow();
                         const auto* copy = model().findClip(shortcutCopyId);
                         if (copy == nullptr) return;
                         shortcutJoinEndMs = copy->endMs();
                         const auto right = actions.splitClipAt(shortcutCopyId, copy->offsetMs + copy->effectiveDurationMs / 2.0);
                         mirror.refreshNow();
                         shortcutJoins = actions.joinClips({shortcutCopyId, right});
                     },
                     {},
                     [this](const juce::var&)
                     {
                         mirror.refreshNow();
                         const auto* copy = model().findClip(shortcutCopyId);
                         const bool ok = shortcutJoins == 1 && copy != nullptr && std::abs(copy->endMs() - shortcutJoinEndMs) < 1.0;
                         if (ok) report.add("join clips: one clip again, to " + juce::String(shortcutJoinEndMs, 0) + " ms");
                         return ok;
                     }});
    steps.back().poll = true;
    // A drifting recording: the BPM variability check finds it drifting, auto-warp lands its
    // beats on the project's grid, and following it writes it into the tempo map (then undone).
    steps.push_back({"tempo drift",
                     [this]
                     {
                         driftStage = 0;
                         const auto file = workDir.getChildFile("Files").getChildFile("drifting.wav");
                         if (!writeDriftingClicks(file, driftBeats)) return;
                         ImportRequest request;
                         request.file = file;
                         importer.import(std::vector<ImportRequest>{request}, [this](const juce::StringArray& ids)
                                         { driftItemId = ids[0]; });
                     },
                     {},
                     [this](const juce::var&)
                     {
                         mirror.refreshNow();
                         const auto* item = model().findLibraryItem(driftItemId);
                         if (item == nullptr) return false;
                         if (driftStage == 0)
                         {
                             if (item->bpm <= 0.0) return false; // its tempo detection first
                             driftAnalyser = std::make_unique<TempoDriftAnalyser>(engine, mirror);
                             driftAnalyser->analyse(driftItemId);
                             driftStage = 1;
                             return false;
                         }
                         const auto drift = tempodrift::of(*item);
                         if (driftStage == 1)
                         {
                             if (!drift.valid()) return false;
                             driftLine = "drift " + juce::String(drift.driftPercent, 1) + "% around " + juce::String(drift.mainBpm, 1) + " BPM, "
                                         + juce::String(static_cast<int>(drift.sections.size())) + " stretch(es)";
                             if (drift.driftPercent < 6.0 || drift.sections.empty())
                             {
                                 lastTempoLine = "tempo drift missed: " + driftLine;
                                 return false;
                             }
                             const auto track = actions.addTrack();
                             driftClipId = actions.addClip(track, *item, model().contentEndMs() + 8000.0);
                             mirror.refreshNow();
                             tempodrift::autoWarp(actions, model(), driftClipId);
                             driftStage = 2;
                             return false;
                         }
                         const auto* clip = model().findClip(driftClipId);
                         if (clip == nullptr) return false;
                         if (driftStage == 2)
                         {
                             if (clip->sourceTempo.empty() || !model().clipFollows(*clip)) return false;
                             // Every click, through the warp, a whole number of project beats after the first.
                             const double beatMs = 60000.0 / model().bpm;
                             const double first = model().followTimelineMs(*clip, driftBeats.front() * 1000.0 - clip->inMs);
                             double worst = 0.0;
                             for (size_t k = 0; k < driftBeats.size(); ++k)
                             {
                                 const double at = model().followTimelineMs(*clip, driftBeats[k] * 1000.0 - clip->inMs) - first;
                                 worst = juce::jmax(worst, std::abs(at - static_cast<double>(k) * beatMs));
                             }
                             if (worst > 10.0)
                             {
                                 lastTempoLine = "tempo drift: " + driftLine + "; worst beat " + juce::String(worst, 1) + " ms off the grid";
                                 return false;
                             }
                             driftLine << "; auto-warped, worst beat " << juce::String(worst, 1) << " ms off the grid";
                             tempodrift::followInProjectTempo(actions, model(), driftClipId);
                             driftStage = 3;
                             return false;
                         }
                         if (driftStage == 3)
                         {
                             if (model().tempoMap.size() < 10 || clip->warpEnabled) return false;
                             // The map over the clip says what the recording does there.
                             const double mid = clip->offsetMs + clip->durationMs * 0.75;
                             const double mapped = model().tempoCurve().bpmAt(mid);
                             const double measured = TempoCurve(drift.curve, drift.mainBpm).bpmAt(clip->inMs + clip->durationMs * 0.75);
                             driftLine << "; followed: " << juce::String(static_cast<int>(model().tempoMap.size())) << " tempo points, "
                                       << juce::String(mapped, 1) << " BPM three quarters in (measured " << juce::String(measured, 1) << ")";
                             report.add("tempo drift: " + driftLine);
                             {
                                 // The report, for eyes.
                                 TempoReport view(mirror, actions, *driftAnalyser, driftItemId, driftClipId);
                                 const auto image = view.createComponentSnapshot(view.getLocalBounds());
                                 juce::FileOutputStream out(workDir.getChildFile("tempo-report.png"));
                                 if (out.openedOk()) juce::PNGImageFormat().writeImageToStream(image, out);
                             }
                             engine.send("EDIT_UNDO"); // leave the tempo map as it was for the steps after
                             driftStage = 4;
                             return std::abs(mapped - measured) < 0.5;
                         }
                         return false;
                     }});
    steps.back().poll = true;
    steps.back().timeoutMs = 90000.0;
    // Two files dropped at once: the prompt, answered "as dropped", mixes them on two decks.
    steps.push_back({"auto mix on drop",
                     [this]
                     {
                         auto* window = juce::TopLevelWindow::getTopLevelWindow(0);
                         auto* workspace = window != nullptr ? findDescendant<Workspace>(window) : nullptr;
                         if (workspace == nullptr) return;
                         const auto a = workDir.getChildFile("Files").getChildFile("drop-a.wav");
                         const auto b = workDir.getChildFile("Files").getChildFile("drop-b.wav");
                         if (!writeArrangement(a, 1.0f) || !writeArrangement(b, 0.8f)) return;
                         mirror.refreshNow();
                         dropTracksBefore = static_cast<int>(model().tracks.size());
                         dropPromptSeen = false;
                         workspace->importAndPlace({{a, {}}, {b, {}}}, {}, 0.0);
                     },
                     {},
                     [this](const juce::var&)
                     {
                         if (!dropPromptSeen)
                         {
                             if (auto* alert = dynamic_cast<juce::AlertWindow*>(juce::ModalComponentManager::getInstance()->getModalComponent(0)))
                             {
                                 dropPromptSeen = true;
                                 alert->exitModalState(2); // Auto Mix (As Dropped)
                             }
                             return false;
                         }
                         mirror.refreshNow();
                         const auto& m = model();
                         if (static_cast<int>(m.tracks.size()) < dropTracksBefore + 2) return false;
                         const auto& deckA = m.tracks[m.tracks.size() - 2];
                         const auto& deckB = m.tracks.back();
                         if (deckA.clips.size() != 1 || deckB.clips.size() != 1) return false;
                         const auto* itemA = m.findLibraryItem(deckA.clips[0].libraryItemId);
                         const auto* itemB = m.findLibraryItem(deckB.clips[0].libraryItemId);
                         const bool files = itemA != nullptr && itemB != nullptr && itemA->displayName().contains("drop-a")
                                            && itemB->displayName().contains("drop-b");
                         const bool joined = mix::recordFor(m, deckA.clips[0].id, deckB.clips[0].id) != nullptr;
                         lastTempoLine = "auto mix on drop: " + deckA.name + " / " + deckB.name + ", files " + (files ? "yes" : "no")
                                         + ", transition " + (joined ? "yes" : "no");
                         if (files && joined) report.add(lastTempoLine);
                         return files && joined;
                     }});
    steps.back().poll = true;
    steps.back().timeoutMs = 60000.0;
    // Stemcel's own sections find the drop and the breakdown of a generated arrangement.
    steps.push_back({"sections",
                     [this]
                     {
                         const auto file = workDir.getChildFile("Files").getChildFile("arrangement.wav");
                         if (!writeArrangement(file)) return;
                         ImportRequest request;
                         request.file = file;
                         request.hints.bpm = 120.0;
                         importer.import(std::vector<ImportRequest>{request}, [this](const juce::StringArray& ids)
                                         {
                                             sectionItemId = ids[0];
                                             sectionAnalyser = std::make_unique<SectionAnalyser>(engine, mirror, prefs);
                                             sectionAnalyser->analyse(sectionItemId, true);
                                         });
                     },
                     {},
                     [this](const juce::var&)
                     {
                         const auto* item = model().findLibraryItem(sectionItemId);
                         if (item == nullptr) return false;
                         const auto list = sections::of(*item);
                         if (list.empty()) return false;
                         juce::StringArray names;
                         for (const auto& s : list) names.add(s.label + "@" + juce::String(s.startSec, 0));
                         const bool found = names.joinIntoString(",").contains("Drop") && names.joinIntoString(",").contains("Breakdown");
                         lastTempoLine = "sections " + names.joinIntoString(", ");
                         if (found) report.add(lastTempoLine);
                         return found;
                     }});
    steps.back().poll = true;
    steps.back().timeoutMs = 60000.0;
    // The analysed arrangement on the timeline, for eyes: the strip along the top of its clip.
    steps.push_back({"sections picture",
                     [this]
                     {
                         const auto* item = model().findLibraryItem(sectionItemId);
                         if (item == nullptr) return;
                         const auto trackId = actions.addTrack();
                         sectionClipId = actions.addClip(trackId, *model().findLibraryItem(sectionItemId), model().contentEndMs() + 4000.0);
                         mirror.refreshNow();
                     },
                     {},
                     [this](const juce::var&)
                     {
                         // A timeline of its own over this project, scrolled to the clip.
                         const auto* clip = model().findClip(sectionClipId);
                         if (clip == nullptr) return false;
                         TimelineViewState view;
                         LiveState live(engine);
                         WaveformStore waveforms(engine.host().peaksCache());
                         TimelineView timeline(mirror, view, live, actions, waveforms);
                         timeline.setSize(1200, 420);
                         for (const auto& row : layoutRows(model()))
                             if (row.trackId == clip->trackId) view.scrollY = juce::jmax(0, row.top - 10);
                         view.pxPerSecond = 1000.0 / juce::jmax(1.0, clip->endMs() - clip->offsetMs) * 1100.0;
                         view.scrollXPx = view.msToX(clip->offsetMs) + view.scrollXPx - 40.0;
                         const auto image = timeline.createComponentSnapshot(timeline.getLocalBounds());
                         juce::FileOutputStream out(workDir.getChildFile("sections.png"));
                         if (out.openedOk()) juce::PNGImageFormat().writeImageToStream(image, out);
                         return true;
                     }});
    steps.back().poll = true;
    steps.push_back({"remove transition",
                     [this] { mix::remove(engine, actions, model(), mixTransitionId); },
                     {},
                     [this](const juce::var&)
                     {
                         if (const auto* list = model().raw.getProperty("mixTransitions", {}).getArray())
                             for (const auto& t : *list)
                                 if (t.getProperty("id", {}).toString() == mixTransitionId) return false;
                         report.add("transition removed");
                         return true;
                     }});
    steps.back().poll = true;
}

SmokeTest::~SmokeTest()
{
    stopTimer();
    reviewDialog.reset();
    midiActions.reset();
    midiLive.reset();
    if (subscription != 0) engine.unsubscribe(subscription);
    if (failureSubscription != 0) engine.unsubscribe(failureSubscription);
}

void SmokeTest::start()
{
    workDir.createDirectory();
    startTimer(250);
    runStep();
}

void SmokeTest::pass()
{
    report.add("PASS " + steps[current].name);
    ++current;
    delayDone = false;
    // Next step after the current broadcast finishes delivering.
    juce::MessageManager::callAsync([this] { runStep(); });
}

void SmokeTest::runStep()
{
    if (finished) return;
    if (current >= steps.size())
    {
        finish(true, "all steps passed");
        return;
    }
    const auto& step = steps[current];
    if (subscription != 0) engine.unsubscribe(subscription);
    subscription = 0;
    stepStartedMs = juce::Time::getMillisecondCounterHiRes();

    // The engine-ready broadcast may already have happened before the test started.
    if (step.awaitType == "ENGINE_AUDIO_STATUS" && !engine.latest("ENGINE_AUDIO_STATUS").isVoid())
    {
        pass();
        return;
    }

    if (step.delayMs > 0.0 && !delayDone)
    {
        delayDone = true;
        juce::Timer::callAfterDelay(static_cast<int>(step.delayMs), [this] { runStep(); });
        return;
    }

    if (step.awaitType.isEmpty() && step.poll)
    {
        step.act();
        polling = true; // timerCallback checks until it holds or times out
        return;
    }

    if (step.awaitType.isEmpty())
    {
        step.act();
        mirror.refreshNow();
        if (step.accept({}))
        {
            pass();
            return;
        }
        // Say what the project actually holds, so a failure explains itself.
        juce::StringArray state;
        for (const auto& t : model().tracks)
            for (const auto& c : t.clips)
                state.add(c.id.substring(0, 13) + " offset=" + juce::String(c.offsetMs) + " in=" + juce::String(c.inMs)
                          + " dur=" + juce::String(c.durationMs) + " eff=" + juce::String(c.effectiveDurationMs)
                          + " ratio=" + juce::String(c.effectiveTempoRatio) + (c.effectiveWarpActive ? " warp" : ""));
        report.add("state: " + state.joinIntoString("; ") + " right=" + rightClipId);
        if (const auto* t = model().findTrack(trackId))
            report.add("track: " + juce::JSON::toString(t->raw, true).substring(0, 600));
        finish(false, "check failed in step '" + step.name + "'");
        return;
    }

    subscription = engine.subscribe(step.awaitType,
                                    [this](const juce::var& payload)
                                    {
                                        if (finished || current >= steps.size()) return;
                                        if (steps[current].accept(payload)) pass();
                                    });
    step.act();
}

void SmokeTest::timerCallback()
{
    if (finished || current >= steps.size()) return;
    if (polling)
    {
        mirror.refreshNow();
        if (steps[current].accept({}))
        {
            polling = false;
            pass();
            return;
        }
    }
    const double limit = steps[current].timeoutMs > 0.0 ? steps[current].timeoutMs : kStepTimeoutMs;
    if (juce::Time::getMillisecondCounterHiRes() - stepStartedMs > limit)
    {
        if (polling)
            for (const auto& item : model().library)
                report.add("item " + item.id + " " + item.fileName + " bpm=" + juce::String(item.bpm, 3) + " anchor="
                           + juce::String(item.beatAnchorSec, 3) + " key=" + item.key + " sr="
                           + juce::String(item.sampleRate) + " ms=" + juce::String(item.durationMs) + " media=" + item.mediaId);
        if (lastTempoLine.isNotEmpty()) report.add(lastTempoLine);
        finish(false, "timed out waiting for " + steps[current].awaitType + " in step '" + steps[current].name + "'");
    }
}

void SmokeTest::finish(bool ok, const juce::String& message)
{
    if (finished) return;
    finished = true;
    stopTimer();
    report.add((ok ? "OK: " : "FAIL: ") + message);
    workDir.getChildFile("smoke-result.txt").replaceWithText(report.joinIntoString("\n") + "\n");
    log::info("smoke", report.joinIntoString(" | "));
    juce::JUCEApplicationBase::getInstance()->setApplicationReturnValue(ok ? 0 : 1);
    juce::JUCEApplicationBase::quit();
}

} // namespace stemcel::native
