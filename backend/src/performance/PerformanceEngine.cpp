#include "PerformanceEngine.h"

#include <algorithm>
#include <cmath>

namespace stemcel::perf
{
namespace
{
constexpr std::array<double, 8> kBeatLoopPads{0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0};
// Beat jump pages (rekordbox): odd pads jump back, even pads forward; 0 = FINE (5 ms).
constexpr std::array<std::array<double, 4>, 3> kBeatJumpPages{{
    {0.0, 0.125, 0.25, 0.5},
    {1.0, 2.0, 4.0, 8.0},
    {16.0, 32.0, 64.0, 128.0},
}};
// Key shift / keyboard pages: pads 1–4 are the top row, 5–8 the bottom.
constexpr std::array<std::array<int, 8>, 3> kKeyPages{{
    {4, 5, 6, 7, 0, 1, 2, 3},
    {-4, -3, -2, -1, -8, -7, -6, -5},
    {8, 9, 10, 11, -12, -11, -10, -9},
}};
constexpr double kFineJumpSeconds = 0.005;
constexpr double kParamMinIntervalSec = 0.010;

double paramEpsilon(RecParam p) noexcept
{
    switch (p)
    {
        case RecParam::filter:
        case RecParam::colorFx:
        case RecParam::beatFxLevel:
        case RecParam::padFxLevel:
            return 0.004;
        case RecParam::fxBpm:
        case RecParam::levelDb:
        case RecParam::bassDb:
        case RecParam::midDb:
        case RecParam::trebleDb:
            return 0.05;
        default:
            return 0.5; // whole-number steps: any change
    }
}

// Release FX types: Vinyl Brake, Echo, Spin Back.
constexpr std::array<int, 3> kReleaseTypes{static_cast<int>(djfx::BeatFx::vinylBrake), static_cast<int>(djfx::BeatFx::echo),
                                           static_cast<int>(djfx::BeatFx::spinBack)};

// PAD FX1 and PAD FX2: [bank][pad + 8 with SHIFT]. rekordbox doesn't publish its defaults;
// these cover every Beat FX, at the lengths DJs reach for.
using FX = djfx::BeatFx;
constexpr int t(FX f) { return static_cast<int>(f); }
const PadFxPreset kPadFx[2][16] = {
    {{t(FX::echo), 3, 0.6F}, {t(FX::echo), 4, 0.6F}, {t(FX::roll), 2, 1.0F}, {t(FX::roll), 1, 1.0F},
     {t(FX::trans), 2, 0.9F}, {t(FX::filter), 5, 0.8F}, {t(FX::flanger), 6, 0.7F}, {t(FX::reverb), 7, 0.7F},
     {t(FX::delay), 3, 0.6F}, {t(FX::pingPong), 3, 0.6F}, {t(FX::spiral), 3, 0.6F}, {t(FX::phaser), 7, 0.7F},
     {t(FX::pitch), 5, 0.85F}, {t(FX::slipRoll), 3, 0.6F}, {t(FX::helix), 3, 0.7F}, {t(FX::vinylBrake), 5, 0.5F}},
    {{t(FX::echo), 2, 0.6F}, {t(FX::pingPong), 2, 0.6F}, {t(FX::roll), 0, 1.0F}, {t(FX::roll), 3, 1.0F},
     {t(FX::trans), 1, 0.9F}, {t(FX::trans), 3, 0.9F}, {t(FX::spiral), 5, 0.6F}, {t(FX::echo), 5, 0.7F, true},
     {t(FX::reverb), 8, 0.8F}, {t(FX::phaser), 8, 0.7F}, {t(FX::flanger), 7, 0.7F}, {t(FX::filter), 6, 0.8F},
     {t(FX::pitch), 5, 0.2F}, {t(FX::delay), 5, 0.6F}, {t(FX::helix), 2, 0.7F}, {t(FX::vinylBrake), 6, 0.6F}},
};
} // namespace

const PadFxPreset& padFxPreset(int bank, int pad, bool shifted) noexcept
{
    return kPadFx[std::clamp(bank, 0, 1)][std::clamp(pad, 0, 7) + (shifted ? 8 : 0)];
}

const char* recParamName(RecParam p) noexcept
{
    switch (p)
    {
        case RecParam::levelDb: return "level";
        case RecParam::bassDb: return "toneBass";
        case RecParam::midDb: return "toneMid";
        case RecParam::trebleDb: return "toneTreble";
        case RecParam::filter: return "filter";
        case RecParam::colorFxType: return "colorFxType";
        case RecParam::colorFx: return "colorFx";
        case RecParam::beatFxType: return "beatFxType";
        case RecParam::beatFxBeats: return "beatFxBeats";
        case RecParam::beatFxLevel: return "beatFxLevel";
        case RecParam::beatFxOn: return "beatFxOn";
        case RecParam::padFxType: return "padFxType";
        case RecParam::padFxBeats: return "padFxBeats";
        case RecParam::padFxLevel: return "padFxLevel";
        case RecParam::padFxOn: return "padFxOn";
        case RecParam::fxBpm: return "fxBpm";
        default: return "";
    }
}

// Writes the decks' segments and platter points into the recorder queue.
class PerformanceEngine::Recorder final : public DeckRecorder
{
  public:
    explicit Recorder(PerformanceEngine& e) : engine(e) {}
    void segment(const Segment& s) noexcept override
    {
        RecEvent r;
        r.at = s.at;
        r.kind = RecEvent::Kind::segment;
        r.deck = static_cast<std::uint8_t>(s.deck);
        r.serial = s.serial;
        r.a = s.endPos;
        r.b = s.startPos;
        r.tempo = static_cast<float>(s.tempo);
        r.semitones = static_cast<std::int16_t>(s.semitones);
        r.flags = static_cast<std::uint8_t>((s.moving ? 1 : 0) | (s.audible ? 2 : 0) | (s.keyLock ? 4 : 0) |
                                            (s.scratch ? 8 : 0));
        engine.pushRec(r);
    }
    void platter(int deck, juce::int64 at, double pos) noexcept override
    {
        RecEvent r;
        r.at = at;
        r.kind = RecEvent::Kind::platter;
        r.deck = static_cast<std::uint8_t>(deck);
        r.a = pos;
        engine.pushRec(r);
    }

  private:
    PerformanceEngine& engine;
};

PerformanceEngine::PerformanceEngine()
{
    for (int d = 0; d < kDecks; ++d) decks[static_cast<size_t>(d)] = std::make_unique<DeckPlayer>(d + 1);
    recorder = std::make_unique<Recorder>(*this);
    for (auto& p : peaks) p.store(0.0F);
}

PerformanceEngine::~PerformanceEngine() = default;

void PerformanceEngine::pushRec(const RecEvent& r) noexcept
{
    int s1 = 0, n1 = 0, s2 = 0, n2 = 0;
    recFifo.prepareToWrite(1, s1, n1, s2, n2);
    if (n1 == 0)
    {
        recDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    recQueue[static_cast<size_t>(s1)] = r;
    recFifo.finishedWrite(1);
}

// --- Message thread -------------------------------------------------------------------------

void PerformanceEngine::setOpen(bool open) { opened.store(open, std::memory_order_release); }

void PerformanceEngine::setRecording(bool on)
{
    if (on && !recordingOn.load()) recordingRestart.store(true, std::memory_order_release);
    recordingOn.store(on, std::memory_order_release);
}

void PerformanceEngine::post(const Event& e)
{
    const juce::SpinLock::ScopedLockType lock(writeLock);
    int s1 = 0, n1 = 0, s2 = 0, n2 = 0;
    queueFifo.prepareToWrite(1, s1, n1, s2, n2);
    if (n1 == 0) return;
    queue[static_cast<size_t>(s1)] = Queued{e, nullptr, 0, false};
    queueFifo.finishedWrite(1);
}

void PerformanceEngine::load(int deck, std::shared_ptr<const DeckTrack> track, int copyFromDeck)
{
    if (deck < 1 || deck > kDecks) return;
    const DeckTrack* raw = track.get();
    if (track != nullptr)
    {
        const std::scoped_lock lock(tracksMutex);
        if (std::find(tracks.begin(), tracks.end(), track) == tracks.end()) tracks.push_back(std::move(track));
    }
    Event e;
    e.deck = static_cast<std::int16_t>(deck);
    const juce::SpinLock::ScopedLockType lock(writeLock);
    int s1 = 0, n1 = 0, s2 = 0, n2 = 0;
    queueFifo.prepareToWrite(1, s1, n1, s2, n2);
    if (n1 == 0) return;
    queue[static_cast<size_t>(s1)] = Queued{e, raw, copyFromDeck, true};
    queueFifo.finishedWrite(1);
}

void PerformanceEngine::loadSample(int slot, std::shared_ptr<const DeckTrack> sample)
{
    if (slot < 0 || slot >= kSamplerSlots) return;
    const DeckTrack* raw = sample.get();
    if (sample != nullptr)
    {
        const std::scoped_lock lock(tracksMutex);
        if (std::find(tracks.begin(), tracks.end(), sample) == tracks.end()) tracks.push_back(std::move(sample));
    }
    const juce::SpinLock::ScopedLockType lock(writeLock);
    int s1 = 0, n1 = 0, s2 = 0, n2 = 0;
    queueFifo.prepareToWrite(1, s1, n1, s2, n2);
    if (n1 == 0) return;
    Queued q;
    q.track = raw;
    q.sampleSlot = slot;
    queue[static_cast<size_t>(s1)] = q;
    queueFifo.finishedWrite(1);
}

void PerformanceEngine::startCapture(int deck, double seconds)
{
    captureDeck.store(0);
    captureBuffer.assign(static_cast<size_t>(std::max(1.0, seconds) * rate), 0.0F);
    captureLength.store(0);
    captureStart.store(-1);
    captureDeck.store(deck, std::memory_order_release);
}

bool PerformanceEngine::stopCapture(std::vector<float>& audio, juce::int64& startClock, double& sampleRate)
{
    captureDeck.store(0, std::memory_order_release);
    // The audio thread may be finishing a block: wait it out.
    juce::Thread::sleep(30);
    const int n = captureLength.load(std::memory_order_acquire);
    audio.assign(captureBuffer.begin(), captureBuffer.begin() + n);
    startClock = captureStart.load();
    sampleRate = rate;
    return n > 0;
}

void PerformanceEngine::collectGarbage()
{
    // A track can go once no deck shows it and no load of it is still queued.
    const auto& v = views.read();
    const std::scoped_lock lock(tracksMutex);
    if (queueFifo.getNumReady() > 0) return;
    tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
                                [&](const std::shared_ptr<const DeckTrack>& t)
                                {
                                    for (const auto& d : v.decks)
                                        if (d.serial == t->serial) return false;
                                    for (const int serial : v.sampleSerials)
                                        if (serial == t->serial) return false;
                                    return true;
                                }),
                 tracks.end());
}

void PerformanceEngine::drainRecording(std::vector<RecEvent>& out)
{
    int s1 = 0, n1 = 0, s2 = 0, n2 = 0;
    const int ready = recFifo.getNumReady();
    recFifo.prepareToRead(ready, s1, n1, s2, n2);
    for (int i = 0; i < n1; ++i) out.push_back(recQueue[static_cast<size_t>(s1 + i)]);
    for (int i = 0; i < n2; ++i) out.push_back(recQueue[static_cast<size_t>(s2 + i)]);
    recFifo.finishedRead(n1 + n2);
}

void PerformanceEngine::consumePeaks(std::array<float, kDecks>& peaksOut)
{
    for (int d = 0; d < kDecks; ++d) peaksOut[static_cast<size_t>(d)] = peaks[static_cast<size_t>(d)].exchange(0.0F);
}

// --- Audio thread ---------------------------------------------------------------------------

void PerformanceEngine::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    rate = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlock = std::max(512, samplesPerBlockExpected);
    deckBuffer.setSize(2 * kDecks, maxBlock);
    cueBuffer.setSize(2, maxBlock * 4);
    for (auto& d : decks) d->prepare(rate, maxBlock);
    for (auto& c : channels)
    {
        c.eq.prepare(rate, 2);
        c.eq.setParams(static_cast<float>(curves::eqDb(c.eqLow)), static_cast<float>(curves::eqDb(c.eqMid)),
                       static_cast<float>(curves::eqDb(c.eqHi)), static_cast<float>(curves::filterAmount(c.filter)), true);
        c.gain.reset(rate, 0.008);
        c.gain.setCurrentAndTargetValue(0.0F);
        c.fx.allocate(rate);
    }
    masterFx.allocate(rate);
    applyBeatFx();
}

double PerformanceEngine::fxBpmFor(int deck) const noexcept
{
    // TAP mode: the tapped tempo; else the channel's own (the master deck's for the master).
    if (fxTapMode && tapBpm > 0.0) return tapBpm;
    const int d = deck >= 1 ? deck : masterDeck;
    if (d >= 1 && d <= kDecks)
        if (const double bpm = decks[static_cast<size_t>(d - 1)]->effectiveBpm(); bpm > 0.0) return bpm;
    for (const auto& p : decks)
        if (p->effectiveBpm() > 0.0) return p->effectiveBpm();
    return 120.0;
}

// The Beat FX section on its unit (a channel's, or the master's), with a Release FX taking it
// over while it plays out; the other units' section FX off.
void PerformanceEngine::applyBeatFx() noexcept
{
    for (int unit = 0; unit <= kDecks; ++unit)
    {
        auto& fx = unit == 0 ? masterFx : channels[static_cast<size_t>(unit - 1)].fx.beat;
        const bool routed = (fxChannel == 2 && unit == 0) || (fxChannel == unit - 1 && unit >= 1);
        const bool releasing = releaseLeft > 0 && releaseUnit == unit;
        fx.setType(releasing ? kReleaseTypes[static_cast<size_t>(std::clamp(releaseType, 0, 2))] : fxType);
        fx.setBeatStep(releasing ? djfx::kDefaultBeatStep : fxStep);
        fx.setLevel(releasing ? 0.8F : fxLevel);
        fx.setOn(releasing || (routed && fxOn));
    }
}

void PerformanceEngine::beatFxControl(const Event& e) noexcept
{
    const bool press = e.value > 0.5F;
    int heldDeck = 0;
    for (int d = 1; d <= kDecks; ++d)
        if (channels[static_cast<size_t>(d - 1)].padFxHeld >= 0) heldDeck = d;
    switch (e.control)
    {
        case Control::fxBeatPrev:
        case Control::fxBeatNext:
        {
            if (!press) return;
            const int delta = e.control == Control::fxBeatNext ? 1 : -1;
            if (heldDeck != 0)
            {
                // BEAT ◁/▷ while a Pad FX pad is held: that pad's length, until it is let go.
                auto& c = channels[static_cast<size_t>(heldDeck - 1)];
                c.padFxRestoreStep = std::clamp((c.padFxRestoreStep >= 0 ? c.padFxRestoreStep : djfx::kDefaultBeatStep) + delta, 0, 9);
                c.fx.pad.setBeatStep(c.padFxRestoreStep);
                return;
            }
            fxStep = std::clamp(fxStep + delta, 0, static_cast<int>(djfx::kBeatSteps.size()) - 1);
            break;
        }
        case Control::fxBpmAuto:
            if (press) fxTapMode = false;
            break;
        case Control::fxTap:
        case Control::hpCueShift:
            if (!press) break;
            fxTapMode = true;
            if (lastTapAt >= 0)
            {
                const double seconds = static_cast<double>(clock - lastTapAt) / rate;
                if (seconds > 0.2 && seconds < 2.0) tapBpm = tapBpm > 0.0 ? 0.5 * tapBpm + 0.5 * (60.0 / seconds) : 60.0 / seconds;
            }
            lastTapAt = clock;
            break;
        case Control::fxSelect:
            if (press) fxType = (fxType + 1) % djfx::kBeatFxSelectable;
            break;
        case Control::fxSelectPrev:
            if (press) fxType = (fxType + djfx::kBeatFxSelectable - 1) % djfx::kBeatFxSelectable;
            break;
        case Control::fxTypeSelect:
            fxType = std::clamp(static_cast<int>(std::lround(e.value)), 0, djfx::kBeatFxSelectable - 1);
            break;
        case Control::fxChannel:
            fxChannel = std::clamp(static_cast<int>(e.index), 0, 2);
            break;
        case Control::fxLevel:
            fxLevel = std::clamp(e.value, 0.0F, 1.0F);
            break;
        case Control::fxOn:
            if (press) fxOn = !fxOn;
            break;
        case Control::fxRelease:
            if (!press) break;
            // Release FX on the Beat FX's channel for a beat or two, then the Beat FX is off.
            releaseUnit = fxChannel == 2 ? 0 : fxChannel + 1;
            releaseLeft = static_cast<juce::int64>(60.0 / fxBpmFor(releaseUnit) * (releaseType == 1 ? 1.0 : 2.0) * rate);
            fxOn = false;
            break;
        case Control::releaseFxSelect:
            releaseType = std::clamp(static_cast<int>(std::lround(e.value)), 0, 2);
            break;
        case Control::colorFxSelect:
            colorType = std::clamp(static_cast<int>(std::lround(e.value)), 0, static_cast<int>(djfx::ColorFx::count) - 1);
            for (auto& c : channels)
            {
                // The FILTER knob drives the channel filter, or the colour effect instead.
                const bool filter = colorType == static_cast<int>(djfx::ColorFx::filter);
                c.eq.setFilterTarget(filter ? static_cast<float>(curves::filterAmount(c.filter)) : 0.0F, false);
                c.fx.color.setType(colorType);
                c.fx.color.setAmount(filter ? 0.0F : static_cast<float>(curves::filterAmount(c.filter)));
            }
            break;
        default:
            break;
    }
    applyBeatFx();
}

void PerformanceEngine::triggerSample(int slot, bool start) noexcept
{
    auto& v = voices[static_cast<size_t>(slot)];
    const bool rec = recordingOn.load(std::memory_order_relaxed);
    if (v.playing && rec)
    {
        RecEvent r;
        r.at = clock;
        r.kind = RecEvent::Kind::sample;
        r.deck = static_cast<std::uint8_t>(kSamplerDeck);
        r.param = static_cast<std::uint8_t>(slot);
        r.flags = 0;
        r.serial = v.sample != nullptr ? v.sample->serial : 0;
        pushRec(r);
    }
    v.playing = false;
    if (!start || v.sample == nullptr || v.sample->audio == nullptr) return;
    v.pos = 0.0;
    v.playing = true;
    if (rec)
    {
        RecEvent r;
        r.at = clock;
        r.kind = RecEvent::Kind::sample;
        r.deck = static_cast<std::uint8_t>(kSamplerDeck);
        r.param = static_cast<std::uint8_t>(slot);
        r.flags = 1;
        r.serial = v.sample->serial;
        r.a = v.sample->audio->getNumSamples() / v.sample->sampleRate;
        pushRec(r);
    }
}

void PerformanceEngine::renderSampler(float* const* out, int numOutChannels, int n) noexcept
{
    const auto gain = static_cast<float>(curves::faderGain(samplerVolume));
    for (int slot = 0; slot < kSamplerSlots; ++slot)
    {
        auto& v = voices[static_cast<size_t>(slot)];
        if (!v.playing || v.sample == nullptr || v.sample->audio == nullptr) continue;
        const auto& audio = *v.sample->audio;
        const double step = v.sample->sampleRate / rate;
        const int len = audio.getNumSamples();
        const float* l = audio.getReadPointer(0);
        const float* r = audio.getReadPointer(std::min(1, audio.getNumChannels() - 1));
        for (int i = 0; i < n; ++i)
        {
            const auto k = static_cast<int>(v.pos);
            if (k >= len - 1)
            {
                triggerSample(slot, false); // played out
                break;
            }
            const auto f = static_cast<float>(v.pos - k);
            out[0][i] += (l[k] + (l[k + 1] - l[k]) * f) * gain;
            if (numOutChannels > 1) out[1][i] += (r[k] + (r[k + 1] - r[k]) * f) * gain;
            v.pos += step;
        }
    }
}

double PerformanceEngine::channelGain(int deck) const noexcept
{
    const auto& c = channels[static_cast<size_t>(deck - 1)];
    const double trim = juce::Decibels::decibelsToGain(curves::trimDb(c.trim), curves::kSilentDb);
    const auto curve = static_cast<curves::CrossfaderCurve>(crossfaderCurve.load(std::memory_order_relaxed));
    // Deck 1 on the crossfader's A side, deck 2 on B (rekordbox's default assign for two decks).
    return trim * curves::faderGain(c.fader) * curves::crossfaderGain(crossfader, deck == 1 ? 0 : 1, curve);
}

SyncSource PerformanceEngine::syncSourceFor(int deck) const noexcept
{
    SyncSource s;
    if (masterDeck == 0 || masterDeck == deck) return s;
    const auto& m = *decks[static_cast<size_t>(masterDeck - 1)];
    if (m.track() == nullptr || m.effectiveBpm() <= 0.0) return s;
    s.valid = true;
    s.bpm = m.effectiveBpm();
    s.beatPhase = m.beatPhase();
    s.moving = m.moving();
    return s;
}

void PerformanceEngine::updateMaster() noexcept
{
    // The master passes to a playing synced deck when the master stops (rekordbox's auto).
    if (masterDeck != 0 && !decks[static_cast<size_t>(masterDeck - 1)]->moving())
        for (int d = 1; d <= kDecks; ++d)
            if (d != masterDeck && decks[static_cast<size_t>(d - 1)]->syncOn() && decks[static_cast<size_t>(d - 1)]->moving())
            {
                masterDeck = d;
                break;
            }
    if (masterDeck != 0 && decks[static_cast<size_t>(masterDeck - 1)]->track() == nullptr) masterDeck = 0;
    for (int d = 1; d <= kDecks; ++d) decks[static_cast<size_t>(d - 1)]->setMaster(d == masterDeck);
}

void PerformanceEngine::syncButton(int deck, bool longPress) noexcept
{
    auto& player = *decks[static_cast<size_t>(deck - 1)];
    if (player.track() == nullptr) return;
    if (longPress)
    {
        masterDeck = deck;
        player.setSync(true);
        updateMaster();
        return;
    }
    if (player.syncOn())
    {
        player.setSync(false);
        if (masterDeck == deck)
        {
            masterDeck = 0;
            for (int d = 1; d <= kDecks; ++d)
                if (d != deck && decks[static_cast<size_t>(d - 1)]->syncOn()) masterDeck = d;
        }
        updateMaster();
        return;
    }
    player.setSync(true);
    if (masterDeck == 0)
    {
        // No master yet: another deck that is loaded (playing first) leads, else this one.
        int lead = 0;
        for (int d = 1; d <= kDecks; ++d)
        {
            if (d == deck) continue;
            const auto& other = *decks[static_cast<size_t>(d - 1)];
            if (other.track() == nullptr || other.effectiveBpm() <= 0.0) continue;
            if (lead == 0 || other.moving()) lead = d;
        }
        masterDeck = lead != 0 ? lead : deck;
    }
    updateMaster();
    player.syncPhaseNow(syncSourceFor(deck));
}

void PerformanceEngine::applyPad(int deck, const Event& e, bool shifted) noexcept
{
    auto& player = *decks[static_cast<size_t>(deck - 1)];
    auto& c = channels[static_cast<size_t>(deck - 1)];
    const int pad = juce::jlimit(0, 7, static_cast<int>(e.index));
    const bool press = e.value > 0.5F;
    c.padMode = e.mode;
    Event out;
    out.deck = e.deck;
    switch (e.mode)
    {
        case PadMode::hotCue:
            out.index = static_cast<std::int16_t>(pad);
            out.value = e.value;
            out.control = shifted ? Control::hotCueDelete : Control::hotCue;
            if (!shifted || press) player.handle(out);
            break;
        case PadMode::beatLoop:
            if (!press) break;
            out.control = Control::beatLoop;
            out.value = static_cast<float>(kBeatLoopPads[static_cast<size_t>(pad)]);
            player.handle(out);
            break;
        case PadMode::beatJump:
        {
            if (!press) break;
            if (shifted)
            {
                if (pad == 6) c.beatJumpPage = std::max(0, c.beatJumpPage - 1);
                if (pad == 7) c.beatJumpPage = std::min(2, c.beatJumpPage + 1);
                break;
            }
            double beats = kBeatJumpPages[static_cast<size_t>(c.beatJumpPage)][static_cast<size_t>(pad / 2)];
            if (beats <= 0.0) beats = kFineJumpSeconds / std::max(1.0e-3, player.beatSeconds());
            out.control = Control::beatJump;
            out.value = static_cast<float>(pad % 2 == 0 ? -beats : beats);
            player.handle(out);
            break;
        }
        case PadMode::keyShift:
            if (!press) break;
            if (shifted)
            {
                if (pad == 6) c.keyPage = (c.keyPage + 2) % 3;
                if (pad == 7) c.keyPage = (c.keyPage + 1) % 3;
                if (pad == 0)
                {
                    out.control = Control::keyShift;
                    out.value = 0.0F; // key reset
                    player.handle(out);
                }
                break;
            }
            out.control = Control::keyShift;
            out.value = static_cast<float>(kKeyPages[static_cast<size_t>(c.keyPage)][static_cast<size_t>(pad)]);
            player.handle(out);
            break;
        case PadMode::keyboard:
        {
            if (shifted)
            {
                if (!press) break;
                if (pad == 6) c.keyPage = (c.keyPage + 2) % 3;
                else if (pad == 7) c.keyPage = (c.keyPage + 1) % 3;
                else c.keyboardCue = -1; // choose another hot cue
                break;
            }
            if (c.keyboardCue < 0)
            {
                // The first pad picks (or sets) the hot cue the keyboard plays.
                if (!press) break;
                c.keyboardCue = pad;
                out.control = Control::hotCue;
                out.index = static_cast<std::int16_t>(pad);
                out.value = 1.0F;
                const auto* const before = player.track();
                DeckView v;
                player.fillView(v);
                if (!v.hotCues[static_cast<size_t>(pad)].set() && before != nullptr) player.handle(out); // sets it
                break;
            }
            Event key;
            key.deck = e.deck;
            key.control = Control::keyShift;
            key.value = static_cast<float>(kKeyPages[static_cast<size_t>(c.keyPage)][static_cast<size_t>(pad)]);
            if (press) player.handle(key);
            out.control = Control::hotCue;
            out.index = static_cast<std::int16_t>(c.keyboardCue);
            out.value = e.value;
            player.handle(out);
            break;
        }
        case PadMode::padFx1:
        case PadMode::padFx2:
        {
            const int bank = e.mode == PadMode::padFx1 ? 0 : 1;
            const int slot = pad + (shifted ? 8 : 0);
            if (press)
            {
                const auto& preset = padFxPreset(bank, pad, shifted);
                c.padFxHeld = slot;
                c.padFxRestoreStep = -1;
                c.fx.pad.setType(preset.release ? kReleaseTypes[static_cast<size_t>(std::clamp(releaseType, 0, 2))] : preset.type);
                c.fx.pad.setBeatStep(preset.step);
                c.fx.pad.setLevel(preset.level);
                c.fx.pad.setOn(true);
            }
            else if (c.padFxHeld == slot)
            {
                c.padFxHeld = -1;
                c.padFxRestoreStep = -1;
                c.fx.pad.setOn(false);
            }
            break;
        }
        case PadMode::sampler:
        {
            if (!press) break;
            const int slot = samplerBank * 16 + (deck - 1) * 8 + pad;
            // A pad plays its slot from the start (again if it is playing); SHIFT stops it.
            // SHIFT on an idle slot loads the browser's track into it (the app does that).
            if (shifted) triggerSample(slot, false);
            else triggerSample(slot, true);
            break;
        }
        default:
            break;
    }
}

void PerformanceEngine::apply(const Queued& q) noexcept
{
    if (q.sampleSlot >= 0)
    {
        auto& v = voices[static_cast<size_t>(q.sampleSlot)];
        triggerSample(q.sampleSlot, false);
        v.sample = q.track;
        return;
    }
    if (q.isLoad)
    {
        const int d = q.event.deck;
        if (d < 1 || d > kDecks) return;
        const DeckPlayer* copy = q.copyFrom >= 1 && q.copyFrom <= kDecks ? decks[static_cast<size_t>(q.copyFrom - 1)].get() : nullptr;
        decks[static_cast<size_t>(d - 1)]->load(q.track, copy);
        channels[static_cast<size_t>(d - 1)].keyboardCue = -1;
        updateMaster();
        return;
    }
    const auto& e = q.event;
    const bool press = e.value > 0.5F;
    switch (e.control)
    {
        case Control::crossfader:
            crossfader = std::clamp(e.value, 0.0F, 1.0F);
            return;
        case Control::fxChannel:
        case Control::fxLevel:
        case Control::fxOn:
        case Control::fxRelease:
        case Control::fxBeatPrev:
        case Control::fxBeatNext:
        case Control::fxBpmAuto:
        case Control::fxTap:
        case Control::fxSelect:
        case Control::fxSelectPrev:
        case Control::fxTypeSelect:
        case Control::releaseFxSelect:
        case Control::colorFxSelect:
        case Control::hpCueShift:
            beatFxControl(e);
            return;
        case Control::samplerBank:
            samplerBank = std::clamp(static_cast<int>(std::lround(e.value)), 0, 3);
            return;
        case Control::samplerVolume:
            samplerVolume = std::clamp(e.value, 0.0F, 1.0F);
            return;
        case Control::masterLevel:
        case Control::masterCue:
        case Control::hpMix:
        case Control::hpLevel:
            return; // the master and headphone knobs act in the unit's own hardware
        default:
            break;
    }
    if (e.deck < 1 || e.deck > kDecks) return;
    const int d = e.deck;
    auto& c = channels[static_cast<size_t>(d - 1)];
    auto& player = *decks[static_cast<size_t>(d - 1)];
    switch (e.control)
    {
        case Control::trim: c.trim = e.value; break;
        case Control::eqHi: c.eqHi = e.value; c.eq.setTrebleTarget(static_cast<float>(curves::eqDb(e.value)), false); break;
        case Control::eqMid: c.eqMid = e.value; c.eq.setMidTarget(static_cast<float>(curves::eqDb(e.value)), false); break;
        case Control::eqLow: c.eqLow = e.value; c.eq.setBassTarget(static_cast<float>(curves::eqDb(e.value)), false); break;
        case Control::filter:
        {
            c.filter = e.value;
            const bool asFilter = colorType == static_cast<int>(djfx::ColorFx::filter);
            c.eq.setFilterTarget(asFilter ? static_cast<float>(curves::filterAmount(e.value)) : 0.0F, false);
            c.fx.color.setType(colorType);
            c.fx.color.setAmount(asFilter ? 0.0F : static_cast<float>(curves::filterAmount(e.value)));
            break;
        }
        case Control::fader: c.fader = e.value; break;
        case Control::hpCue:
            if (press) c.hpCue = !c.hpCue;
            break;
        case Control::padMode:
            if (press)
            {
                c.padMode = e.mode;
                if (e.mode != PadMode::keyboard) c.keyboardCue = -1;
            }
            break;
        case Control::pad:
            applyPad(d, e, false);
            break;
        case Control::padShift:
            applyPad(d, e, true);
            break;
        case Control::sync:
            if (press) syncButton(d, false);
            break;
        case Control::syncLong:
            if (press) syncButton(d, true);
            break;
        default:
            player.handle(e);
            break;
    }
}

void PerformanceEngine::recordParams(int deck, juce::int64 at, bool force) noexcept
{
    auto& c = channels[static_cast<size_t>(deck - 1)];
    const bool asFilter = colorType == static_cast<int>(djfx::ColorFx::filter);
    const bool routed = fxChannel == deck - 1;
    const bool releasing = releaseLeft > 0 && releaseUnit == deck;
    const PadFxPreset* pad = nullptr;
    if (c.padFxHeld >= 0) pad = &padFxPreset(c.padMode == PadMode::padFx2 ? 1 : 0, c.padFxHeld % 8, c.padFxHeld >= 8);
    const std::array<double, static_cast<size_t>(RecParam::count)> values{
        curves::gainToDb(channelGain(deck)),
        curves::eqDb(c.eqLow),
        curves::eqDb(c.eqMid),
        curves::eqDb(c.eqHi),
        asFilter ? curves::filterAmount(c.filter) : 0.0,
        static_cast<double>(colorType),
        asFilter ? 0.0 : curves::filterAmount(c.filter),
        static_cast<double>(releasing ? kReleaseTypes[static_cast<size_t>(std::clamp(releaseType, 0, 2))] : fxType),
        static_cast<double>(releasing ? djfx::kDefaultBeatStep : fxStep),
        releasing ? 0.8 : static_cast<double>(fxLevel),
        (releasing || (routed && fxOn)) ? 1.0 : 0.0,
        static_cast<double>(pad == nullptr ? 1 : (pad->release ? kReleaseTypes[static_cast<size_t>(std::clamp(releaseType, 0, 2))] : pad->type)),
        static_cast<double>(pad == nullptr ? djfx::kDefaultBeatStep : (c.padFxRestoreStep >= 0 ? c.padFxRestoreStep : pad->step)),
        static_cast<double>(pad == nullptr ? 0.5F : pad->level),
        pad != nullptr ? 1.0 : 0.0,
        fxBpmFor(deck)};
    const auto minGap = static_cast<juce::int64>(kParamMinIntervalSec * rate);
    for (size_t i = 0; i < values.size(); ++i)
    {
        const bool changed = std::abs(values[i] - c.sent[i]) > paramEpsilon(static_cast<RecParam>(i));
        if (!force && !changed && !c.pending[i]) continue;
        if (!force && at - c.sentAt[i] < minGap)
        {
            c.pending[i] = changed || c.pending[i];
            continue;
        }
        if (!force && !changed)
        {
            c.pending[i] = false;
            continue;
        }
        RecEvent r;
        r.at = at;
        r.kind = RecEvent::Kind::param;
        r.deck = static_cast<std::uint8_t>(deck);
        r.param = static_cast<std::uint8_t>(i);
        r.a = values[i];
        pushRec(r);
        c.sent[i] = values[i];
        c.sentAt[i] = at;
        c.pending[i] = false;
    }
}

void PerformanceEngine::recordMasterParams(juce::int64 at, bool force) noexcept
{
    // The master's Beat FX (CH SELECT = MASTER) and its tempo; the sampler's level.
    const bool releasing = releaseLeft > 0 && releaseUnit == 0;
    const std::array<std::pair<RecParam, double>, 5> values{{
        {RecParam::beatFxType, static_cast<double>(releasing ? kReleaseTypes[static_cast<size_t>(std::clamp(releaseType, 0, 2))] : fxType)},
        {RecParam::beatFxBeats, static_cast<double>(releasing ? djfx::kDefaultBeatStep : fxStep)},
        {RecParam::beatFxLevel, releasing ? 0.8 : static_cast<double>(fxLevel)},
        {RecParam::beatFxOn, (releasing || (fxChannel == 2 && fxOn)) ? 1.0 : 0.0},
        {RecParam::fxBpm, fxBpmFor(0)},
    }};
    const auto minGap = static_cast<juce::int64>(kParamMinIntervalSec * rate);
    for (const auto& [param, value] : values)
    {
        const auto i = static_cast<size_t>(param);
        const bool changed = std::abs(value - masterSent[i]) > paramEpsilon(param);
        if (!force && (!changed || at - masterSentAt[i] < minGap)) continue;
        RecEvent r;
        r.at = at;
        r.kind = RecEvent::Kind::param;
        r.deck = 0;
        r.param = static_cast<std::uint8_t>(param);
        r.a = value;
        pushRec(r);
        masterSent[i] = value;
        masterSentAt[i] = at;
    }
    const double samplerDb = curves::gainToDb(curves::faderGain(samplerVolume));
    if (force || std::abs(samplerDb - samplerSentDb) > 0.05)
    {
        RecEvent r;
        r.at = at;
        r.kind = RecEvent::Kind::param;
        r.deck = static_cast<std::uint8_t>(kSamplerDeck);
        r.param = static_cast<std::uint8_t>(RecParam::levelDb);
        r.a = samplerDb;
        pushRec(r);
        samplerSentDb = samplerDb;
    }
}

void PerformanceEngine::getNextAudioBlock(const juce::AudioSourceChannelInfo& info)
{
    // Controls first, so this block already answers them.
    {
        int s1 = 0, n1 = 0, s2 = 0, n2 = 0;
        const int ready = queueFifo.getNumReady();
        queueFifo.prepareToRead(ready, s1, n1, s2, n2);
        const bool open = opened.load(std::memory_order_acquire);
        for (int i = 0; i < n1 + n2; ++i)
        {
            const auto& q = queue[static_cast<size_t>(i < n1 ? s1 + i : s2 + (i - n1))];
            if (open || q.isLoad) apply(q);
        }
        queueFifo.finishedRead(n1 + n2);
    }
    if (!opened.load(std::memory_order_acquire) || info.buffer == nullptr)
    {
        if (info.buffer != nullptr) info.buffer->clear(info.startSample, info.numSamples);
        cueSamples = 0;
        auto& v = views.writeBuffer();
        v.open = false;
        views.publish();
        return;
    }
    const int chans = info.buffer->getNumChannels();
    float* outs[2] = {info.buffer->getWritePointer(0, info.startSample),
                      info.buffer->getWritePointer(std::min(1, chans - 1), info.startSample)};
    int done = 0;
    cueSamples = 0;
    while (done < info.numSamples)
    {
        const int n = std::min(maxBlock, info.numSamples - done);
        float* chunk[2] = {outs[0] + done, outs[1] + done};
        renderBlock(chunk, chans, n);
        done += n;
    }
    publishView();
}

void PerformanceEngine::renderBlock(float* const* out, int numOutChannels, int n) noexcept
{
    const bool rec = recordingOn.load(std::memory_order_acquire);
    const bool restart = recordingRestart.exchange(false, std::memory_order_acq_rel);
    DeckRecorder* sink = rec ? recorder.get() : nullptr;
    if (wasRecording && (!rec || restart))
        for (auto& d : decks) d->endTake(recorder.get(), clock); // close the last take's segments
    wasRecording = rec;
    if (restart)
    {
        RecEvent r;
        r.at = clock;
        r.kind = RecEvent::Kind::clockRate;
        r.a = rate;
        pushRec(r);
    }

    updateMaster();
    if (releaseLeft > 0)
    {
        releaseLeft -= n;
        if (releaseLeft <= 0)
        {
            releaseLeft = 0;
            releaseUnit = -1;
        }
        applyBeatFx();
    }
    for (int d = 1; d <= kDecks; ++d) channels[static_cast<size_t>(d - 1)].fx.setBpm(fxBpmFor(d));
    masterFx.setBpm(fxBpmFor(0));
    std::fill(out[0], out[0] + n, 0.0F);
    if (numOutChannels > 1 && out[1] != out[0]) std::fill(out[1], out[1] + n, 0.0F);
    const bool cueOn = cueOutputAvailable.load(std::memory_order_relaxed);
    const int cueAt = cueSamples;
    if (cueOn && cueAt + n <= cueBuffer.getNumSamples()) cueBuffer.clear(cueAt, n);

    for (int d = 1; d <= kDecks; ++d)
    {
        auto& player = *decks[static_cast<size_t>(d - 1)];
        auto& c = channels[static_cast<size_t>(d - 1)];
        float* buf[2] = {deckBuffer.getWritePointer((d - 1) * 2), deckBuffer.getWritePointer((d - 1) * 2 + 1)};
        const double target = channelGain(d);
        const bool audible = target > 1.0e-4;
        if (restart) player.announce(); // a new take: the deck states where it is
        const auto sync = syncSourceFor(d);
        // A synced follower that just started lines its beats up with the master's.
        if (player.moving() && !c.wasMoving && player.syncOn() && masterDeck != d) player.syncPhaseNow(sync);
        player.render(buf, n, sync, sink, clock, audible);
        c.wasMoving = player.moving();

        // The channel as a timeline track plays it: EQ and filter, Sound Color FX, the level,
        // then Beat FX and Pad FX (whose echoes ring on after the fader closes).
        juce::AudioBuffer<float> view(buf, 2, n);
        c.eq.process(view, 0, n);
        c.fx.processColor(view, 0, n);
        if (cueOn && c.hpCue && cueAt + n <= cueBuffer.getNumSamples())
            for (int ch = 0; ch < 2; ++ch) cueBuffer.addFrom(ch, cueAt, buf[ch], n);
        c.gain.setTargetValue(static_cast<float>(target));
        for (int i = 0; i < n; ++i)
        {
            const float g = c.gain.getNextValue();
            buf[0][i] *= g;
            buf[1][i] *= g;
        }
        c.fx.processBeat(view, 0, n);
        if (captureDeck.load(std::memory_order_acquire) == d)
        {
            if (captureStart.load(std::memory_order_relaxed) < 0) captureStart.store(clock, std::memory_order_relaxed);
            const int at = captureLength.load(std::memory_order_relaxed);
            const int room = std::min(n, static_cast<int>(captureBuffer.size()) - at);
            for (int i = 0; i < room; ++i) captureBuffer[static_cast<size_t>(at + i)] = 0.5F * (buf[0][i] + buf[1][i]);
            captureLength.store(at + std::max(0, room), std::memory_order_release);
        }
        float peak = 0.0F;
        for (int i = 0; i < n; ++i)
        {
            const float l = buf[0][i], r = buf[1][i];
            out[0][i] += l;
            if (numOutChannels > 1) out[1][i] += r;
            peak = std::max({peak, std::abs(l), std::abs(r)});
        }
        float prev = peaks[static_cast<size_t>(d - 1)].load(std::memory_order_relaxed);
        while (peak > prev && !peaks[static_cast<size_t>(d - 1)].compare_exchange_weak(prev, peak)) {}
        if (rec) recordParams(d, clock, restart);
    }
    // The sampler into the master, then the master's Beat FX over everything.
    renderSampler(out, numOutChannels, n);
    if (numOutChannels > 1) masterFx.process(out[0], out[1], n);
    if (rec) recordMasterParams(clock, restart);
    if (cueOn) cueSamples = cueAt + n;
    clock += n;
}

void PerformanceEngine::renderCue(juce::AudioBuffer<float>& buffer, int firstChannel, int startSample,
                                  int numSamples) noexcept
{
    for (int ch = firstChannel; ch < buffer.getNumChannels(); ++ch)
    {
        const int src = std::min(ch - firstChannel, 1);
        if (cueSamples >= numSamples && opened.load(std::memory_order_acquire))
            buffer.copyFrom(ch, startSample, cueBuffer, src, 0, numSamples);
        else
            buffer.clear(ch, startSample, numSamples);
    }
}

void PerformanceEngine::publishView() noexcept
{
    auto& v = views.writeBuffer();
    v.open = true;
    v.clock = clock;
    v.sampleRate = rate;
    for (int d = 0; d < kDecks; ++d)
    {
        decks[static_cast<size_t>(d)]->fillView(v.decks[static_cast<size_t>(d)]);
        const auto& c = channels[static_cast<size_t>(d)];
        auto& cv = v.channels[static_cast<size_t>(d)];
        cv.trim = c.trim;
        cv.eqHi = c.eqHi;
        cv.eqMid = c.eqMid;
        cv.eqLow = c.eqLow;
        cv.filter = c.filter;
        cv.fader = c.fader;
        cv.hpCue = c.hpCue;
        cv.padMode = c.padMode;
        cv.beatJumpPage = c.beatJumpPage;
        cv.keyPage = c.keyPage;
        cv.keyboardCue = c.keyboardCue;
        cv.padFxHeld = c.padFxHeld;
    }
    v.fxType = fxType;
    v.fxStep = fxStep;
    v.fxChannel = fxChannel;
    v.colorType = colorType;
    v.releaseType = releaseType;
    v.fxLevel = fxLevel;
    v.fxOn = fxOn || releaseLeft > 0;
    v.fxTapMode = fxTapMode;
    v.fxBpm = fxBpmFor(fxChannel == 2 ? 0 : fxChannel + 1);
    v.samplerBank = samplerBank;
    v.samplerVolume = samplerVolume;
    v.samplesLoaded = 0;
    v.samplesPlaying = 0;
    for (int slot = 0; slot < kSamplerSlots; ++slot)
    {
        const auto& voice = voices[static_cast<size_t>(slot)];
        v.sampleSerials[static_cast<size_t>(slot)] = voice.sample != nullptr ? voice.sample->serial : 0;
        if (voice.sample != nullptr) v.samplesLoaded |= (1ULL << slot);
        if (voice.playing) v.samplesPlaying |= (1ULL << slot);
    }
    v.crossfader = crossfader;
    v.masterDeck = masterDeck;
    v.cueOutput = cueOutputAvailable.load(std::memory_order_relaxed);
    v.recording = recordingOn.load(std::memory_order_relaxed);
    views.publish();
}

} // namespace stemcel::perf
