#include "PerformanceCommands.h"

#include "AudioEngine.h"
#include "BridgeServer.h"
#include "DecodedCache.h"
#include "EnginePlaybackPath.h"
#include "Log.h"
#include "MidiDeviceCommands.h"
#include "ProjectState.h"
#include "performance/PerformanceEngine.h"
#include "performance/PerformanceMapping.h"
#include "DjFx.h"

#include <juce_events/juce_events.h>
#include <samplerate.h>

#include <map>
#include <memory>

namespace stemcel
{
namespace
{
using namespace perf;

juce::var cuesToVar(const std::array<Cue, kHotCues>& cues)
{
    juce::Array<juce::var> list;
    for (const auto& c : cues)
    {
        if (!c.set())
        {
            list.add(juce::var());
            continue;
        }
        auto* o = new juce::DynamicObject();
        o->setProperty("pos", c.pos);
        if (c.isLoop()) o->setProperty("end", c.end);
        list.add(juce::var(o));
    }
    return list;
}

juce::var memoryToVar(const std::array<Cue, kMemoryCues>& cues)
{
    juce::Array<juce::var> list;
    for (const auto& c : cues)
    {
        if (!c.set()) continue;
        auto* o = new juce::DynamicObject();
        o->setProperty("pos", c.pos);
        if (c.isLoop()) o->setProperty("end", c.end);
        list.add(juce::var(o));
    }
    return list;
}

template <size_t N>
void cuesFromVar(const juce::var& v, std::array<Cue, N>& out, bool compact)
{
    const auto* list = v.getArray();
    if (list == nullptr) return;
    size_t slot = 0;
    for (int i = 0; i < list->size() && slot < N; ++i)
    {
        const auto& item = (*list)[i];
        if (!item.isObject())
        {
            if (!compact) ++slot;
            continue;
        }
        Cue c;
        c.pos = static_cast<double>(item.getProperty("pos", -1.0));
        c.end = static_cast<double>(item.getProperty("end", -1.0));
        out[slot++] = c;
    }
}

// Reads a file whole into stereo at `rate` (worker thread).
std::shared_ptr<juce::AudioBuffer<float>> readWhole(const juce::File& file, juce::AudioFormatManager& formats,
                                                    double rate, juce::String& error)
{
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr)
    {
        error = "can't read " + file.getFileName();
        return nullptr;
    }
    const auto length = static_cast<int>(std::min<juce::int64>(reader->lengthInSamples, 60LL * 60 * 192000));
    juce::AudioBuffer<float> source(2, std::max(1, length));
    source.clear();
    reader->read(&source, 0, length, 0, true, reader->numChannels > 1);
    if (reader->numChannels == 1) source.copyFrom(1, 0, source, 0, 0, length);
    const double fileRate = reader->sampleRate > 0.0 ? reader->sampleRate : rate;
    if (std::abs(fileRate - rate) < 0.5) return std::make_shared<juce::AudioBuffer<float>>(std::move(source));

    // To the output's rate, so the deck plays it sample for sample.
    const double ratio = rate / fileRate;
    const auto outLength = static_cast<int>(std::ceil(length * ratio)) + 16;
    std::vector<float> interleavedIn(static_cast<size_t>(length) * 2), interleavedOut(static_cast<size_t>(outLength) * 2);
    for (int i = 0; i < length; ++i)
    {
        interleavedIn[static_cast<size_t>(i) * 2] = source.getSample(0, i);
        interleavedIn[static_cast<size_t>(i) * 2 + 1] = source.getSample(1, i);
    }
    SRC_DATA data{};
    data.data_in = interleavedIn.data();
    data.data_out = interleavedOut.data();
    data.input_frames = length;
    data.output_frames = outLength;
    data.src_ratio = ratio;
    if (const int err = src_simple(&data, SRC_SINC_MEDIUM_QUALITY, 2); err != 0)
    {
        error = src_strerror(err);
        return nullptr;
    }
    auto out = std::make_shared<juce::AudioBuffer<float>>(2, static_cast<int>(std::max<long>(1, data.output_frames_gen)));
    for (int i = 0; i < out->getNumSamples(); ++i)
    {
        out->setSample(0, i, interleavedOut[static_cast<size_t>(i) * 2]);
        out->setSample(1, i, interleavedOut[static_cast<size_t>(i) * 2 + 1]);
    }
    return out;
}

class PerformanceHost final : public MidiPerformanceSink, private juce::Timer
{
  public:
    PerformanceHost(AudioEngine& e, BridgeServer& b) : engine(e), bridge(b) {}
    ~PerformanceHost() override
    {
        stopTimer();
        setMidiPerformanceSink(nullptr);
    }

    perf::PerformanceEngine& perfEngine() { return engine.performanceEngine(); }

    void open()
    {
        if (perfEngine().isOpen()) return;
        perfEngine().setOpen(true);
        cueOutput = engine.setPerformanceOutputs(true);
        setMidiPerformanceSink(this);
        startTimerHz(60);
        auto* o = new juce::DynamicObject();
        o->setProperty("cueOutput", cueOutput);
        bridge.broadcast("PERF_OPENED", juce::var(o));
    }

    void close()
    {
        if (!perfEngine().isOpen()) return;
        flushRecording();
        perfEngine().setRecording(false);
        lightsOff();
        setMidiPerformanceSink(nullptr);
        perfEngine().setOpen(false);
        engine.setPerformanceOutputs(false);
        stopTimer();
        bridge.broadcast("PERF_CLOSED", juce::var(new juce::DynamicObject()));
    }

    // MidiPerformanceSink -------------------------------------------------------------------
    bool acceptsDevice(const juce::String& deviceName) const override
    {
        return perf::Mapping::forDevice(deviceName) != nullptr;
    }

    void deviceOpened(const juce::String& identifier, const juce::String& deviceName) override
    {
        auto mapping = perf::Mapping::forDevice(deviceName);
        if (mapping == nullptr) return;
        {
            const juce::SpinLock::ScopedLockType lock(devicesLock);
            auto& d = devices[identifier];
            d.mapping = mapping;
            d.translator = std::make_unique<perf::Mapping::Translator>(mapping);
            d.ledsSent.clear();
        }
        for (const auto& frame : mapping->initMessages())
            sendMidiToDevice(identifier, frame.data(), static_cast<int>(frame.size()));
        stemcel::log::info("performance", "controller " + deviceName + " → " + mapping->name());
    }

    void handleMidi(const juce::String& identifier, const juce::uint8* data, int size) override
    {
        perf::Event events[4];
        int count = 0;
        {
            const juce::SpinLock::ScopedLockType lock(devicesLock);
            const auto it = devices.find(identifier);
            if (it == devices.end() || it->second.translator == nullptr) return;
            count = it->second.translator->translate(data, size, events, 4);
        }
        noteMonitor(data, size, count > 0 ? events[0] : perf::Event{});
        for (int i = 0; i < count; ++i) route(events[i]);
    }

    // Tests: raw bytes as if from a controller named `deviceName`.
    void inject(const juce::String& deviceName, const std::vector<juce::uint8>& bytes)
    {
        const juce::String id = "inject:" + deviceName;
        {
            const juce::SpinLock::ScopedLockType lock(devicesLock);
            if (devices.find(id) == devices.end())
            {
                auto mapping = perf::Mapping::forDevice(deviceName);
                if (mapping == nullptr) return;
                auto& d = devices[id];
                d.mapping = mapping;
                d.translator = std::make_unique<perf::Mapping::Translator>(mapping);
                d.injected = true;
            }
        }
        for (size_t i = 0; i + 2 < bytes.size(); i += 3) handleMidi(id, bytes.data() + i, 3);
    }

    void route(const perf::Event& e)
    {
        if (perf::isAppControl(e.control))
        {
            const juce::SpinLock::ScopedLockType lock(appLock);
            appEvents.push_back(e);
            return;
        }
        if (e.control == perf::Control::padShift && e.mode == perf::PadMode::sampler && e.value > 0.5F)
        {
            auto load = e;
            load.control = perf::Control::samplerLoad;
            const juce::SpinLock::ScopedLockType lock(appLock);
            appEvents.push_back(load);
        }
        perfEngine().post(e);
    }

    // A sampler slot's sample: read whole on a worker, at the output's rate.
    void loadSample(const juce::var& payload, ProjectState& project, const DecodedCache& decoded, juce::ThreadPool& pool)
    {
        const int slot = static_cast<int>(payload.getProperty("slot", -1));
        if (slot < 0 || slot >= perf::kSamplerSlots) return;
        const auto itemId = payload.getProperty("libraryItemId", "").toString();
        if (itemId.isEmpty())
        {
            perfEngine().loadSample(slot, nullptr);
            return;
        }
        const auto sourcePath = project.getLibraryItemFilePath(itemId);
        if (sourcePath.isEmpty()) return;
        auto sample = std::make_shared<perf::DeckTrack>();
        sample->serial = ++serials;
        sample->itemId = itemId;
        sample->title = payload.getProperty("title", juce::File(sourcePath).getFileNameWithoutExtension()).toString();
        itemBySerial[sample->serial] = itemId;
        const auto playbackPath = resolveEnginePlaybackPath(sourcePath, project, decoded);
        const double rate = engine.getOutputSampleRate() > 0.0 ? engine.getOutputSampleRate() : 44100.0;
        auto* self = this;
        pool.addJob(
            [self, slot, sample, rate, playbackPath, sourcePath, &decoded]
            {
                auto& formats = self->engine.getFormatManager();
                auto file = juce::File(playbackPath);
                if (!file.existsAsFile() || file.hasFileExtension("mp3"))
                {
                    const auto built = decoded.ensureDecoded(juce::File(sourcePath), formats);
                    if (built.existsAsFile()) file = built;
                }
                juce::String error;
                auto audio = readWhole(file, formats, rate, error);
                juce::MessageManager::callAsync(
                    [self, slot, sample, audio, rate]
                    {
                        if (g_host.get() != self || audio == nullptr) return;
                        sample->audio = audio;
                        sample->sampleRate = rate;
                        self->perfEngine().loadSample(slot, sample);
                        auto* o = new juce::DynamicObject();
                        o->setProperty("slot", slot);
                        o->setProperty("libraryItemId", sample->itemId);
                        o->setProperty("title", sample->title);
                        o->setProperty("lengthSec", audio->getNumSamples() / rate);
                        self->bridge.broadcast("PERF_SAMPLE_LOADED", juce::var(o));
                    });
            });
    }

    // Loading ---------------------------------------------------------------------------------
    void load(const juce::var& payload, ProjectState& project, const DecodedCache& decoded, juce::ThreadPool& pool)
    {
        const int deck = static_cast<int>(payload.getProperty("deck", 0));
        if (deck < 1 || deck > perf::kDecks) return;
        const int fromDeck = static_cast<int>(payload.getProperty("fromDeck", 0));
        if (fromDeck >= 1 && fromDeck <= perf::kDecks)
        {
            // Instant Doubles: the other deck's track, where it is.
            const auto it = loadedTracks.find(fromDeck);
            if (it == loadedTracks.end() || it->second == nullptr) return;
            loadedTracks[deck] = it->second;
            perfEngine().load(deck, it->second, fromDeck);
            broadcastLoaded(deck, it->second);
            return;
        }
        const auto itemId = payload.getProperty("libraryItemId", "").toString();
        auto sourcePath = payload.getProperty("path", "").toString();
        if (sourcePath.isEmpty()) sourcePath = project.getLibraryItemFilePath(itemId);
        if (sourcePath.isEmpty())
        {
            broadcastFailed(deck, itemId, "unknown library item");
            return;
        }
        auto track = std::make_shared<perf::DeckTrack>();
        track->serial = ++serials;
        track->itemId = itemId;
        itemBySerial[track->serial] = itemId;
        track->title = payload.getProperty("title", juce::File(sourcePath).getFileNameWithoutExtension()).toString();
        track->artist = payload.getProperty("artist", "").toString();
        track->key = payload.getProperty("key", "").toString();
        track->bpm = payload.hasProperty("bpm") ? static_cast<double>(payload["bpm"]) : project.getLibraryItemBpm(itemId);
        track->anchorSec = payload.hasProperty("beatAnchorSec") ? static_cast<double>(payload["beatAnchorSec"])
                                                                 : project.getLibraryItemBeatAnchorSec(itemId);
        track->cueSec = static_cast<double>(payload.getProperty("cueSec", -1.0));
        cuesFromVar(payload.getProperty("hotCues", {}), track->hotCues, false);
        cuesFromVar(payload.getProperty("memoryCues", {}), track->memoryCues, true);
        stemcel::log::info("performance", "load deck " + juce::String(deck) + ": " + itemId);
        const auto playbackPath = resolveEnginePlaybackPath(sourcePath, project, decoded);
        const double rate = engine.getOutputSampleRate() > 0.0 ? engine.getOutputSampleRate() : 44100.0;
        const int request = ++loadRequests[deck];
        auto* self = this;
        pool.addJob(
            [self, deck, request, track, rate, playbackPath, sourcePath, &decoded]
            {
                auto& formats = self->engine.getFormatManager();
                auto file = juce::File(playbackPath);
                if (!file.existsAsFile() || file.hasFileExtension("mp3"))
                {
                    const auto built = decoded.ensureDecoded(juce::File(sourcePath), formats);
                    if (built.existsAsFile()) file = built;
                }
                juce::String error;
                stemcel::log::info("performance", "reading " + file.getFileName());
                auto audio = readWhole(file, formats, rate, error);
                stemcel::log::info("performance", "read " + juce::String(audio != nullptr ? audio->getNumSamples() : 0) + " samples");
                juce::MessageManager::callAsync(
                    [self, deck, request, track, audio, rate, error]
                    {
                        if (g_host.get() != self || self->loadRequests[deck] != request) return; // superseded
                        if (audio == nullptr)
                        {
                            self->broadcastFailed(deck, track->itemId, error);
                            return;
                        }
                        stemcel::log::info("performance", "loading deck " + juce::String(deck));
                        track->audio = audio;
                        track->sampleRate = rate;
                        self->loadedTracks[deck] = track;
                        self->perfEngine().load(deck, track, 0);
                        self->broadcastLoaded(deck, track);
                    });
            });
    }

    static std::unique_ptr<PerformanceHost> g_host;

  private:
    struct Device
    {
        std::shared_ptr<const perf::Mapping> mapping;
        std::unique_ptr<perf::Mapping::Translator> translator;
        std::map<int, int> ledsSent; // (status << 8 | data1) → value
        bool injected = false;
    };

    void broadcastLoaded(int deck, const std::shared_ptr<const perf::DeckTrack>& track)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("deck", deck);
        o->setProperty("ok", true);
        o->setProperty("libraryItemId", track->itemId);
        o->setProperty("serial", track->serial);
        o->setProperty("title", track->title);
        o->setProperty("artist", track->artist);
        o->setProperty("key", track->key);
        o->setProperty("bpm", track->bpm);
        o->setProperty("lengthSec", track->audio != nullptr ? track->audio->getNumSamples() / track->sampleRate : 0.0);
        bridge.broadcast("PERF_LOADED", juce::var(o));
        const juce::SpinLock::ScopedLockType lock(devicesLock);
        for (auto& [id, d] : devices) d.ledsSent.erase((0x9F << 8) | (deck - 1));
    }

    void broadcastFailed(int deck, const juce::String& itemId, const juce::String& error)
    {
        stemcel::log::warn("performance", "load deck " + juce::String(deck) + " failed: " + error);
        auto* o = new juce::DynamicObject();
        o->setProperty("deck", deck);
        o->setProperty("ok", false);
        o->setProperty("libraryItemId", itemId);
        o->setProperty("error", error);
        bridge.broadcast("PERF_LOADED", juce::var(o));
    }

    void noteMonitor(const juce::uint8* data, int size, const perf::Event& e)
    {
        juce::String text;
        for (int i = 0; i < std::min(size, 3); ++i) text << juce::String::toHexString(data[i]).paddedLeft('0', 2).toUpperCase() << " ";
        text = text.trim();
        if (e.control != perf::Control::none)
        {
            text << " → " << perf::controlName(e.control);
            if (e.deck > 0) text << " " << static_cast<int>(e.deck);
            if (e.control == perf::Control::pad || e.control == perf::Control::padShift)
                text << " " << perf::padModeName(e.mode) << " " << (e.index + 1);
            text << " = " << juce::String(e.value, 3);
        }
        else
            text << " → (not mapped)";
        const juce::SpinLock::ScopedLockType lock(appLock);
        monitor.add(text);
        while (monitor.size() > 8) monitor.remove(0);
        monitorChanged = true;
    }

    void timerCallback() override
    {
        auto& perfE = perfEngine();
        const auto& v = perfE.view();
        broadcastState(v);
        // Controls the app answers.
        std::vector<perf::Event> app;
        {
            const juce::SpinLock::ScopedLockType lock(appLock);
            app.swap(appEvents);
        }
        for (const auto& e : app)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("control", perf::controlName(e.control));
            o->setProperty("deck", static_cast<int>(e.deck));
            o->setProperty("value", e.value);
            o->setProperty("index", static_cast<int>(e.index));
            bridge.broadcast("PERF_UI", juce::var(o));
        }
        flushRecording();
        // Cues changed (set, deleted, stored): saved with the item by the app.
        for (int d = 0; d < perf::kDecks; ++d)
        {
            const auto& deck = v.decks[static_cast<size_t>(d)];
            const auto key = std::make_pair(deck.serial, deck.cueRevision);
            if (deck.serial == 0 || cueSeen[static_cast<size_t>(d)] == key) continue;
            const bool first = cueSeen[static_cast<size_t>(d)].first != deck.serial;
            cueSeen[static_cast<size_t>(d)] = key;
            if (first) continue;
            const auto it = loadedTracks.find(d + 1);
            auto* o = new juce::DynamicObject();
            o->setProperty("deck", d + 1);
            o->setProperty("libraryItemId", it != loadedTracks.end() && it->second != nullptr ? it->second->itemId : juce::String());
            o->setProperty("hotCues", cuesToVar(deck.hotCues));
            o->setProperty("memoryCues", memoryToVar(deck.memoryCues));
            bridge.broadcast("PERF_CUES", juce::var(o));
        }
        if (++tick % 2 == 0) updateLeds(v);
        if (tick % 60 == 0) perfE.collectGarbage();
    }

    void flushRecording()
    {
        recBuffer.clear();
        perfEngine().drainRecording(recBuffer);
        if (recBuffer.empty()) return;
        juce::Array<juce::var> list;
        for (const auto& r : recBuffer)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("at", static_cast<double>(r.at));
            o->setProperty("deck", static_cast<int>(r.deck));
            switch (r.kind)
            {
                case perf::RecEvent::Kind::segment:
                {
                    o->setProperty("kind", "segment");
                    o->setProperty("serial", r.serial);
                    const auto it = itemBySerial.find(r.serial);
                    o->setProperty("libraryItemId", it != itemBySerial.end() ? it->second : juce::String());
                    o->setProperty("endPos", r.a);
                    o->setProperty("startPos", r.b);
                    o->setProperty("tempo", r.tempo);
                    o->setProperty("semitones", r.semitones);
                    o->setProperty("moving", (r.flags & 1) != 0);
                    o->setProperty("audible", (r.flags & 2) != 0);
                    o->setProperty("keyLock", (r.flags & 4) != 0);
                    o->setProperty("scratch", (r.flags & 8) != 0);
                    break;
                }
                case perf::RecEvent::Kind::platter:
                    o->setProperty("kind", "platter");
                    o->setProperty("pos", r.a);
                    break;
                case perf::RecEvent::Kind::param:
                    o->setProperty("kind", "param");
                    o->setProperty("param", perf::recParamName(static_cast<perf::RecParam>(r.param)));
                    o->setProperty("value", r.a);
                    break;
                case perf::RecEvent::Kind::clockRate:
                    o->setProperty("kind", "start");
                    o->setProperty("sampleRate", r.a);
                    break;
                case perf::RecEvent::Kind::sample:
                {
                    o->setProperty("kind", "sample");
                    o->setProperty("slot", static_cast<int>(r.param));
                    o->setProperty("start", (r.flags & 1) != 0);
                    o->setProperty("lengthSec", r.a);
                    const auto it = itemBySerial.find(r.serial);
                    o->setProperty("libraryItemId", it != itemBySerial.end() ? it->second : juce::String());
                    break;
                }
            }
            list.add(juce::var(o));
        }
        auto* payload = new juce::DynamicObject();
        payload->setProperty("events", list);
        bridge.broadcast("PERF_REC", juce::var(payload));
    }

    void broadcastState(const perf::EngineView& v)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("open", v.open);
        o->setProperty("clock", static_cast<double>(v.clock));
        o->setProperty("sampleRate", v.sampleRate);
        o->setProperty("crossfader", v.crossfader);
        o->setProperty("masterDeck", v.masterDeck);
        o->setProperty("cueOutput", v.cueOutput);
        o->setProperty("recording", v.recording);
        std::array<float, perf::kDecks> peaks{};
        perfEngine().consumePeaks(peaks);
        juce::Array<juce::var> decks;
        for (int d = 0; d < perf::kDecks; ++d)
        {
            const auto& dv = v.decks[static_cast<size_t>(d)];
            const auto& cv = v.channels[static_cast<size_t>(d)];
            lastPeaks[static_cast<size_t>(d)] = peaks[static_cast<size_t>(d)];
            auto* x = new juce::DynamicObject();
            x->setProperty("serial", dv.serial);
            x->setProperty("loaded", dv.loaded);
            x->setProperty("lengthSec", dv.lengthSec);
            x->setProperty("posSec", dv.posSec);
            x->setProperty("cueSec", dv.cueSec);
            x->setProperty("playing", dv.playing);
            x->setProperty("previewing", dv.previewing);
            x->setProperty("atCue", dv.atCue);
            x->setProperty("loopActive", dv.loopActive);
            x->setProperty("loopArmed", dv.loopArmed);
            x->setProperty("loopInPending", dv.loopInPending);
            x->setProperty("loopInSec", dv.loopInSec);
            x->setProperty("loopOutSec", dv.loopOutSec);
            x->setProperty("loopAdjust", dv.loopAdjust);
            x->setProperty("sync", dv.sync);
            x->setProperty("master", dv.master);
            x->setProperty("keyLock", dv.keyLock);
            x->setProperty("quantize", dv.quantize);
            x->setProperty("touched", dv.touched);
            x->setProperty("slider", dv.slider);
            x->setProperty("range", dv.range);
            x->setProperty("tempo", dv.tempo);
            x->setProperty("bpm", dv.bpm);
            x->setProperty("effectiveBpm", dv.effectiveBpm);
            x->setProperty("rate", dv.rate);
            x->setProperty("semitones", dv.semitones);
            x->setProperty("hotCues", cuesToVar(dv.hotCues));
            x->setProperty("memoryCues", memoryToVar(dv.memoryCues));
            x->setProperty("trim", cv.trim);
            x->setProperty("eqHi", cv.eqHi);
            x->setProperty("eqMid", cv.eqMid);
            x->setProperty("eqLow", cv.eqLow);
            x->setProperty("filter", cv.filter);
            x->setProperty("fader", cv.fader);
            x->setProperty("hpCue", cv.hpCue);
            x->setProperty("padMode", perf::padModeName(cv.padMode));
            x->setProperty("beatJumpPage", cv.beatJumpPage);
            x->setProperty("keyPage", cv.keyPage);
            x->setProperty("keyboardCue", cv.keyboardCue);
            x->setProperty("padFxHeld", cv.padFxHeld);
            x->setProperty("peak", peaks[static_cast<size_t>(d)]);
            decks.add(juce::var(x));
        }
        o->setProperty("decks", decks);
        auto* fx = new juce::DynamicObject();
        fx->setProperty("type", v.fxType);
        fx->setProperty("typeName", djfx::beatFxName(v.fxType));
        fx->setProperty("step", v.fxStep);
        fx->setProperty("stepLabel", djfx::beatStepLabel(v.fxStep));
        fx->setProperty("channel", v.fxChannel);
        fx->setProperty("level", v.fxLevel);
        fx->setProperty("on", v.fxOn);
        fx->setProperty("tap", v.fxTapMode);
        fx->setProperty("bpm", v.fxBpm);
        fx->setProperty("colorType", v.colorType);
        fx->setProperty("colorName", djfx::colorFxName(v.colorType));
        fx->setProperty("releaseType", v.releaseType);
        o->setProperty("fx", juce::var(fx));
        auto* sampler = new juce::DynamicObject();
        sampler->setProperty("bank", v.samplerBank);
        sampler->setProperty("volume", v.samplerVolume);
        sampler->setProperty("loaded", juce::String::toHexString(static_cast<juce::int64>(v.samplesLoaded)));
        sampler->setProperty("playing", juce::String::toHexString(static_cast<juce::int64>(v.samplesPlaying)));
        o->setProperty("sampler", juce::var(sampler));
        {
            const juce::SpinLock::ScopedLockType lock(appLock);
            if (monitorChanged)
            {
                juce::Array<juce::var> m;
                for (const auto& s : monitor) m.add(s);
                o->setProperty("midi", m);
                monitorChanged = false;
            }
        }
        bridge.broadcast("PERF_STATE", juce::var(o));
    }

    // LEDs ------------------------------------------------------------------------------------
    static void setLed(Device& d, const juce::String& id, const juce::String& state, int deck, int index, int value)
    {
        for (const auto& led : d.mapping->leds(state, deck, index))
        {
            const int key = (led.status << 8) | led.data1;
            const auto it = d.ledsSent.find(key);
            if (it != d.ledsSent.end() && it->second == value) continue;
            d.ledsSent[key] = value;
            if (d.injected) continue;
            const juce::uint8 bytes[3] = {static_cast<juce::uint8>(led.status), static_cast<juce::uint8>(led.data1),
                                          static_cast<juce::uint8>(value)};
            sendMidiToDevice(id, bytes, 3);
        }
    }

    void updateLeds(const perf::EngineView& v)
    {
        const bool blink = (juce::Time::getMillisecondCounter() / 250) % 2 == 0;
        const juce::SpinLock::ScopedLockType lock(devicesLock);
        for (auto& [id, d] : devices)
        {
            if (d.mapping == nullptr) continue;
            for (int deck = 1; deck <= perf::kDecks; ++deck)
            {
                const auto& dv = v.decks[static_cast<size_t>(deck - 1)];
                const auto& cv = v.channels[static_cast<size_t>(deck - 1)];
                const auto on = [](bool b) { return b ? 0x7F : 0x00; };
                const bool paused = dv.loaded && !dv.playing && !dv.previewing;
                setLed(d, id, "play", deck, 0, on(dv.playing || (paused && dv.atCue && blink)));
                setLed(d, id, "cue", deck, 0, on(dv.loaded && (dv.playing || dv.previewing || (paused && (dv.atCue || blink)))));
                setLed(d, id, "sync", deck, 0, on(dv.sync));
                setLed(d, id, "loopIn", deck, 0, on(dv.loopActive ? (dv.loopAdjust != 1 || blink) : (dv.loopInPending && blink)));
                setLed(d, id, "loopOut", deck, 0, on(dv.loopActive && (dv.loopAdjust != 2 || blink)));
                setLed(d, id, "reloop", deck, 0, on(dv.loopActive || (dv.loopArmed && blink)));
                setLed(d, id, "hpCue", deck, 0, on(cv.hpCue));
                setLed(d, id, "loaded", deck, 0, on(dv.loaded));
                for (int m = 0; m < static_cast<int>(perf::PadMode::count); ++m)
                    setLed(d, id, juce::String("padMode.") + perf::padModeName(static_cast<perf::PadMode>(m)), deck, 0,
                           on(static_cast<int>(cv.padMode) == m));
                const auto modeName = juce::String("pad.") + perf::padModeName(cv.padMode);
                for (int pad = 0; pad < 8; ++pad)
                {
                    bool lit = false;
                    switch (cv.padMode)
                    {
                        case perf::PadMode::hotCue: lit = dv.hotCues[static_cast<size_t>(pad)].set(); break;
                        case perf::PadMode::beatLoop:
                        {
                            constexpr double lengths[8] = {0.25, 0.5, 1, 2, 4, 8, 16, 32};
                            const double beatSec = dv.bpm > 0.0 ? 60.0 / dv.bpm : 0.5;
                            lit = dv.loopActive && std::abs((dv.loopOutSec - dv.loopInSec) - lengths[pad] * beatSec) < 0.002;
                            break;
                        }
                        case perf::PadMode::beatJump: lit = dv.loaded; break;
                        case perf::PadMode::keyboard: lit = cv.keyboardCue < 0 ? dv.hotCues[static_cast<size_t>(pad)].set() : true; break;
                        case perf::PadMode::keyShift: lit = dv.loaded; break;
                        case perf::PadMode::sampler:
                        {
                            const int slot = v.samplerBank * 16 + (deck - 1) * 8 + pad;
                            const bool loaded = (v.samplesLoaded >> slot) & 1ULL;
                            const bool playing = (v.samplesPlaying >> slot) & 1ULL;
                            lit = playing ? blink : loaded;
                            break;
                        }
                        case perf::PadMode::padFx1:
                        case perf::PadMode::padFx2: lit = cv.padFxHeld == pad || cv.padFxHeld == pad + 8; break;
                        default: break;
                    }
                    setLed(d, id, modeName, deck, pad, on(lit));
                }
                // Channel meter: post-fader level, about −48 dB at the bottom.
                const float peak = lastPeaks[static_cast<size_t>(deck - 1)];
                const double db = peak > 1.0e-5F ? 20.0 * std::log10(peak) : -100.0;
                setLed(d, id, "meter", deck, 0, juce::jlimit(0, 127, static_cast<int>((db + 48.0) / 48.0 * 127.0)));
            }
            setLed(d, id, "fxOn", 0, 0, v.fxOn ? 0x7F : 0x00);
        }
    }

    void lightsOff()
    {
        const juce::SpinLock::ScopedLockType lock(devicesLock);
        for (auto& [id, d] : devices)
        {
            if (d.injected) continue;
            for (auto& [key, value] : d.ledsSent)
            {
                if (value == 0) continue;
                const juce::uint8 bytes[3] = {static_cast<juce::uint8>(key >> 8), static_cast<juce::uint8>(key & 0xFF), 0};
                sendMidiToDevice(id, bytes, 3);
            }
            d.ledsSent.clear();
        }
    }

    AudioEngine& engine;
    BridgeServer& bridge;
    bool cueOutput = false;
    juce::SpinLock devicesLock, appLock;
    std::map<juce::String, Device> devices;
    std::vector<perf::Event> appEvents;
    juce::StringArray monitor;
    bool monitorChanged = false;
    std::vector<perf::RecEvent> recBuffer;
    std::map<int, std::shared_ptr<const perf::DeckTrack>> loadedTracks;
    std::map<int, juce::String> itemBySerial; // every track loaded, for the take log
    std::map<int, int> loadRequests;
    std::array<std::pair<int, int>, perf::kDecks> cueSeen{};
    std::array<float, perf::kDecks> lastPeaks{};
    int serials = 0;
    int tick = 0;
};

std::unique_ptr<PerformanceHost> PerformanceHost::g_host;
auto& g_host = PerformanceHost::g_host;

PerformanceHost& host(AudioEngine& engine, BridgeServer& bridge)
{
    if (g_host == nullptr) g_host = std::make_unique<PerformanceHost>(engine, bridge);
    return *g_host;
}
} // namespace

bool dispatchPerformanceCommand(const juce::String& type, const juce::var& payload, AudioEngine& engine,
                                ProjectState& project, BridgeServer& bridge, const DecodedCache& decodedCache,
                                juce::ThreadPool& pool)
{
    if (!type.startsWith("PERF_")) return false;
    auto& h = host(engine, bridge);
    if (type == "PERF_OPEN") h.open();
    else if (type == "PERF_CLOSE") h.close();
    else if (type == "PERF_LOAD") h.load(payload, project, decodedCache, pool);
    else if (type == "PERF_SAMPLER_LOAD") h.loadSample(payload, project, decodedCache, pool);
    else if (type == "PERF_CAPTURE")
    {
        // Tests: a deck channel's live output, written to `path` (mono float WAV) when stopped.
        auto& perfEngine = engine.performanceEngine();
        if (static_cast<bool>(payload.getProperty("on", false)))
            perfEngine.startCapture(static_cast<int>(payload.getProperty("deck", 1)), static_cast<double>(payload.getProperty("seconds", 20.0)));
        else
        {
            std::vector<float> audio;
            juce::int64 startClock = 0;
            double rate = 44100.0;
            const juce::File file(payload.getProperty("path", "").toString());
            bool ok = perfEngine.stopCapture(audio, startClock, rate) && file.getFullPathName().isNotEmpty();
            if (ok)
            {
                file.deleteFile();
                juce::WavAudioFormat wav;
                std::unique_ptr<juce::AudioFormatWriter> writer(
                    wav.createWriterFor(new juce::FileOutputStream(file), rate, 1, 32, {}, 0));
                ok = writer != nullptr;
                if (ok)
                {
                    const float* channels[1] = {audio.data()};
                    writer->writeFromFloatArrays(channels, 1, static_cast<int>(audio.size()));
                }
            }
            auto* o = new juce::DynamicObject();
            o->setProperty("ok", ok);
            o->setProperty("path", file.getFullPathName());
            o->setProperty("startClock", static_cast<double>(startClock));
            o->setProperty("sampleRate", rate);
            o->setProperty("lengthSec", static_cast<double>(audio.size()) / rate);
            bridge.broadcast("PERF_CAPTURE_DONE", juce::var(o));
        }
    }
    else if (type == "PERF_RECORD") engine.performanceEngine().setRecording(static_cast<bool>(payload.getProperty("on", false)));
    else if (type == "PERF_CONTROL")
    {
        perf::Event e;
        e.control = perf::controlFromName(payload.getProperty("control", "").toString());
        if (e.control == perf::Control::none) return true;
        e.deck = static_cast<std::int16_t>(static_cast<int>(payload.getProperty("deck", 0)));
        e.index = static_cast<std::int16_t>(static_cast<int>(payload.getProperty("index", 0)));
        if (payload.hasProperty("mode"))
        {
            const auto mode = perf::padModeFromName(payload.getProperty("mode", "").toString());
            if (mode != perf::PadMode::count) e.mode = mode;
        }
        e.value = static_cast<float>(static_cast<double>(payload.getProperty("value", 1.0)));
        h.route(e);
    }
    else if (type == "PERF_MIDI_INJECT")
    {
        std::vector<juce::uint8> bytes;
        if (const auto* list = payload.getProperty("bytes", {}).getArray())
            for (const auto& b : *list) bytes.push_back(static_cast<juce::uint8>(static_cast<int>(b) & 0xFF));
        h.inject(payload.getProperty("device", "DDJ-400").toString(), bytes);
    }
    else
        stemcel::log::warn("performance", "unknown command " + type);
    return true;
}

void shutdownPerformance() { g_host.reset(); }

} // namespace stemcel
