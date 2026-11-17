#include "PerformanceCommands.h"

#include "AudioEngine.h"
#include "BridgeServer.h"
#include "DecodedCache.h"
#include "EnginePlaybackPath.h"
#include "Log.h"
#include "LoudnessAnalyzer.h"
#include "MidiDeviceCommands.h"
#include "ProjectState.h"
#include "performance/PerformanceEngine.h"
#include "performance/PerformanceMapping.h"
#include "DjFx.h"

#include <juce_events/juce_events.h>
#include <samplerate.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <optional>

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

// A track's audio read in pieces at the output's rate, so a deck can start on the first
// seconds while the rest arrives (and a track can be read ahead while the browser rests on
// it). The worker fills `buffer` and moves `ready` on; the message thread watches the flags.
struct StreamedAudio
{
    juce::String sourcePath;
    double rate = 44100.0;
    std::shared_ptr<juce::AudioBuffer<float>> buffer; // set before `allocated`
    std::shared_ptr<std::atomic<int>> ready = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<int>> total = std::make_shared<std::atomic<int>>(0);
    std::atomic<bool> allocated{false}, complete{false}, failed{false}, cancelled{false};
    double lufs = std::numeric_limits<double>::quiet_NaN(); // set before `complete`
    juce::String error;                                      // set before `failed`
};

// Reads `file` into `s` (worker thread).
void streamAudio(StreamedAudio& s, const juce::File& file, juce::AudioFormatManager& formats)
{
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr)
    {
        s.error = "can't read " + file.getFileName();
        s.failed.store(true, std::memory_order_release);
        return;
    }
    const auto length = static_cast<int>(std::min<juce::int64>(reader->lengthInSamples, 60LL * 60 * 192000));
    const double fileRate = reader->sampleRate > 0.0 ? reader->sampleRate : s.rate;
    const bool convert = std::abs(fileRate - s.rate) >= 0.5;
    const double ratio = s.rate / fileRate;
    const int capacity = std::max(1, convert ? static_cast<int>(std::ceil(length * ratio)) + 64 : length);
    auto buffer = std::make_shared<juce::AudioBuffer<float>>(2, capacity);
    s.buffer = buffer;
    s.total->store(convert ? static_cast<int>(std::round(length * ratio)) : length, std::memory_order_release);
    s.allocated.store(true, std::memory_order_release);

    constexpr int kChunk = 1 << 16; // about 1.4 s
    juce::AudioBuffer<float> in(2, kChunk);
    int error = 0;
    // The fastest sinc: about four times the medium one's speed, still 97 dB clean, and the
    // deck interpolates again as it plays anyway.
    SRC_STATE* src = convert ? src_new(SRC_SINC_FASTEST, 2, &error) : nullptr;
    std::vector<float> interIn(static_cast<size_t>(kChunk) * 2);
    const int outCap = static_cast<int>(std::ceil(kChunk * ratio)) + 256;
    std::vector<float> interOut(static_cast<size_t>(outCap) * 2);
    int written = 0;
    const auto put = [&](const float* inter, int frames)
    {
        frames = std::min(frames, capacity - written);
        auto* l = buffer->getWritePointer(0, written);
        auto* r = buffer->getWritePointer(1, written);
        for (int i = 0; i < frames; ++i)
        {
            l[i] = inter[static_cast<size_t>(i) * 2];
            r[i] = inter[static_cast<size_t>(i) * 2 + 1];
        }
        written += frames;
    };
    const auto resample = [&](int frames, bool last)
    {
        int used = 0;
        for (;;)
        {
            SRC_DATA d{};
            d.data_in = interIn.data() + static_cast<size_t>(used) * 2;
            d.input_frames = frames - used;
            d.data_out = interOut.data();
            d.output_frames = outCap;
            d.src_ratio = ratio;
            d.end_of_input = last ? 1 : 0;
            if (src_process(src, &d) != 0) return;
            put(interOut.data(), static_cast<int>(d.output_frames_gen));
            used += static_cast<int>(d.input_frames_used);
            if (d.output_frames_gen == 0 && (used >= frames || d.input_frames_used == 0)) return;
        }
    };
    for (int at = 0; at < length && !s.cancelled.load(std::memory_order_relaxed); at += kChunk)
    {
        const int n = std::min(kChunk, length - at);
        reader->read(&in, 0, n, at, true, reader->numChannels > 1);
        if (reader->numChannels == 1) in.copyFrom(1, 0, in, 0, 0, n);
        if (convert && src != nullptr)
        {
            for (int i = 0; i < n; ++i)
            {
                interIn[static_cast<size_t>(i) * 2] = in.getSample(0, i);
                interIn[static_cast<size_t>(i) * 2 + 1] = in.getSample(1, i);
            }
            resample(n, at + n >= length);
        }
        else
        {
            const int frames = std::min(n, capacity - written);
            buffer->copyFrom(0, written, in, 0, 0, frames);
            buffer->copyFrom(1, written, in, 1, 0, frames);
            written += frames;
        }
        s.ready->store(written, std::memory_order_release);
    }
    if (src != nullptr) src_delete(src);
    if (s.cancelled.load())
    {
        s.error = "cancelled";
        s.failed.store(true, std::memory_order_release);
        return;
    }
    for (int ch = 0; ch < 2; ++ch) buffer->clear(ch, written, capacity - written);
    s.total->store(written, std::memory_order_release);
    s.ready->store(written, std::memory_order_release);
    // Its loudness, for auto gain (the analyser takes the two usual rates).
    if (std::abs(s.rate - 44100.0) < 1.0 || std::abs(s.rate - 48000.0) < 1.0)
    {
        LoudnessAnalyzer loudness(s.rate);
        constexpr int block = 1 << 15;
        for (int at = 0; at < written; at += block)
        {
            const int n = std::min(block, written - at);
            const float* channels[2] = {buffer->getReadPointer(0, at), buffer->getReadPointer(1, at)};
            loudness.process(channels, 2, n);
        }
        const auto result = loudness.finalize();
        if (!result.silent) s.lufs = result.integratedLufs;
    }
    s.complete.store(true, std::memory_order_release);
}

// Stem Mix: a deck's track's stems read for the deck (worker thread). Each stem is read at
// the track's rate, cut or padded to its length and kept as 16-bit samples scaled to its own
// peak. With `instPath`, the instrumental (the track less the three) is written there too, so
// the timeline has it as a stem of its own for takes made with stems out.
struct StemLoad
{
    int deck = 0, serial = 0;
    std::shared_ptr<const perf::DeckTrack> track;
    std::array<juce::String, perf::StemSet::kParts> paths; // vocals, drums, bass
    juce::String instPath;
    std::shared_ptr<perf::StemSet> set;
    juce::String error;
    std::atomic<bool> done{false}, failed{false};
};

void readStems(StemLoad& load, juce::AudioFormatManager& formats)
{
    const auto& track = *load.track;
    // The track's own read finishes first: its length is the stems' length.
    for (int waited = 0; track.readyFrames != nullptr && track.totalFrames != nullptr &&
                         track.readyFrames->load() < track.totalFrames->load() && waited < 3000;
         ++waited)
        juce::Thread::sleep(50);
    const int frames = track.totalFrames != nullptr ? track.totalFrames->load() : track.audio->getNumSamples();
    auto set = std::make_shared<perf::StemSet>();
    set->frames = std::min(frames, track.audio->getNumSamples());
    std::array<juce::AudioBuffer<float>, perf::StemSet::kParts> floats; // kept only for the instrumental
    for (size_t part = 0; part < perf::StemSet::kParts; ++part)
    {
        if (load.paths[part].isEmpty()) continue; // a two-stem split: no bass or drums
        set->present[part] = true;
        StreamedAudio stream;
        stream.rate = track.sampleRate;
        streamAudio(stream, juce::File(load.paths[part]), formats);
        if (!stream.complete.load() || stream.buffer == nullptr)
        {
            load.error = "can't read the stem " + juce::File(load.paths[part]).getFileName();
            load.failed.store(true, std::memory_order_release);
            return;
        }
        const auto& audio = *stream.buffer;
        const int n = std::min(set->frames, stream.total->load());
        float peak = 1.0e-6F;
        for (int ch = 0; ch < 2; ++ch) peak = std::max(peak, audio.getMagnitude(ch, 0, n));
        const float step = peak / 32767.0F;
        set->scale[part] = step;
        for (size_t ch = 0; ch < 2; ++ch)
        {
            auto& out = set->data[part][ch];
            out.assign(static_cast<size_t>(set->frames), 0);
            const float* in = audio.getReadPointer(static_cast<int>(ch));
            for (int i = 0; i < n; ++i)
                out[static_cast<size_t>(i)] = static_cast<std::int16_t>(std::lround(juce::jlimit(-32767.0F, 32767.0F, in[i] / step)));
        }
        if (load.instPath.isNotEmpty()) floats[part].makeCopyOf(audio);
    }
    if (load.instPath.isNotEmpty())
    {
        // The instrumental: the track less the vocals, drums and bass (less the vocals alone for a
        // two-stem split), as 24-bit WAV.
        juce::AudioBuffer<float> inst(2, set->frames);
        for (int ch = 0; ch < 2; ++ch)
        {
            inst.copyFrom(ch, 0, *track.audio, std::min(ch, track.audio->getNumChannels() - 1), 0, set->frames);
            for (const auto& part : floats)
                if (part.getNumSamples() > 0) inst.addFrom(ch, 0, part, ch, 0, std::min(set->frames, part.getNumSamples()), -1.0F);
        }
        const juce::File file(load.instPath);
        file.getParentDirectory().createDirectory();
        file.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(new juce::FileOutputStream(file), track.sampleRate, 2, 24, {}, 0));
        if (writer == nullptr || !writer->writeFromAudioSampleBuffer(inst, 0, inst.getNumSamples()))
            load.instPath.clear();
    }
    load.set = std::move(set);
    load.done.store(true, std::memory_order_release);
}

class PerformanceHost final : public MidiPerformanceSink, private juce::Timer
{
  public:
    PerformanceHost(AudioEngine& e, BridgeServer& b) : engine(e), bridge(b) {}
    ~PerformanceHost() override
    {
        for (auto& [key, stream] : streams) stream->cancelled.store(true);
        loadPool.removeAllJobs(true, 4000);
        stopTimer();
        setMidiPerformanceSink(nullptr);
    }

    perf::PerformanceEngine& perfEngine() { return engine.performanceEngine(); }

    void open()
    {
        if (!perfEngine().isOpen())
        {
            perfEngine().setOpen(true);
            cueDevice = engine.openPerformanceCue(perf::Mapping::allModels());
            cueOutput = cueDevice.isNotEmpty();
            setMidiPerformanceSink(this);
            startTimerHz(60);
        }
        auto* o = new juce::DynamicObject();
        o->setProperty("cueOutput", cueOutput);
        o->setProperty("cueDevice", cueDevice);
        bridge.broadcast("PERF_OPENED", juce::var(o));
        // What the decks and the sampler still hold from last time, for the view to show.
        for (const auto& [deck, track] : loadedTracks)
            if (track != nullptr) broadcastLoaded(deck, track);
        for (const auto& [slot, sample] : loadedSamples)
            if (sample != nullptr)
            {
                auto* s = new juce::DynamicObject();
                s->setProperty("slot", slot);
                s->setProperty("libraryItemId", sample->itemId);
                s->setProperty("title", sample->title);
                bridge.broadcast("PERF_SAMPLE_LOADED", juce::var(s));
            }
    }

    void close()
    {
        if (!perfEngine().isOpen()) return;
        flushRecording();
        perfEngine().setRecording(false);
        lightsOff();
        setMidiPerformanceSink(nullptr);
        perfEngine().setOpen(false);
        engine.closePerformanceCue();
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
            for (int i = 0; i < count; ++i) events[i].deck = static_cast<std::int16_t>(layered(*it->second.mapping, events[i].deck));
        }
        noteMonitor(data, size, count > 0 ? events[0] : perf::Event{});
        for (int i = 0; i < count; ++i) route(events[i]);
    }

    // PERF_DECK_LAYER {side: 1 | 2, on}: a two-deck controller's side on deck 3 or 4.
    void setLayer(const juce::var& p)
    {
        const int side = static_cast<int>(p.getProperty("side", 0));
        if (side < 1 || side > 2) return;
        layers[static_cast<size_t>(side - 1)].store(static_cast<bool>(p.getProperty("on", false)));
    }

    // The deck a controller's deck `deck` drives: with two decks of its own and that side's
    // layer on, the one behind it (1 → 3, 2 → 4).
    int layered(const perf::Mapping& mapping, int deck) const
    {
        if (mapping.deckCount() > 2 || deck < 1 || deck > 2) return deck;
        return layers[static_cast<size_t>(deck - 1)].load() ? deck + 2 : deck;
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
        if (e.control == perf::Control::channelBind && e.deck >= 1 && e.deck <= perf::kDecks)
            boundDecks[static_cast<size_t>(e.deck - 1)] = e.value > 0.5F;
        // A channel driving a timeline track: its transport and jog work the timeline (the
        // app), its deck's own controls rest; the mixer, Pad FX and sampler work as ever.
        if (e.deck >= 1 && e.deck <= perf::kDecks && boundDecks[static_cast<size_t>(e.deck - 1)])
        {
            switch (e.control)
            {
                case perf::Control::play:
                case perf::Control::playShift:
                case perf::Control::cue:
                case perf::Control::cueShift:
                case perf::Control::jogScratch:
                case perf::Control::jogBend:
                case perf::Control::jogSearch:
                {
                    const juce::SpinLock::ScopedLockType lock(appLock);
                    appEvents.push_back(e);
                    return;
                }
                case perf::Control::sync:
                case perf::Control::syncLong:
                case perf::Control::tempo:
                case perf::Control::loopIn:
                case perf::Control::loopInLong:
                case perf::Control::loopOut:
                case perf::Control::reloopExit:
                case perf::Control::callPrev:
                case perf::Control::callNext:
                {
                    // Not the deck's while it drives a track, but the app hears them: in a redo,
                    // touching one takes the deck over from the take.
                    const juce::SpinLock::ScopedLockType lock(appLock);
                    appEvents.push_back(e);
                    return;
                }
                case perf::Control::syncShift:
                case perf::Control::reloopShift:
                case perf::Control::callPrevShift:
                case perf::Control::callNextShift:
                case perf::Control::jogTouch:
                case perf::Control::faderStartPlay:
                case perf::Control::faderStartCue:
                    return;
                case perf::Control::pad:
                case perf::Control::padShift:
                    if (e.mode != perf::PadMode::padFx1 && e.mode != perf::PadMode::padFx2 && e.mode != perf::PadMode::sampler)
                    {
                        const juce::SpinLock::ScopedLockType lock(appLock);
                        appEvents.push_back(e);
                        return;
                    }
                    break;
                default:
                    break;
            }
        }
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
                        self->loadedSamples[slot] = sample;
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
        if (const auto* beats = payload.getProperty("beats", {}).getArray())
            for (const auto& b : *beats) track->beats.push_back(static_cast<double>(b));
        cuesFromVar(payload.getProperty("hotCues", {}), track->hotCues, false);
        cuesFromVar(payload.getProperty("memoryCues", {}), track->memoryCues, true);
        stemcel::log::info("performance", "load deck " + juce::String(deck) + ": " + (itemId.isNotEmpty() ? itemId : sourcePath));
        track->sourcePath = sourcePath;
        // The collection may know its loudness already.
        if (payload.hasProperty("lufs"))
        {
            track->hasLoudness = true;
            track->loudnessLufs = static_cast<double>(payload["lufs"]);
        }
        auto stream = startStream(sourcePath, project, decoded);
        pendingLoads[static_cast<size_t>(deck - 1)] = PendingLoad{stream, track, ++loadRequests[deck], false};
        pollLoads();
    }

    // Reads a track ahead (the browser rests on it), so a LOAD of it is instant.
    void preload(const juce::var& payload, ProjectState& project, const DecodedCache& decoded)
    {
        auto sourcePath = payload.getProperty("path", "").toString();
        if (sourcePath.isEmpty()) sourcePath = project.getLibraryItemFilePath(payload.getProperty("libraryItemId", "").toString());
        if (sourcePath.isNotEmpty() && juce::File(sourcePath).existsAsFile()) startStream(sourcePath, project, decoded);
    }

    // A deck loaded straight from a file has its library item once the import ends.
    void attachItem(const juce::var& payload)
    {
        const auto path = payload.getProperty("path", "").toString();
        const auto itemId = payload.getProperty("libraryItemId", "").toString();
        if (path.isEmpty() || itemId.isEmpty()) return;
        for (auto& [deck, track] : loadedTracks)
            if (track != nullptr && track->itemId.isEmpty() && track->sourcePath == path)
            {
                // The item id is the message thread's alone (the audio thread never reads it).
                const_cast<perf::DeckTrack&>(*track).itemId = itemId; // NOLINT(cppcoreguidelines-pro-type-const-cast)
                itemBySerial[track->serial] = itemId;
                auto* o = new juce::DynamicObject();
                o->setProperty("deck", deck);
                o->setProperty("serial", track->serial);
                o->setProperty("libraryItemId", itemId);
                bridge.broadcast("PERF_ITEM_ATTACHED", juce::var(o));
            }
        for (auto& pending : pendingLoads)
            if (pending.has_value() && pending->track->itemId.isEmpty() && pending->track->sourcePath == path)
            {
                pending->track->itemId = itemId;
                itemBySerial[pending->track->serial] = itemId;
            }
    }

    static std::unique_ptr<PerformanceHost> g_host;

    // PERF_STEMS {deck, serial, vocals, drums, bass, instPath?}: the deck's track's stems.
    void stems(const juce::var& p)
    {
        const int deck = static_cast<int>(p.getProperty("deck", 0));
        const int serial = static_cast<int>(p.getProperty("serial", 0));
        const auto it = loadedTracks.find(deck);
        if (it == loadedTracks.end() || it->second == nullptr || it->second->serial != serial || it->second->audio == nullptr)
        {
            broadcastStems(deck, serial, false, "the deck has another track now", {});
            return;
        }
        auto load = std::make_shared<StemLoad>();
        load->deck = deck;
        load->serial = serial;
        load->track = it->second;
        load->paths = {p.getProperty("vocals", "").toString(), p.getProperty("drums", "").toString(), p.getProperty("bass", "").toString()};
        load->instPath = p.getProperty("instPath", "").toString();
        stemLoads.push_back(load);
        auto& formats = engine.getFormatManager();
        loadPool.addJob([load, &formats] { readStems(*load, formats); });
    }

  private:
    struct PendingLoad
    {
        std::shared_ptr<StreamedAudio> stream;
        std::shared_ptr<perf::DeckTrack> track;
        int request = 0;
        bool published = false; // on the deck (playable as far as it has read)
    };

    // The stream for a file at the output's rate: one already reading (a preload, the other
    // deck's), or a new one.
    std::shared_ptr<StreamedAudio> startStream(const juce::String& sourcePath, ProjectState& project, const DecodedCache& decoded)
    {
        const double rate = engine.getOutputSampleRate() > 0.0 ? engine.getOutputSampleRate() : 44100.0;
        const auto key = sourcePath + "@" + juce::String(rate);
        if (const auto it = streams.find(key); it != streams.end() && !it->second->failed.load())
        {
            streamOrder.removeString(key);
            streamOrder.add(key);
            return it->second;
        }
        auto stream = std::make_shared<StreamedAudio>();
        stream->sourcePath = sourcePath;
        stream->rate = rate;
        streams[key] = stream;
        streamOrder.removeString(key);
        streamOrder.add(key);
        // Keep the decks' and the last few read ahead; stop reading the rest.
        while (streamOrder.size() > 4)
        {
            const auto oldest = streamOrder[0];
            streamOrder.remove(0);
            if (const auto it = streams.find(oldest); it != streams.end())
            {
                const bool onDeck = std::any_of(pendingLoads.begin(), pendingLoads.end(),
                                                [&](const auto& p) { return p.has_value() && p->stream == it->second; });
                if (!onDeck && !it->second->complete.load()) it->second->cancelled.store(true);
                streams.erase(it);
            }
        }
        const auto playbackPath = resolveEnginePlaybackPath(sourcePath, project, decoded);
        auto& formats = engine.getFormatManager();
        loadPool.addJob(
            [stream, playbackPath, sourcePath, &decoded, &formats]
            {
                if (stream->cancelled.load()) return;
                auto file = juce::File(playbackPath);
                if (!file.existsAsFile() || file.hasFileExtension("mp3"))
                {
                    const auto built = decoded.ensureDecoded(juce::File(sourcePath), formats);
                    if (built.existsAsFile()) file = built;
                }
                const auto started = juce::Time::getMillisecondCounterHiRes();
                streamAudio(*stream, file, formats);
                stemcel::log::info("performance", "read " + file.getFileName() + " in "
                                                      + juce::String(juce::Time::getMillisecondCounterHiRes() - started, 0) + " ms");
            });
        return stream;
    }

    void pollStems()
    {
        for (auto it = stemLoads.begin(); it != stemLoads.end();)
        {
            auto& load = **it;
            if (load.failed.load(std::memory_order_acquire))
            {
                broadcastStems(load.deck, load.serial, false, load.error, {});
                it = stemLoads.erase(it);
                continue;
            }
            if (!load.done.load(std::memory_order_acquire))
            {
                ++it;
                continue;
            }
            perfEngine().attachStems(load.deck, load.serial, load.set);
            broadcastStems(load.deck, load.serial, true, {}, load.instPath);
            it = stemLoads.erase(it);
        }
    }

    void broadcastStems(int deck, int serial, bool ok, const juce::String& error, const juce::String& instPath)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("deck", deck);
        o->setProperty("serial", serial);
        o->setProperty("ok", ok);
        if (error.isNotEmpty()) o->setProperty("error", error);
        if (instPath.isNotEmpty()) o->setProperty("instPath", instPath);
        bridge.broadcast("PERF_STEMS_READY", juce::var(o));
    }

    std::vector<std::shared_ptr<StemLoad>> stemLoads;

    // On the message thread (the state timer, and a load): a deck takes its track once the
    // first seconds are in, and hears its loudness when the read ends.
    void pollLoads()
    {
        pollStems();
        for (int d = 1; d <= perf::kDecks; ++d)
        {
            auto& pending = pendingLoads[static_cast<size_t>(d - 1)];
            if (!pending.has_value()) continue;
            auto& s = *pending->stream;
            auto& track = pending->track;
            if (s.failed.load(std::memory_order_acquire))
            {
                broadcastFailed(d, track->itemId, s.error);
                pending.reset();
                continue;
            }
            const bool complete = s.complete.load(std::memory_order_acquire);
            if (!pending->published && s.allocated.load(std::memory_order_acquire))
            {
                const int enough = std::min(s.total->load(), static_cast<int>(s.rate * 4.0));
                if (complete || s.ready->load(std::memory_order_acquire) >= enough)
                {
                    track->audio = s.buffer;
                    track->sampleRate = s.rate;
                    if (!complete)
                    {
                        track->readyFrames = s.ready;
                        track->totalFrames = s.total;
                    }
                    loadedTracks[d] = track;
                    perfEngine().load(d, track, 0);
                    broadcastLoaded(d, track);
                    pending->published = true;
                }
            }
            if (pending->published && complete)
            {
                if (!track->hasLoudness && std::isfinite(s.lufs))
                {
                    track->hasLoudness = true;
                    track->loudnessLufs = s.lufs;
                }
                auto* o = new juce::DynamicObject();
                o->setProperty("deck", d);
                o->setProperty("serial", track->serial);
                o->setProperty("lengthSec", s.total->load() / s.rate);
                if (track->hasLoudness) o->setProperty("lufs", track->loudnessLufs);
                bridge.broadcast("PERF_LOAD_COMPLETE", juce::var(o));
                pending.reset();
            }
        }
    }

    std::array<std::optional<PendingLoad>, perf::kDecks> pendingLoads;
    std::map<juce::String, std::shared_ptr<StreamedAudio>> streams; // by path and rate
    juce::StringArray streamOrder;                                   // oldest first
    juce::ThreadPool loadPool{2};                                    // not behind peak jobs
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
        const double frames = track->totalFrames != nullptr ? track->totalFrames->load()
                              : track->audio != nullptr ? track->audio->getNumSamples() : 0;
        o->setProperty("lengthSec", frames / track->sampleRate);
        o->setProperty("path", track->sourcePath);
        o->setProperty("anchorSec", track->anchorSec);
        juce::Array<juce::var> beats;
        for (const double b : track->beats) beats.add(b);
        o->setProperty("beats", beats);
        if (track->hasLoudness) o->setProperty("lufs", track->loudnessLufs);
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
        pollLoads();
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
                    o->setProperty("stemsOff", (r.flags >> 4) & 0xF);
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
        juce::Array<juce::var> layerList{layers[0].load(), layers[1].load()};
        o->setProperty("layers", layerList);
        o->setProperty("cueOutput", v.cueOutput);
        o->setProperty("cueDevice", cueDevice);
        o->setProperty("recording", v.recording);
        std::array<std::array<float, 2>, perf::kDecks> peaks{};
        perfEngine().consumePeaks(peaks);
        juce::Array<juce::var> decks;
        for (int d = 0; d < perf::kDecks; ++d)
        {
            const auto& dv = v.decks[static_cast<size_t>(d)];
            const auto& cv = v.channels[static_cast<size_t>(d)];
            const auto& sides = peaks[static_cast<size_t>(d)];
            lastPeaks[static_cast<size_t>(d)] = std::max(sides[0], sides[1]);
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
            x->setProperty("hasStems", dv.hasStems);
            x->setProperty("stemsOff", dv.stemsOff);
            x->setProperty("stemsPresent", dv.stemsPresent);
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
            x->setProperty("bound", cv.bound);
            x->setProperty("peak", std::max(sides[0], sides[1]));
            x->setProperty("peakL", sides[0]);
            x->setProperty("peakR", sides[1]);
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
            // Lit and dark are the controller's own values (a meter's level passes through).
            const int sent = value == 0x7F ? led.on : value == 0 ? led.off : value;
            const int key = (led.status << 8) | led.data1;
            const auto it = d.ledsSent.find(key);
            if (it != d.ledsSent.end() && it->second == sent) continue;
            d.ledsSent[key] = sent;
            if (d.injected) continue;
            const juce::uint8 bytes[3] = {static_cast<juce::uint8>(led.status), static_cast<juce::uint8>(led.data1),
                                          static_cast<juce::uint8>(sent)};
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
                // The controller's deck lights show the deck it drives (its layer's).
                const int source = layered(*d.mapping, deck);
                const auto& dv = v.decks[static_cast<size_t>(source - 1)];
                const auto& cv = v.channels[static_cast<size_t>(source - 1)];
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
                // Stem Mix shows as SAMPLER blinking (the controller is in its sampler mode, and
                // its pads light the stems that are in).
                const bool stemMix = cv.padMode == perf::PadMode::stems;
                for (int m = 0; m < static_cast<int>(perf::PadMode::stems); ++m)
                    setLed(d, id, juce::String("padMode.") + perf::padModeName(static_cast<perf::PadMode>(m)), deck, 0,
                           on(static_cast<int>(cv.padMode) == m || (stemMix && m == static_cast<int>(perf::PadMode::sampler) && blink)));
                const auto modeName = juce::String("pad.") + perf::padModeName(stemMix ? perf::PadMode::sampler : cv.padMode);
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
                            const int slot = v.samplerBank * 16 + ((deck - 1) % 2) * 8 + pad;
                            const bool loaded = (v.samplesLoaded >> slot) & 1ULL;
                            const bool playing = (v.samplesPlaying >> slot) & 1ULL;
                            lit = playing ? blink : loaded;
                            break;
                        }
                        case perf::PadMode::padFx1:
                        case perf::PadMode::padFx2: lit = cv.padFxHeld == pad || cv.padFxHeld == pad + 8; break;
                        case perf::PadMode::stems:
                        {
                            // 1–4: the stem is in; 5–8: it plays alone. Blinking: no stems yet.
                            const int bit = 1 << (pad % perf::kStems);
                            if (!dv.hasStems) lit = pad < perf::kStems && blink;
                            else if ((dv.stemsPresent & bit) == 0) lit = false; // not in a two-stem split
                            else lit = pad < perf::kStems ? (dv.stemsOff & bit) == 0 : dv.stemsOff == (dv.stemsPresent & ~bit);
                            break;
                        }
                        default: break;
                    }
                    setLed(d, id, modeName, deck, pad, on(lit));
                }
                // Channel meter: post-fader level, about −48 dB at the bottom.
                const float peak = lastPeaks[static_cast<size_t>(source - 1)];
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
    juce::String cueDevice; // where the headphone cue goes
    juce::SpinLock devicesLock, appLock;
    std::map<juce::String, Device> devices;
    std::vector<perf::Event> appEvents;
    juce::StringArray monitor;
    bool monitorChanged = false;
    std::vector<perf::RecEvent> recBuffer;
    std::map<int, std::shared_ptr<const perf::DeckTrack>> loadedTracks, loadedSamples;
    std::map<int, juce::String> itemBySerial; // every track loaded, for the take log
    std::map<int, int> loadRequests;
    std::array<std::pair<int, int>, perf::kDecks> cueSeen{};
    std::array<float, perf::kDecks> lastPeaks{};
    int serials = 0;
    int tick = 0;
    std::array<std::atomic<bool>, perf::kDecks> boundDecks{};
    std::array<std::atomic<bool>, 2> layers{}; // the controller's sides on decks 3 and 4
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
    else if (type == "PERF_PRELOAD") h.preload(payload, project, decodedCache);
    else if (type == "PERF_ATTACH_ITEM") h.attachItem(payload);
    else if (type == "PERF_STEMS") h.stems(payload);
    else if (type == "PERF_DECK_LAYER") h.setLayer(payload);
    else if (type == "PERF_SAMPLER_LOAD") h.loadSample(payload, project, decodedCache, pool);
    else if (type == "PERF_TRACK_LIVE")
    {
        // A timeline track driven from a performance channel: live values, and those let go.
        const auto trackId = payload.getProperty("trackId", "").toString();
        std::vector<std::pair<juce::String, float>> values;
        if (const auto* o = payload.getProperty("values", {}).getDynamicObject())
            for (const auto& p : o->getProperties()) values.emplace_back(p.name.toString(), static_cast<float>(static_cast<double>(p.value)));
        juce::StringArray clear;
        if (const auto* list = payload.getProperty("clear", {}).getArray())
            for (const auto& id : *list) clear.add(id.toString());
        if (static_cast<bool>(payload.getProperty("all", false))) engine.clearTrackLive(trackId);
        else engine.setTrackLive(trackId, values, clear);
    }
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
