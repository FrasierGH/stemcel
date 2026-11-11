#include "DjFx.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

namespace stemcel::djfx
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kMaxDelaySeconds = 4.0;  // as the DJM's delay range
constexpr double kHistorySeconds = 8.0;   // what rolls, the brake and spin back read from
constexpr double kFadeSeconds = 0.006;

constexpr const char* kBeatNames[] = {"DELAY", "ECHO",  "PING PONG", "SPIRAL", "REVERB",     "TRANS", "FILTER", "FLANGER",
                                      "PHASER", "PITCH", "SLIP ROLL", "ROLL",   "VINYL BRAKE", "HELIX", "SPIN BACK"};
constexpr const char* kColorNames[] = {"FILTER", "SPACE", "DUB ECHO", "SWEEP", "NOISE", "CRUSH"};

// A biquad (RBJ cookbook), coefficients set per block.
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1[2] = {0, 0}, z2[2] = {0, 0};

    void reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0.0; }
    void set(int kind, double sr, double hz, double q)
    {
        hz = std::clamp(hz, 10.0, sr * 0.45);
        const double w = 2.0 * kPi * hz / sr, cw = std::cos(w), sw = std::sin(w), alpha = sw / (2.0 * q);
        double nb0 = 1, nb1 = 0, nb2 = 0, na0 = 1, na1 = 0, na2 = 0;
        switch (kind)
        {
            case 0: nb0 = (1 - cw) / 2; nb1 = 1 - cw; nb2 = (1 - cw) / 2; break;  // low-pass
            case 1: nb0 = (1 + cw) / 2; nb1 = -(1 + cw); nb2 = (1 + cw) / 2; break; // high-pass
            case 2: nb0 = alpha; nb1 = 0; nb2 = -alpha; break;                      // band-pass
            default: nb0 = 1; nb1 = -2 * cw; nb2 = 1; break;                        // notch
        }
        na0 = 1 + alpha;
        na1 = -2 * cw;
        na2 = 1 - alpha;
        b0 = nb0 / na0;
        b1 = nb1 / na0;
        b2 = nb2 / na0;
        a1 = na1 / na0;
        a2 = na2 / na0;
    }
    float run(int ch, float x)
    {
        const double y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return static_cast<float>(y);
    }
};

// A stereo ring buffer with fractional reads.
struct Ring
{
    std::vector<float> l, r;
    int size = 1, write = 0;
    void allocate(int n)
    {
        size = std::max(16, n);
        l.assign(static_cast<size_t>(size), 0.0F);
        r.assign(static_cast<size_t>(size), 0.0F);
        write = 0;
    }
    void clear()
    {
        std::fill(l.begin(), l.end(), 0.0F);
        std::fill(r.begin(), r.end(), 0.0F);
    }
    void push(float a, float b)
    {
        l[static_cast<size_t>(write)] = a;
        r[static_cast<size_t>(write)] = b;
        if (++write >= size) write = 0;
    }
    // `ago` samples before the next write (1 = the last sample written).
    float read(const std::vector<float>& buf, double ago) const
    {
        double p = static_cast<double>(write) - ago;
        while (p < 0.0) p += size;
        while (p >= size) p -= size;
        const int i = static_cast<int>(p);
        const auto f = static_cast<float>(p - i);
        const int j = i + 1 >= size ? 0 : i + 1;
        return buf[static_cast<size_t>(i)] + (buf[static_cast<size_t>(j)] - buf[static_cast<size_t>(i)]) * f;
    }
    float readL(double ago) const { return read(l, ago); }
    float readR(double ago) const { return read(r, ago); }
};

float smoothStep(float g, float target, float k) { return g + (target - g) * k; }
} // namespace

const char* beatFxName(int type) noexcept
{
    return type >= 0 && type < static_cast<int>(BeatFx::count) ? kBeatNames[type] : "";
}
const char* colorFxName(int type) noexcept
{
    return type >= 0 && type < static_cast<int>(ColorFx::count) ? kColorNames[type] : "";
}
juce::String beatStepLabel(int step)
{
    static const char* labels[] = {"1/16", "1/8", "1/4", "1/2", "3/4", "1", "2", "4", "8", "16"};
    return step >= 0 && step < static_cast<int>(kBeatSteps.size()) ? labels[step] : "";
}

// --- Beat FX --------------------------------------------------------------------------------

struct BeatFxUnit::State
{
    double sr = 44100.0;
    std::atomic<int> type{static_cast<int>(BeatFx::echo)};
    std::atomic<int> step{kDefaultBeatStep};
    std::atomic<float> level{0.5F};
    std::atomic<bool> on{false};
    std::atomic<double> bpm{120.0};

    Ring delay, history;
    juce::Reverb reverb;
    Biquad filterBq, toneBq;
    std::array<double, 12> apState{};
    float onGain = 0.0F; // 0..1 cross-fade in and out
    bool wasOn = false;
    int lastType = -1;
    double delaySamples = 22050.0;
    double lfo = 0.0;
    // Rolls: the loop's length and where it started (in history samples ago).
    double rollLen = 0.0, rollPos = 0.0;
    int rollRecorded = 0;
    // Brake and spin back: a read head moving through the history.
    double headAgo = 0.0, headSpeed = 1.0, brakeTime = 0.0;
    // Pitch: two grains.
    double grainPhase = 0.0;
};

BeatFxUnit::BeatFxUnit() = default;
BeatFxUnit::~BeatFxUnit() = default;
BeatFxUnit::BeatFxUnit(BeatFxUnit&&) noexcept = default;
BeatFxUnit& BeatFxUnit::operator=(BeatFxUnit&&) noexcept = default;

void BeatFxUnit::allocate(double sampleRate)
{
    auto s = std::make_unique<State>();
    s->sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    s->delay.allocate(static_cast<int>(kMaxDelaySeconds * s->sr) + 8);
    s->history.allocate(static_cast<int>(kHistorySeconds * s->sr) + 8);
    s->reverb.setSampleRate(s->sr);
    if (state != nullptr)
    {
        s->type.store(state->type.load());
        s->step.store(state->step.load());
        s->level.store(state->level.load());
        s->bpm.store(state->bpm.load());
    }
    state = std::move(s);
}

void BeatFxUnit::reset() noexcept
{
    if (state == nullptr) return;
    state->delay.clear();
    state->reverb.reset();
    state->filterBq.reset();
    state->toneBq.reset();
    state->apState.fill(0.0);
    state->onGain = 0.0F;
    state->wasOn = false;
}

void BeatFxUnit::setType(int type) noexcept
{
    if (state != nullptr) state->type.store(std::clamp(type, 0, static_cast<int>(BeatFx::count) - 1), std::memory_order_relaxed);
}
void BeatFxUnit::setBeatStep(int step) noexcept
{
    if (state != nullptr) state->step.store(std::clamp(step, 0, static_cast<int>(kBeatSteps.size()) - 1), std::memory_order_relaxed);
}
void BeatFxUnit::setLevel(float level) noexcept
{
    if (state != nullptr) state->level.store(std::clamp(level, 0.0F, 1.0F), std::memory_order_relaxed);
}
void BeatFxUnit::setOn(bool on) noexcept
{
    if (state != nullptr) state->on.store(on, std::memory_order_relaxed);
}
void BeatFxUnit::setBpm(double bpm) noexcept
{
    if (state != nullptr && bpm > 20.0) state->bpm.store(std::min(bpm, 300.0), std::memory_order_relaxed);
}
bool BeatFxUnit::isOn() const noexcept { return state != nullptr && state->on.load(std::memory_order_relaxed); }

void BeatFxUnit::process(float* L, float* R, int n) noexcept
{
    if (state == nullptr) return;
    auto& s = *state;
    const auto type = static_cast<BeatFx>(s.type.load(std::memory_order_relaxed));
    const bool on = s.on.load(std::memory_order_relaxed);
    const float level = s.level.load(std::memory_order_relaxed);
    const double beatSec = 60.0 / s.bpm.load(std::memory_order_relaxed);
    const double beats = kBeatSteps[static_cast<size_t>(std::clamp(s.step.load(std::memory_order_relaxed), 0, 9))];
    const double sr = s.sr;
    const auto fadeK = static_cast<float>(1.0 - std::exp(-1.0 / (kFadeSeconds * sr)));
    const bool trails = type == BeatFx::delay || type == BeatFx::echo || type == BeatFx::pingPong || type == BeatFx::spiral ||
                        type == BeatFx::reverb || type == BeatFx::helix;
    const bool rising = on && !s.wasOn;
    if (static_cast<int>(type) != s.lastType)
    {
        s.lastType = static_cast<int>(type);
        s.filterBq.reset();
        s.apState.fill(0.0);
    }

    // Idle: nothing on and nothing ringing.
    if (!on && s.onGain < 1.0e-4F && !trails)
    {
        s.wasOn = false;
        for (int i = 0; i < n; ++i) s.history.push(L[i], R[i]);
        return;
    }

    const double targetDelay = std::clamp(beatSec * beats * sr, 16.0, kMaxDelaySeconds * sr - 4.0);
    if (rising)
    {
        if (!trails) s.delay.clear();
        s.rollLen = std::clamp(beatSec * beats * sr, 64.0, static_cast<double>(s.delay.size - 1));
        s.rollRecorded = 0;
        s.rollPos = 0.0;
        s.headAgo = 1.0;
        s.headSpeed = type == BeatFx::spinBack ? -3.0 : 1.0;
        s.brakeTime = 0.0;
        if (type == BeatFx::reverb) s.reverb.reset();
    }
    s.wasOn = on;

    // Per-block parameters.
    if (type == BeatFx::reverb)
    {
        juce::Reverb::Parameters p;
        p.roomSize = static_cast<float>(std::clamp(0.35 + 0.06 * static_cast<double>(s.step.load()), 0.3, 0.98));
        p.damping = 0.45F;
        p.wetLevel = 1.0F;
        p.dryLevel = 0.0F;
        p.width = 1.0F;
        s.reverb.setParameters(p);
    }
    const double lfoInc = 1.0 / std::max(1.0, beatSec * beats * sr * (type == BeatFx::filter || type == BeatFx::flanger || type == BeatFx::phaser ? 2.0 : 1.0));

    for (int i = 0; i < n; ++i)
    {
        const float dryL = L[i], dryR = R[i];
        s.history.push(dryL, dryR);
        s.onGain = smoothStep(s.onGain, on ? 1.0F : 0.0F, fadeK);
        const float g = s.onGain;
        s.delaySamples += (targetDelay - s.delaySamples) * 0.0005;
        s.lfo += lfoInc;
        if (s.lfo >= 1.0) s.lfo -= 1.0;
        float outL = dryL, outR = dryR;

        switch (type)
        {
            case BeatFx::delay:
            case BeatFx::echo:
            case BeatFx::spiral:
            case BeatFx::helix:
            {
                const float wl = s.delay.readL(s.delaySamples), wr = s.delay.readR(s.delaySamples);
                float fb = type == BeatFx::delay ? 0.0F : type == BeatFx::echo ? 0.55F : type == BeatFx::spiral ? 0.72F : 0.92F;
                const float inGain = type == BeatFx::helix ? level * g : g;
                float fl = wl * fb, fr = wr * fb;
                if (type == BeatFx::spiral)
                {
                    // The repeats blur into a swirl: an all-pass on the feedback path.
                    const double c = 0.6;
                    double y = -c * fl + s.apState[0];
                    s.apState[0] = fl + c * y;
                    fl = static_cast<float>(y);
                    y = -c * fr + s.apState[1];
                    s.apState[1] = fr + c * y;
                    fr = static_cast<float>(y);
                }
                s.delay.push(dryL * inGain + fl, dryR * inGain + fr);
                const float wet = type == BeatFx::helix ? 1.0F : level;
                outL = dryL + wl * wet;
                outR = dryR + wr * wet;
                break;
            }
            case BeatFx::pingPong:
            {
                const float wl = s.delay.readL(s.delaySamples), wr = s.delay.readR(s.delaySamples);
                const float mono = 0.5F * (dryL + dryR) * g;
                s.delay.push(mono + wr * 0.6F, wl * 0.6F);
                outL = dryL + wl * level;
                outR = dryR + wr * level;
                break;
            }
            case BeatFx::reverb:
            {
                float l = dryL * g, r = dryR * g;
                s.reverb.processStereo(&l, &r, 1);
                outL = dryL + l * level;
                outR = dryR + r * level;
                break;
            }
            case BeatFx::trans:
            {
                // A gate on the beat: open for the first half of each step.
                const double phase = s.lfo;
                const float gate = phase < 0.5 ? 1.0F : 0.0F;
                const float depth = level * g;
                const float k = 1.0F - depth * (1.0F - gate);
                outL = dryL * k;
                outR = dryR * k;
                break;
            }
            case BeatFx::filter:
            {
                if ((i & 31) == 0)
                {
                    const double sweep = 0.5 - 0.5 * std::cos(2.0 * kPi * s.lfo);
                    s.filterBq.set(0, sr, 180.0 * std::pow(60.0, sweep), 2.5);
                }
                const float fl = s.filterBq.run(0, dryL), fr = s.filterBq.run(1, dryR);
                const float mix = level * g;
                outL = dryL + (fl - dryL) * mix;
                outR = dryR + (fr - dryR) * mix;
                break;
            }
            case BeatFx::flanger:
            {
                const double d = (0.0015 + 0.0045 * (0.5 - 0.5 * std::cos(2.0 * kPi * s.lfo))) * sr;
                const float wl = s.delay.readL(d), wr = s.delay.readR(d);
                s.delay.push(dryL + wl * 0.55F, dryR + wr * 0.55F);
                const float mix = level * g;
                outL = dryL + (0.5F * (dryL + wl) - dryL) * mix;
                outR = dryR + (0.5F * (dryR + wr) - dryR) * mix;
                break;
            }
            case BeatFx::phaser:
            {
                const double f = 250.0 * std::pow(8.0, 0.5 - 0.5 * std::cos(2.0 * kPi * s.lfo));
                const double t = std::tan(kPi * f / sr);
                const double c = (t - 1.0) / (t + 1.0);
                double xl = dryL, xr = dryR;
                for (int st = 0; st < 6; ++st)
                {
                    double& zl = s.apState[static_cast<size_t>(st)];
                    double& zr = s.apState[static_cast<size_t>(st + 6)];
                    const double yl = c * xl + zl;
                    zl = xl - c * yl;
                    xl = yl;
                    const double yr = c * xr + zr;
                    zr = xr - c * yr;
                    xr = yr;
                }
                const float mix = level * g;
                outL = dryL + (0.5F * (dryL + static_cast<float>(xl)) - dryL) * mix;
                outR = dryR + (0.5F * (dryR + static_cast<float>(xr)) - dryR) * mix;
                break;
            }
            case BeatFx::pitch:
            {
                // Two grains reading the recent past at a shifted speed, cross-faded.
                const double ratio = std::pow(2.0, (static_cast<double>(level) - 0.5) * 2.0); // ½× .. 2×
                const double window = 0.06 * sr;
                s.grainPhase += (1.0 - ratio) / window;
                if (s.grainPhase >= 1.0) s.grainPhase -= 1.0;
                if (s.grainPhase < 0.0) s.grainPhase += 1.0;
                double gl = 0.0, gr = 0.0;
                for (int grain = 0; grain < 2; ++grain)
                {
                    double ph = s.grainPhase + 0.5 * grain;
                    if (ph >= 1.0) ph -= 1.0;
                    const double ago = 1.0 + ph * window;
                    const double w = std::sin(kPi * ph);
                    gl += s.history.readL(ago) * w * w;
                    gr += s.history.readR(ago) * w * w;
                }
                outL = dryL + (static_cast<float>(gl) - dryL) * g;
                outR = dryR + (static_cast<float>(gr) - dryR) * g;
                break;
            }
            case BeatFx::roll:
            case BeatFx::slipRoll:
            {
                // The first pass is heard as it is recorded (into the delay buffer, as the
                // loop); then the loop repeats. SLIP ROLL's length follows LEVEL/DEPTH.
                float rl = dryL, rr = dryR;
                if (on)
                {
                    const int capacity = static_cast<int>(s.rollLen);
                    if (s.rollRecorded < capacity)
                    {
                        s.delay.l[static_cast<size_t>(s.rollRecorded)] = dryL;
                        s.delay.r[static_cast<size_t>(s.rollRecorded)] = dryR;
                        ++s.rollRecorded;
                    }
                    else
                    {
                        double len = s.rollLen;
                        if (type == BeatFx::slipRoll) len = std::max(64.0, s.rollLen * (0.25 + 0.75 * level));
                        const auto at = static_cast<size_t>(std::min(s.rollPos, len - 1.0));
                        rl = s.delay.l[at];
                        rr = s.delay.r[at];
                        s.rollPos += 1.0;
                        if (s.rollPos >= len) s.rollPos = 0.0;
                    }
                }
                const float mix = type == BeatFx::roll ? g * (0.5F + 0.5F * level) : g;
                outL = dryL + (rl - dryL) * mix;
                outR = dryR + (rr - dryR) * mix;
                break;
            }
            case BeatFx::vinylBrake:
            case BeatFx::spinBack:
            {
                // A read head that falls behind the live signal as the record slows (or spins
                // backwards), from the moment the effect came on.
                float bl = dryL, br = dryR;
                if (on)
                {
                    const double total = std::max(0.05, beatSec * beats * (type == BeatFx::vinylBrake ? (0.25 + level * 1.75) : 1.0));
                    s.brakeTime += 1.0 / sr;
                    const double u = std::min(1.0, s.brakeTime / total);
                    s.headSpeed = type == BeatFx::vinylBrake ? (1.0 - u) * (1.0 - u) : -3.0 * (1.0 - u);
                    s.headAgo += 1.0 - s.headSpeed; // the live signal moves on by 1, the head by headSpeed
                    s.headAgo = std::min(s.headAgo, kHistorySeconds * sr * 0.95);
                    const float fade = static_cast<float>(1.0 - u * u);
                    bl = s.history.readL(s.headAgo) * fade;
                    br = s.history.readR(s.headAgo) * fade;
                }
                outL = dryL + (bl - dryL) * g;
                outR = dryR + (br - dryR) * g;
                break;
            }
            default:
                break;
        }
        L[i] = outL;
        R[i] = outR;
    }
}

// --- Sound Color FX -------------------------------------------------------------------------

struct ColorFxUnit::State
{
    double sr = 44100.0;
    std::atomic<int> type{static_cast<int>(ColorFx::filter)};
    std::atomic<float> amount{0.0F};
    std::atomic<double> bpm{120.0};
    Ring delay;
    juce::Reverb reverb;
    Biquad bq, wetBq;
    juce::Random random;
    float smoothed = 0.0F;
    float holdL = 0.0F, holdR = 0.0F;
    double holdPhase = 0.0;
};

ColorFxUnit::ColorFxUnit() = default;
ColorFxUnit::~ColorFxUnit() = default;
ColorFxUnit::ColorFxUnit(ColorFxUnit&&) noexcept = default;
ColorFxUnit& ColorFxUnit::operator=(ColorFxUnit&&) noexcept = default;

void ColorFxUnit::allocate(double sampleRate)
{
    auto s = std::make_unique<State>();
    s->sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    s->delay.allocate(static_cast<int>(kMaxDelaySeconds * s->sr) + 8);
    s->reverb.setSampleRate(s->sr);
    juce::Reverb::Parameters p;
    p.roomSize = 0.85F;
    p.damping = 0.4F;
    p.wetLevel = 1.0F;
    p.dryLevel = 0.0F;
    s->reverb.setParameters(p);
    if (state != nullptr) s->type.store(state->type.load());
    state = std::move(s);
}

void ColorFxUnit::reset() noexcept
{
    if (state == nullptr) return;
    state->delay.clear();
    state->reverb.reset();
    state->bq.reset();
    state->wetBq.reset();
}

void ColorFxUnit::setType(int type) noexcept
{
    if (state != nullptr) state->type.store(std::clamp(type, 0, static_cast<int>(ColorFx::count) - 1), std::memory_order_relaxed);
}
void ColorFxUnit::setAmount(float amount) noexcept
{
    if (state != nullptr) state->amount.store(std::clamp(amount, -1.0F, 1.0F), std::memory_order_relaxed);
}
void ColorFxUnit::setBpm(double bpm) noexcept
{
    if (state != nullptr && bpm > 20.0) state->bpm.store(std::min(bpm, 300.0), std::memory_order_relaxed);
}

void ColorFxUnit::process(float* L, float* R, int n) noexcept
{
    if (state == nullptr) return;
    auto& s = *state;
    const auto type = static_cast<ColorFx>(s.type.load(std::memory_order_relaxed));
    if (type == ColorFx::filter) return; // the channel's own filter (ToneEq) does it
    const float target = s.amount.load(std::memory_order_relaxed);
    const bool echoes = type == ColorFx::dubEcho || type == ColorFx::space;
    if (std::abs(target) < 1.0e-4F && std::abs(s.smoothed) < 1.0e-4F && !echoes) return;
    const double sr = s.sr;
    const auto k = static_cast<float>(1.0 - std::exp(-1.0 / (0.02 * sr)));
    // Per-block filter settings follow the knob.
    const float a = s.smoothed;
    const float mag = std::abs(a);
    const bool left = a < 0.0F;
    switch (type)
    {
        case ColorFx::space:
        case ColorFx::dubEcho:
            s.wetBq.set(left ? 0 : 1, sr, left ? 6000.0 * std::pow(0.08, mag) : 150.0 * std::pow(20.0, mag), 0.8);
            break;
        case ColorFx::sweep:
            s.bq.set(left ? 3 : 2, sr, 200.0 * std::pow(40.0, mag), left ? 1.2 : 2.0);
            break;
        case ColorFx::noise:
            s.bq.set(left ? 0 : 1, sr, left ? 12000.0 * std::pow(0.03, mag) : 200.0 * std::pow(40.0, mag), 0.9);
            break;
        case ColorFx::crush:
            s.wetBq.set(left ? 0 : 1, sr, left ? 14000.0 * std::pow(0.15, mag) : 80.0 * std::pow(10.0, mag), 0.7);
            break;
        default:
            break;
    }
    const double beatSec = 60.0 / s.bpm.load(std::memory_order_relaxed);
    const double echoDelay = std::clamp(beatSec * 0.75 * sr, 64.0, kMaxDelaySeconds * sr - 4.0);
    for (int i = 0; i < n; ++i)
    {
        s.smoothed = smoothStep(s.smoothed, target, k);
        const float m = std::abs(s.smoothed);
        const float dl = L[i], dr = R[i];
        float ol = dl, orr = dr;
        switch (type)
        {
            case ColorFx::space:
            {
                float l = dl * m, r = dr * m;
                s.reverb.processStereo(&l, &r, 1);
                ol = dl + s.wetBq.run(0, l) * 0.8F;
                orr = dr + s.wetBq.run(1, r) * 0.8F;
                break;
            }
            case ColorFx::dubEcho:
            {
                const float wl = s.delay.readL(echoDelay), wr = s.delay.readR(echoDelay);
                const float fl = s.wetBq.run(0, wl), fr = s.wetBq.run(1, wr);
                s.delay.push(dl * m + fl * 0.68F, dr * m + fr * 0.68F);
                ol = dl + fl;
                orr = dr + fr;
                break;
            }
            case ColorFx::sweep:
                ol = dl + (s.bq.run(0, dl) * (left ? 1.0F : 1.6F) - dl) * m;
                orr = dr + (s.bq.run(1, dr) * (left ? 1.0F : 1.6F) - dr) * m;
                break;
            case ColorFx::noise:
            {
                const float noise = (s.random.nextFloat() * 2.0F - 1.0F) * 0.18F * m;
                ol = dl + s.bq.run(0, noise);
                orr = dr + s.bq.run(1, noise);
                break;
            }
            case ColorFx::crush:
            {
                // Fewer bits and a lower rate as the knob turns.
                const float steps = std::pow(2.0F, 12.0F - 9.0F * m);
                s.holdPhase += 1.0 - 0.9 * m;
                if (s.holdPhase >= 1.0)
                {
                    s.holdPhase -= 1.0;
                    s.holdL = std::round(dl * steps) / steps;
                    s.holdR = std::round(dr * steps) / steps;
                }
                ol = dl + (s.wetBq.run(0, s.holdL) - dl) * std::min(1.0F, m * 1.5F);
                orr = dr + (s.wetBq.run(1, s.holdR) - dr) * std::min(1.0F, m * 1.5F);
                break;
            }
            default:
                break;
        }
        L[i] = ol;
        R[i] = orr;
    }
}

} // namespace stemcel::djfx
