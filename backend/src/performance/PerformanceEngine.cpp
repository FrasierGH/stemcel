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

double paramEpsilon(RecParam p) noexcept { return p == RecParam::filter ? 0.002 : 0.05; }
} // namespace

const char* recParamName(RecParam p) noexcept
{
    switch (p)
    {
        case RecParam::levelDb: return "level";
        case RecParam::bassDb: return "toneBass";
        case RecParam::midDb: return "toneMid";
        case RecParam::trebleDb: return "toneTreble";
        case RecParam::filter: return "filter";
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
    e.deck = static_cast<std::int8_t>(deck);
    const juce::SpinLock::ScopedLockType lock(writeLock);
    int s1 = 0, n1 = 0, s2 = 0, n2 = 0;
    queueFifo.prepareToWrite(1, s1, n1, s2, n2);
    if (n1 == 0) return;
    queue[static_cast<size_t>(s1)] = Queued{e, raw, copyFromDeck, true};
    queueFifo.finishedWrite(1);
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

void PerformanceEngine::consumePeaks(std::array<float, kDecks>& out)
{
    for (int d = 0; d < kDecks; ++d) out[static_cast<size_t>(d)] = peaks[static_cast<size_t>(d)].exchange(0.0F);
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
            out.index = static_cast<std::int8_t>(pad);
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
                out.index = static_cast<std::int8_t>(pad);
                out.value = 1.0F;
                const auto before = player.track();
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
            out.index = static_cast<std::int8_t>(c.keyboardCue);
            out.value = e.value;
            player.handle(out);
            break;
        }
        default:
            break; // sampler and pad FX: their own milestone
    }
}

void PerformanceEngine::apply(const Queued& q) noexcept
{
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
        case Control::masterLevel:
        case Control::masterCue:
        case Control::hpMix:
        case Control::hpLevel:
            return; // effects: their own milestone; the master and headphone knobs act in hardware
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
        case Control::filter: c.filter = e.value; c.eq.setFilterTarget(static_cast<float>(curves::filterAmount(e.value)), false); break;
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
    const std::array<double, static_cast<size_t>(RecParam::count)> values{
        curves::gainToDb(channelGain(deck)), curves::eqDb(c.eqLow), curves::eqDb(c.eqMid), curves::eqDb(c.eqHi),
        curves::filterAmount(c.filter)};
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

        juce::AudioBuffer<float> view(buf, 2, n);
        c.eq.process(view, 0, n);
        if (cueOn && c.hpCue && cueAt + n <= cueBuffer.getNumSamples())
            for (int ch = 0; ch < 2; ++ch) cueBuffer.addFrom(ch, cueAt, buf[ch], n);
        c.gain.setTargetValue(static_cast<float>(target));
        float peak = 0.0F;
        for (int i = 0; i < n; ++i)
        {
            const float g = c.gain.getNextValue();
            const float l = buf[0][i] * g, r = buf[1][i] * g;
            out[0][i] += l;
            if (numOutChannels > 1) out[1][i] += r;
            peak = std::max(peak, std::max(std::abs(l), std::abs(r)));
        }
        float prev = peaks[static_cast<size_t>(d - 1)].load(std::memory_order_relaxed);
        while (peak > prev && !peaks[static_cast<size_t>(d - 1)].compare_exchange_weak(prev, peak)) {}
        if (rec) recordParams(d, clock, restart);
    }
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
    }
    v.crossfader = crossfader;
    v.masterDeck = masterDeck;
    v.cueOutput = cueOutputAvailable.load(std::memory_order_relaxed);
    v.recording = recordingOn.load(std::memory_order_relaxed);
    views.publish();
}

} // namespace stemcel::perf
