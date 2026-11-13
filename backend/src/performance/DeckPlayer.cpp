#include "DeckPlayer.h"

#include <signalsmith-stretch/signalsmith-stretch.h>

#include <algorithm>
#include <cmath>

namespace stemcel::perf
{
namespace
{
constexpr int kTailSamples = 96;          // ~2 ms cross-fade over a jump
constexpr double kRateSmoothSec = 0.002;  // motor and nudge response
constexpr double kScratchFollowSec = 0.010;
constexpr double kSecondsPerTurn = 1.8;   // 33⅓ rpm
constexpr double kBendPerTick = 0.0016;   // tempo change per tick of recent side motion
constexpr double kBendDecaySec = 0.06;
constexpr double kSearchSecondsPerTick = 0.04;
constexpr double kAdjustSecondsPerTick = 0.002;
constexpr double kAtCueTolerance = 0.0015; // seconds
constexpr double kMinSplitSec = 0.25;      // pitch-fader splits are no closer than this
constexpr double kTempoSplit = 0.0002;
constexpr std::array<double, 4> kRanges{0.06, 0.10, 0.16, 1.00};

float smoothGain(double rate) noexcept
{
    // Silent while the record is held still, full from a slow drag up.
    const double x = std::clamp((std::abs(rate) - 0.004) / 0.06, 0.0, 1.0);
    return static_cast<float>(x * x * (3.0 - 2.0 * x));
}
} // namespace

struct DeckPlayer::Stretch
{
    signalsmith::stretch::SignalsmithStretch<float> engine;
    std::vector<std::vector<float>> in, seek;
    std::vector<float*> inPtrs, seekPtrs;
    double streamRead = 0.0, streamAudible = 0.0;
    double cursor = 0.0; // where the next input sample is read (loop-aware)
    bool needsReset = true;
    int capacity = 0;
};

DeckPlayer::DeckPlayer(int deckNumber) : deck(deckNumber), stretch(std::make_unique<Stretch>()) {}
DeckPlayer::~DeckPlayer() = default;

void DeckPlayer::prepare(double outputSampleRate, int block)
{
    outRate = outputSampleRate > 0.0 ? outputSampleRate : 44100.0;
    maxBlock = std::max(block, 64);
    auto& s = *stretch;
    s.engine.presetDefault(2, static_cast<float>(outRate));
    const int seekLen = s.engine.outputSeekLength(4.0F) + 64;
    s.capacity = maxBlock * 5 + 64;
    s.in.assign(2, std::vector<float>(static_cast<size_t>(s.capacity)));
    s.seek.assign(2, std::vector<float>(static_cast<size_t>(seekLen)));
    s.inPtrs = {s.in[0].data(), s.in[1].data()};
    s.seekPtrs = {s.seek[0].data(), s.seek[1].data()};
    s.needsReset = true;
}

// --- Track geometry -------------------------------------------------------------------------

double DeckPlayer::sr() const noexcept { return loaded != nullptr ? loaded->sampleRate : outRate; }
double DeckPlayer::length() const noexcept
{
    return loaded != nullptr && loaded->audio != nullptr ? static_cast<double>(loaded->audio->getNumSamples()) : 0.0;
}
double DeckPlayer::beatLen() const noexcept
{
    return loaded != nullptr && loaded->bpm > 0.0 ? 60.0 / loaded->bpm * sr() : 0.0;
}
double DeckPlayer::anchor() const noexcept { return loaded != nullptr ? loaded->anchorSec * sr() : 0.0; }

bool DeckPlayer::hasGrid() const noexcept
{
    return loaded != nullptr && (loaded->bpm > 0.0 || loaded->beats.size() >= 2);
}

double DeckPlayer::gridBeat(double p) const noexcept
{
    if (loaded == nullptr) return 0.0;
    const auto& b = loaded->beats;
    const double sec = p / sr();
    const double period = loaded->bpm > 0.0 ? 60.0 / loaded->bpm : 0.0;
    if (b.size() >= 2)
    {
        if (sec <= b.front()) return (sec - b.front()) / (period > 0.0 ? period : b[1] - b[0]);
        const auto n = b.size();
        if (sec >= b.back()) return static_cast<double>(n - 1) + (sec - b.back()) / (period > 0.0 ? period : b[n - 1] - b[n - 2]);
        const auto i = static_cast<size_t>(std::upper_bound(b.begin(), b.end(), sec) - b.begin()) - 1;
        return static_cast<double>(i) + (sec - b[i]) / std::max(1.0e-6, b[i + 1] - b[i]);
    }
    return period > 0.0 ? (sec - loaded->anchorSec) / period : 0.0;
}

double DeckPlayer::gridPos(double beat) const noexcept
{
    if (loaded == nullptr) return 0.0;
    const auto& b = loaded->beats;
    const double period = loaded->bpm > 0.0 ? 60.0 / loaded->bpm : 0.0;
    if (b.size() >= 2)
    {
        const auto n = b.size();
        if (beat <= 0.0) return (b.front() + beat * (period > 0.0 ? period : b[1] - b[0])) * sr();
        if (beat >= static_cast<double>(n - 1))
            return (b.back() + (beat - static_cast<double>(n - 1)) * (period > 0.0 ? period : b[n - 1] - b[n - 2])) * sr();
        const auto i = static_cast<size_t>(std::floor(beat));
        return (b[i] + (beat - static_cast<double>(i)) * (b[i + 1] - b[i])) * sr();
    }
    return (loaded->anchorSec + beat * period) * sr();
}

// Quantize: only with Q on, to the analysed beats.
double DeckPlayer::snapNearest(double p) const noexcept
{
    if (!quantizeOn || !hasGrid()) return p;
    return gridPos(std::round(gridBeat(p)));
}

double DeckPlayer::snapFloor(double p, double grid) const noexcept
{
    if (!quantizeOn || !hasGrid() || grid <= 0.0 || beatLen() <= 0.0) return p;
    const double fraction = std::min(1.0, grid / beatLen()); // in beats
    return gridPos(std::floor(gridBeat(p) / fraction + 1.0e-6) * fraction);
}

void DeckPlayer::holdTempo(double tempo) noexcept
{
    heldTempo = tempo;
    tempoHeld = true;
    // Where this tempo sits on the slider in the current range (its end when out of range).
    takeover = static_cast<float>(std::clamp(((tempo - 1.0) / range + 1.0) * 0.5, 0.0, 1.0));
    lastSlider = slider;
}

void DeckPlayer::setMaster(bool on) noexcept
{
    // A synced deck that becomes the master keeps the tempo it was synced to.
    if (on && !isMaster && syncEnabled && loaded != nullptr) holdTempo(syncTempo);
    isMaster = on;
}

double DeckPlayer::beatSeconds() const noexcept { return loaded != nullptr && loaded->bpm > 0.0 ? 60.0 / loaded->bpm : 0.5; }

double DeckPlayer::tempoFactor() const noexcept
{
    if (syncEnabled && !isMaster) return syncTempo;
    if (tempoHeld) return heldTempo;
    return std::max(0.0, 1.0 + range * (static_cast<double>(slider) * 2.0 - 1.0));
}

double DeckPlayer::effectiveBpm() const noexcept
{
    return loaded != nullptr && loaded->bpm > 0.0 ? loaded->bpm * tempoFactor() : 0.0;
}

double DeckPlayer::beatPhase() const noexcept
{
    return hasGrid() ? gridBeat(pos) : 0.0;
}

bool DeckPlayer::moving() const noexcept { return loaded != nullptr && (playing || preview != Preview::none); }

// --- Loading --------------------------------------------------------------------------------

void DeckPlayer::load(const DeckTrack* track, const DeckPlayer* copy) noexcept
{
    if (loaded != nullptr && runGain > 0.0F) jumpTo(pos); // fade the old track out
    loaded = track;
    preview = Preview::none;
    playing = false;
    exitLoop();
    loopInPending = false;
    loopArmed = false;
    touched = false;
    bendTicks = 0.0;
    memoryCursor = -1;
    semitones = 0;
    stretch->needsReset = true;
    forceSegment = true;
    if (track == nullptr)
    {
        pos = cuePoint = 0.0;
        hotCues = {};
        memoryCues = {};
        bumpCues();
        return;
    }
    hotCues = track->hotCues;
    memoryCues = track->memoryCues;
    bumpCues();
    // The cue starts on the saved cue point, else the first beat (rekordbox's auto cue).
    double first = 0.0;
    if (track->cueSec >= 0.0) first = track->cueSec * sr();
    else if (!track->beats.empty()) first = track->beats.front() * sr();
    else if (beatLen() > 0.0) first = anchor() - std::floor(anchor() / beatLen()) * beatLen();
    cuePoint = std::clamp(first, 0.0, length());
    pos = cuePoint;
    if (copy != nullptr && copy->loaded == track)
    {
        pos = copy->pos;
        cuePoint = copy->cuePoint;
        playing = copy->moving();
        loopActive = copy->loopActive;
        loopIn = copy->loopIn;
        loopOut = copy->loopOut;
        slider = copy->slider;
        range = copy->range;
        holdTempo(copy->tempoFactor());
        keyLockOn = copy->keyLockOn;
        semitones = copy->semitones;
        if (playing) fadeInLeft = kTailSamples;
    }
}

// --- Controls -------------------------------------------------------------------------------

void DeckPlayer::setSync(bool on) noexcept
{
    if (syncEnabled && !on)
    {
        // Off: keep the synced tempo until the slider is brought to it.
        holdTempo(tempoFactor());
    }
    syncEnabled = on;
}

void DeckPlayer::syncPhaseNow(const SyncSource& master) noexcept
{
    if (!master.valid || master.bpm <= 0.0 || loaded == nullptr || loaded->bpm <= 0.0) return;
    syncTempo = master.bpm / loaded->bpm;
    if (!moving() || !master.moving) return;
    double diff = master.beatPhase - beatPhase();
    diff -= std::round(diff);
    if (std::abs(diff * beatLen()) > 0.004 * sr()) jumpTo(pos + diff * beatLen());
}

void DeckPlayer::handle(const Event& e) noexcept
{
    const bool press = e.value > 0.5F;
    switch (e.control)
    {
        case Control::play:
        case Control::playShift:
            if (!press || loaded == nullptr) break;
            if (preview != Preview::none)
            {
                // PLAY during a CUE (or hot cue) hold: playback carries on after the release.
                preview = Preview::none;
                playing = true;
            }
            else if (playing)
                stopPlaying();
            else
                startPlaying();
            break;
        case Control::cue:
            cueButton(press);
            break;
        case Control::cueShift:
            if (press && loaded != nullptr) jumpTo(0.0);
            break;
        case Control::faderStartPlay:
            if (press && loaded != nullptr && !playing)
            {
                jumpTo(cuePoint);
                startPlaying();
            }
            break;
        case Control::faderStartCue:
            if (press && loaded != nullptr && playing)
            {
                stopPlaying();
                jumpTo(cuePoint);
            }
            break;
        case Control::tempo:
        {
            const float v = std::clamp(e.value, 0.0F, 1.0F);
            if (tempoHeld)
            {
                // The pickup: the slider takes over once it reaches (or passes) the held tempo.
                const bool crossed = (lastSlider - takeover) * (v - takeover) <= 0.0F;
                if (crossed || std::abs(v - takeover) < 0.01F) tempoHeld = false;
                lastSlider = v;
            }
            slider = v;
            break;
        }
        case Control::syncShift:
            if (press)
            {
                // A new range keeps the tempo; the slider picks it up from there.
                const double now = tempoFactor();
                size_t i = 0;
                while (i < kRanges.size() && kRanges[i] < range - 1.0e-6) ++i;
                range = kRanges[(i + 1) % kRanges.size()];
                if (!syncEnabled || isMaster) holdTempo(now);
            }
            break;
        case Control::keyLock:
            keyLockOn = e.value < 0.0F ? !keyLockOn : press;
            break;
        case Control::quantize:
            quantizeOn = e.value < 0.0F ? !quantizeOn : press;
            break;
        case Control::loopIn:
            if (!press || loaded == nullptr) break;
            if (loopActive) loopAdjust = loopAdjust == 1 ? 0 : 1;
            else
            {
                pendingIn = snapNearest(pos);
                loopInPending = true;
            }
            break;
        case Control::loopInLong:
            if (press && loaded != nullptr && !loopActive)
            {
                loopInPending = false;
                beatLoop(4.0);
            }
            break;
        case Control::loopOut:
            if (!press || loaded == nullptr) break;
            if (loopActive) loopAdjust = loopAdjust == 2 ? 0 : 2;
            else if (loopInPending)
            {
                double out = snapNearest(pos);
                const double minLen = beatLen() > 0.0 ? beatLen() / 32.0 : 0.01 * sr();
                if (out <= pendingIn + minLen) out = pendingIn + (beatLen() > 0.0 ? beatLen() : 0.5 * sr());
                loopInPending = false;
                setLoop(pendingIn, out, false);
            }
            break;
        case Control::reloopExit:
            if (!press || loaded == nullptr) break;
            if (loopAdjust != 0) loopAdjust = 0;
            else if (loopActive) exitLoop();
            else if (loopOut > loopIn)
            {
                loopActive = true;
                jumpTo(loopIn);
            }
            break;
        case Control::reloopShift:
            if (press && loopOut > loopIn) loopArmed = !loopArmed;
            break;
        case Control::callPrev:
            if (!press || loaded == nullptr) break;
            if (loopActive) loopResize(0.5);
            else callMemory(-1);
            break;
        case Control::callNext:
            if (!press || loaded == nullptr) break;
            if (loopActive) loopResize(2.0);
            else callMemory(1);
            break;
        case Control::callPrevShift:
            if (press) deleteMemory();
            break;
        case Control::callNextShift:
            if (press) storeMemory();
            break;
        case Control::jogTouch:
            if (loaded == nullptr) break;
            if (press && !touched)
            {
                touched = true;
                platter = pos;
            }
            else if (!press)
                touched = false;
            break;
        case Control::jogScratch:
        case Control::jogBend:
        case Control::jogSearch:
            jog(e.control, e.value);
            break;
        case Control::beatJump:
            beatJump(e.value);
            break;
        case Control::beatLoop:
            if (press || e.value > 0.0F) beatLoop(e.value);
            break;
        case Control::keyShift:
            semitones = juce::jlimit(-12, 12, static_cast<int>(std::lround(e.value)));
            break;
        case Control::hotCue:
            hotCueButton(e.index, press);
            break;
        case Control::hotCueDelete:
            if (e.index >= 0 && e.index < kHotCues && hotCues[static_cast<size_t>(e.index)].set())
            {
                hotCues[static_cast<size_t>(e.index)] = {};
                bumpCues();
            }
            break;
        case Control::seek:
            if (loaded != nullptr) jumpTo(std::clamp(static_cast<double>(e.value) * sr(), 0.0, length()));
            break;
        default:
            break;
    }
}

void DeckPlayer::startPlaying() noexcept
{
    if (loaded == nullptr || length() <= 0.0) return;
    if (pos >= length() - 1.0) jumpTo(cuePoint);
    playing = true;
    preview = Preview::none;
    if (runGain <= 0.0F) fadeInLeft = kTailSamples;
}

void DeckPlayer::stopPlaying() noexcept
{
    if (!playing && preview == Preview::none) return;
    tail = {true, pos, currentRate, kTailSamples}; // fade the sound out where it stopped
    playing = false;
    preview = Preview::none;
    currentRate = 0.0;
}

void DeckPlayer::jumpTo(double p) noexcept
{
    if (loaded == nullptr) return;
    if (!jumped)
    {
        jumped = true;
        jumpFrom = pos;
    }
    if (runGain > 0.0F && std::abs(currentRate) > 1.0e-4) tail = {true, pos, currentRate, kTailSamples};
    pos = std::clamp(p, 0.0, length());
    if (touched) platter = pos;
    stretch->needsReset = true;
    if (runGain > 0.0F) fadeInLeft = kTailSamples;
}

// The CUE button, as on a CDJ in rekordbox's default mode:
//   playing           → back to the cue point, paused (Back Cue)
//   paused, off it    → sets the cue point here (snapped to the beat with quantize)
//   paused, on it     → plays while held; releasing returns to it and pauses
//   PLAY during a hold → playback continues after the release (handled with PLAY)
void DeckPlayer::cueButton(bool pressed) noexcept
{
    if (loaded == nullptr) return;
    if (!pressed)
    {
        if (preview == Preview::cue)
        {
            stopPlaying();
            jumpTo(cuePoint);
        }
        return;
    }
    if (playing)
    {
        stopPlaying();
        jumpTo(cuePoint);
        return;
    }
    if (std::abs(pos - cuePoint) > kAtCueTolerance * sr())
    {
        cuePoint = std::clamp(snapNearest(pos), 0.0, length());
        if (std::abs(cuePoint - pos) > 0.5) jumpTo(cuePoint);
        return;
    }
    preview = Preview::cue;
    previewReturn = cuePoint;
    if (runGain <= 0.0F) fadeInLeft = kTailSamples;
}

void DeckPlayer::hotCueButton(int index, bool pressed) noexcept
{
    if (loaded == nullptr || index < 0 || index >= kHotCues) return;
    auto& cue = hotCues[static_cast<size_t>(index)];
    if (!pressed)
    {
        if (preview == Preview::hotCue && previewHotCue == index)
        {
            stopPlaying();
            jumpTo(previewReturn);
        }
        return;
    }
    if (!cue.set())
    {
        if (loopActive) cue = {loopIn / sr(), loopOut / sr()};
        else cue = {snapNearest(pos) / sr(), -1.0};
        bumpCues();
        return;
    }
    const double at = cue.pos * sr();
    if (cue.isLoop())
    {
        setLoop(at, cue.end * sr(), false);
        jumpTo(at);
        return;
    }
    if (playing)
    {
        // With quantize the jump keeps the beat: it lands as far past the cue as we are past
        // the last beat.
        double target = at;
        const double b = beatLen();
        if (quantizeOn && b > 0.0)
        {
            const double into = pos - snapFloor(pos, b);
            if (into < b * 0.999) target = at + into;
        }
        jumpTo(target);
        return;
    }
    // Paused: plays from the hot cue while held, back to it on release.
    jumpTo(at);
    preview = Preview::hotCue;
    previewHotCue = index;
    previewReturn = at;
}

void DeckPlayer::setLoop(double in, double out, bool jumpInside) noexcept
{
    in = std::clamp(in, 0.0, length());
    out = std::clamp(out, in + 1.0, length());
    loopIn = in;
    loopOut = out;
    loopActive = true;
    loopAdjust = 0;
    stretch->needsReset = true;
    if (jumpInside && (pos < in || pos >= out)) jumpTo(in);
    else if (pos >= out) jumpTo(in + std::fmod(pos - in, out - in));
}

void DeckPlayer::beatLoop(double beats) noexcept
{
    if (loaded == nullptr || beats <= 0.0) return;
    if (loopActive && beatLen() > 0.0 && std::abs((loopOut - loopIn) - beats * beatLen()) < 2.0)
    {
        exitLoop(); // the same pad again ends it
        return;
    }
    const double len = (beatLen() > 0.0 ? beatLen() : 0.5 * sr()) * beats;
    // The loop starts on the grid at or before here (a beat, or the loop's own length when
    // shorter), so the playhead is already inside it.
    const double grid = beatLen() > 0.0 ? std::min(beatLen(), len) : 0.0;
    const double in = grid > 0.0 ? snapFloor(pos, grid) : pos;
    const double out = quantizeOn && hasGrid() ? gridPos(gridBeat(in) + beats) : in + len;
    setLoop(in, out > in + 1.0 ? out : in + len, false);
}

void DeckPlayer::beatJump(double beats) noexcept
{
    if (loaded == nullptr || beats == 0.0) return;
    const double shift = hasGrid() ? gridPos(gridBeat(pos) + beats) - pos : beats * 0.5 * sr();
    if (loopActive)
    {
        loopIn = std::clamp(loopIn + shift, 0.0, length());
        loopOut = std::clamp(loopOut + shift, loopIn + 1.0, length());
    }
    jumpTo(pos + shift);
}

void DeckPlayer::loopResize(double factor) noexcept
{
    const double len = (loopOut - loopIn) * factor;
    if (len < 16.0) return;
    loopOut = std::min(length(), loopIn + len);
    stretch->needsReset = true;
    if (pos >= loopOut) jumpTo(loopIn + std::fmod(pos - loopIn, loopOut - loopIn));
}

void DeckPlayer::callMemory(int direction) noexcept
{
    // The next (or previous) memory cue from the cue point, in time order.
    int best = -1;
    const double from = cuePoint / sr();
    for (int i = 0; i < kMemoryCues; ++i)
    {
        const auto& c = memoryCues[static_cast<size_t>(i)];
        if (!c.set()) continue;
        const bool after = direction > 0 ? c.pos > from + 1.0e-3 : c.pos < from - 1.0e-3;
        if (!after) continue;
        if (best < 0 || (direction > 0 ? c.pos < memoryCues[static_cast<size_t>(best)].pos
                                       : c.pos > memoryCues[static_cast<size_t>(best)].pos))
            best = i;
    }
    if (best < 0) return;
    memoryCursor = best;
    const auto& c = memoryCues[static_cast<size_t>(best)];
    cuePoint = c.pos * sr();
    if (c.isLoop())
    {
        loopIn = c.pos * sr();
        loopOut = c.end * sr();
        loopArmed = true;
    }
    if (!playing) jumpTo(cuePoint);
}

void DeckPlayer::storeMemory() noexcept
{
    if (loaded == nullptr) return;
    Cue cue = loopActive ? Cue{loopIn / sr(), loopOut / sr()} : Cue{cuePoint / sr(), -1.0};
    for (const auto& c : memoryCues)
        if (c.set() && std::abs(c.pos - cue.pos) < 1.0e-3 && std::abs(c.end - cue.end) < 1.0e-3) return;
    for (auto& c : memoryCues)
        if (!c.set())
        {
            c = cue;
            bumpCues();
            return;
        }
}

void DeckPlayer::deleteMemory() noexcept
{
    if (memoryCursor >= 0 && memoryCues[static_cast<size_t>(memoryCursor)].set())
    {
        memoryCues[static_cast<size_t>(memoryCursor)] = {};
        memoryCursor = -1;
        bumpCues();
        return;
    }
    for (auto& c : memoryCues)
        if (c.set() && std::abs(c.pos * sr() - cuePoint) < 2.0)
        {
            c = {};
            bumpCues();
            return;
        }
}

void DeckPlayer::jog(Control control, double ticks) noexcept
{
    if (loaded == nullptr || ticks == 0.0) return;
    jogQuietBlocks = 0;
    const double ticksPerTurn = 720.0;
    const double perTick = kSecondsPerTurn / ticksPerTurn * sr();
    if (loopAdjust != 0 && loopActive)
    {
        const double d = ticks * kAdjustSecondsPerTick * sr();
        if (loopAdjust == 1) loopIn = std::clamp(loopIn + d, 0.0, loopOut - 16.0);
        else loopOut = std::clamp(loopOut + d, loopIn + 16.0, length());
        stretch->needsReset = true;
        return;
    }
    if (control == Control::jogSearch)
    {
        jumpTo(pos + ticks * kSearchSecondsPerTick * sr());
        return;
    }
    if (control == Control::jogScratch && touched)
    {
        platter = std::clamp(platter + ticks * perTick, 0.0, length());
        return;
    }
    if (moving())
        bendTicks += ticks; // the side: a nudge
    else
    {
        // Paused: the wheel moves the position; CUE then sets the cue point there.
        jumpTo(pos + ticks * perTick);
    }
}

// --- Rendering ------------------------------------------------------------------------------

float DeckPlayer::sampleAt(int channel, double p) const noexcept
{
    const auto& audio = *loaded->audio;
    const int len = audio.getNumSamples();
    const float* x = audio.getReadPointer(std::min(channel, audio.getNumChannels() - 1));
    const int i = static_cast<int>(std::floor(p));
    const auto f = static_cast<float>(p - i);
    auto at = [&](int k) { return k >= 0 && k < len ? x[k] : 0.0F; };
    const float xm1 = at(i - 1), x0 = at(i), x1 = at(i + 1), x2 = at(i + 2);
    const float c1 = 0.5F * (x1 - xm1);
    const float c2 = xm1 - 2.5F * x0 + 2.0F * x1 - 0.5F * x2;
    const float c3 = 0.5F * (x2 - xm1) + 1.5F * (x0 - x1);
    return ((c3 * f + c2) * f + c1) * f + x0;
}

void DeckPlayer::advance(double& p, double delta, bool record, juce::int64 at) noexcept
{
    const double before = p;
    p += delta;
    if (loopActive && loopOut > loopIn)
    {
        const double len = loopOut - loopIn;
        if (delta > 0.0 && before < loopOut && p >= loopOut)
        {
            p -= len;
            if (record) emitSegment(at, loopOut);
        }
        else if (delta < 0.0 && before >= loopIn && p < loopIn)
        {
            p += len;
            if (record) emitSegment(at, loopIn);
        }
    }
    else if (loopArmed && loopOut > loopIn && delta > 0.0 && before < loopIn && p >= loopIn)
    {
        loopActive = true;
        loopArmed = false;
    }
}

void DeckPlayer::readWrapped(float* const* dest, double& cursor, int n) const noexcept
{
    const auto& audio = *loaded->audio;
    const int len = audio.getNumSamples();
    const int chans = audio.getNumChannels();
    int written = 0;
    while (written < n)
    {
        auto c = static_cast<juce::int64>(std::floor(cursor));
        int run = n - written;
        if (loopActive && loopOut > loopIn && cursor < loopOut)
            run = std::min(run, std::max(1, static_cast<int>(std::ceil(loopOut - cursor))));
        for (int ch = 0; ch < 2; ++ch)
        {
            const float* src = audio.getReadPointer(std::min(ch, chans - 1));
            for (int i = 0; i < run; ++i)
            {
                const auto k = c + i;
                dest[ch][written + i] = k >= 0 && k < len ? src[k] : 0.0F;
            }
        }
        written += run;
        cursor += run;
        if (loopActive && loopOut > loopIn && cursor >= loopOut) cursor -= (loopOut - loopIn);
    }
}

void DeckPlayer::renderVarispeed(float* const* out, int offset, int n, double targetRate) noexcept
{
    const double k = 1.0 - std::exp(-1.0 / (kRateSmoothSec * outRate));
    for (int i = 0; i < n; ++i)
    {
        currentRate += (targetRate - currentRate) * k;
        const float g = smoothGain(currentRate);
        out[0][offset + i] = sampleAt(0, pos) * g;
        out[1][offset + i] = sampleAt(1, pos) * g;
        advance(pos, currentRate, true, clockNow + offset + i);
        if (pos >= length() || pos < 0.0)
        {
            pos = std::clamp(pos, 0.0, length());
            for (int j = i + 1; j < n; ++j) out[0][offset + j] = out[1][offset + j] = 0.0F;
            if (pos >= length() && moving())
            {
                playing = false;
                preview = Preview::none;
                currentRate = 0.0;
            }
            return;
        }
    }
}

void DeckPlayer::renderStretch(float* const* out, int offset, int n, double rate, double transpose) noexcept
{
    auto& s = *stretch;
    if (s.needsReset || !stretchActive)
    {
        s.engine.reset();
        s.engine.setTransposeFactor(static_cast<float>(transpose));
        const int seekLen = std::min(s.engine.outputSeekLength(static_cast<float>(rate)), static_cast<int>(s.seek[0].size()));
        s.cursor = pos;
        readWrapped(s.seekPtrs.data(), s.cursor, seekLen);
        s.engine.outputSeek(s.seekPtrs.data(), seekLen);
        s.streamRead = seekLen;
        s.streamAudible = 0.0;
        s.needsReset = false;
    }
    s.engine.setTransposeFactor(static_cast<float>(transpose));
    int done = 0;
    while (done < n)
    {
        const int chunk = std::min(n - done, maxBlock);
        s.streamAudible += chunk * rate;
        const double lead = s.engine.inputLatency() + rate * s.engine.outputLatency();
        const int wanted = juce::jlimit(0, s.capacity, static_cast<int>(std::lround(s.streamAudible + lead - s.streamRead)));
        readWrapped(s.inPtrs.data(), s.cursor, wanted);
        s.streamRead += wanted;
        float* outPtrs[2] = {out[0] + offset + done, out[1] + offset + done};
        s.engine.process(s.inPtrs.data(), wanted, outPtrs, chunk);
        done += chunk;
    }
    currentRate = rate;
    // The audible position moves with the output (loop-aware, recorded).
    double remaining = n * rate;
    const double step = 256.0 * rate;
    int at = 0;
    while (remaining > 1.0e-9)
    {
        const double d = std::min(remaining, step);
        advance(pos, d, true, clockNow + offset + at);
        remaining -= d;
        at += 256;
    }
    if (pos >= length())
    {
        pos = length();
        playing = false;
        preview = Preview::none;
        currentRate = 0.0;
        s.needsReset = true;
    }
}

void DeckPlayer::emitSegment(juce::int64 at, double endPos) noexcept
{
    if (recorder == nullptr || loaded == nullptr) return;
    DeckRecorder::Segment s;
    s.deck = deck;
    s.serial = loaded->serial;
    s.at = at;
    s.endPos = endPos / sr();
    s.startPos = pos / sr();
    s.tempo = tempoFactor();
    s.semitones = semitones;
    s.moving = lastKey.moving;
    s.audible = lastKey.audible;
    s.keyLock = lastKey.keyLock;
    s.scratch = lastKey.scratch;
    recorder->segment(s);
    segmentTempo = s.tempo;
    segmentStart = at;
}

void DeckPlayer::render(float* const* out, int n, const SyncSource& master, DeckRecorder* rec, juce::int64 clock,
                        bool audible) noexcept
{
    recorder = rec;
    clockNow = clock;
    audibleNow = audible;
    for (int c = 0; c < 2; ++c) std::fill(out[c], out[c] + n, 0.0F);
    if (loaded == nullptr || loaded->audio == nullptr || length() <= 0.0)
    {
        jumped = false;
        return;
    }

    // Sync: followers take the master's tempo and keep their beats on its beats.
    phaseCorrection = 0.0;
    if (syncEnabled && !isMaster && master.valid && master.bpm > 0.0 && loaded->bpm > 0.0)
    {
        syncTempo = master.bpm / loaded->bpm;
        if (moving() && master.moving && !touched && std::abs(bendTicks) < 0.5)
        {
            double diff = master.beatPhase - beatPhase();
            diff -= std::round(diff);
            phaseCorrection = std::clamp(diff * 0.6, -0.01, 0.01); // within 1%, about a beat a second
        }
    }

    // Jog decay: the side of the wheel bends while it moves.
    bendTicks *= std::exp(-static_cast<double>(n) / (kBendDecaySec * outRate));
    if (std::abs(bendTicks) < 0.01) bendTicks = 0.0;
    const double bend = std::clamp(bendTicks * kBendPerTick, -0.9, 0.9);
    const bool scratching = touched;
    const double srcRatio = sr() / outRate;
    const double motor = srcRatio * tempoFactor() * (1.0 + phaseCorrection);

    // What the deck is doing now, for the recorder: a change starts a new segment.
    const RecKey key{moving(), audible && moving(), keyLockOn || semitones != 0, scratching || bend != 0.0, semitones};
    const bool tempoMoved = std::abs(tempoFactor() - segmentTempo) > kTempoSplit * segmentTempo &&
                            static_cast<double>(clock - segmentStart) >= kMinSplitSec * outRate;
    if (forceSegment || jumped || key != lastKey || (tempoMoved && key.moving && !key.scratch))
    {
        lastKey = key;
        emitSegment(clock, jumped ? jumpFrom : pos);
        forceSegment = false;
    }
    jumped = false;

    const bool running = moving() || scratching;
    const bool useStretch = running && !scratching && bend == 0.0 && (keyLockOn || semitones != 0) && motor > 0.05;
    if (useStretch != stretchActive && runGain > 0.0F && std::abs(currentRate) > 1.0e-4)
    {
        tail = {true, pos, currentRate, kTailSamples};
        fadeInLeft = kTailSamples;
    }

    if (scratching)
    {
        const double follow = (platter - pos) / (kScratchFollowSec * outRate);
        renderVarispeed(out, 0, n, std::clamp(follow, -16.0, 16.0));
        stretchActive = false;
        stretch->needsReset = true;
    }
    else if (useStretch)
    {
        const double pitch = std::pow(2.0, semitones / 12.0) * (keyLockOn ? 1.0 : tempoFactor());
        renderStretch(out, 0, n, motor * (1.0 + bend), srcRatio * pitch);
        stretchActive = true;
    }
    else if (running)
    {
        renderVarispeed(out, 0, n, motor * (1.0 + bend));
        stretchActive = false;
        stretch->needsReset = true;
    }
    else
    {
        currentRate = 0.0;
        stretchActive = false;
    }
    runGain = running ? 1.0F : 0.0F;

    // Fade in after a jump or start, and cross-fade the old position out.
    if (fadeInLeft > 0)
    {
        for (int i = 0; i < n && fadeInLeft > 0; ++i, --fadeInLeft)
        {
            const float g = 1.0F - static_cast<float>(fadeInLeft) / kTailSamples;
            out[0][i] *= g;
            out[1][i] *= g;
        }
    }
    if (tail.active)
    {
        for (int i = 0; i < n && tail.left > 0; ++i, --tail.left)
        {
            const float g = static_cast<float>(tail.left) / kTailSamples * smoothGain(tail.rate);
            out[0][i] += sampleAt(0, tail.pos) * g;
            out[1][i] += sampleAt(1, tail.pos) * g;
            tail.pos += tail.rate;
        }
        if (tail.left <= 0) tail.active = false;
    }

    if (key.scratch && recorder != nullptr) recorder->platter(deck, clock + n, pos / sr());
    if (++jogQuietBlocks > 1000000) jogQuietBlocks = 1000000;
}

void DeckPlayer::endTake(DeckRecorder* r, juce::int64 at) noexcept
{
    if (r == nullptr || loaded == nullptr) return;
    recorder = r;
    const auto open = lastKey;
    lastKey = RecKey{};
    emitSegment(at, pos);
    lastKey = open;
    forceSegment = true;
}

void DeckPlayer::fillView(DeckView& v) const noexcept
{
    const double rate = sr();
    v.serial = loaded != nullptr ? loaded->serial : 0;
    v.loaded = loaded != nullptr;
    v.lengthSec = length() / rate;
    v.posSec = pos / rate;
    v.cueSec = cuePoint / rate;
    v.playing = playing;
    v.previewing = preview != Preview::none;
    v.atCue = std::abs(pos - cuePoint) <= kAtCueTolerance * rate;
    v.loopActive = loopActive;
    v.loopArmed = loopArmed;
    v.loopInPending = loopInPending;
    v.loopInSec = (loopInPending ? pendingIn : loopIn) / rate;
    v.loopOutSec = loopOut / rate;
    v.loopAdjust = loopAdjust;
    v.sync = syncEnabled;
    v.master = isMaster;
    v.keyLock = keyLockOn;
    v.quantize = quantizeOn;
    v.touched = touched;
    v.slider = slider;
    v.range = range;
    v.tempo = tempoFactor();
    v.bpm = loaded != nullptr ? loaded->bpm : 0.0;
    v.effectiveBpm = effectiveBpm();
    v.rate = currentRate;
    v.semitones = semitones;
    v.tempoHeld = tempoHeld;
    v.takeover = takeover;
    v.hotCues = hotCues;
    v.memoryCues = memoryCues;
    v.cueRevision = cueRevision;
}

} // namespace stemcel::perf
