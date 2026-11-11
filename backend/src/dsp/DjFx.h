#pragma once

// DJ effects, as rekordbox and the DDJ-400 have them (the DJM-900NXS2 set):
//   Beat FX     — Delay, Echo, Ping Pong, Spiral, Reverb, Trans, Filter, Flanger, Phaser,
//                 Pitch, Slip Roll, Roll, Vinyl Brake, Helix; timed in beats of the track's
//                 tempo, LEVEL/DEPTH their amount.
//   Sound Color — Space, Dub Echo, Sweep, Noise, Crush, Filter on a channel's FILTER knob
//                 (centre off, one character each way).
//   Release FX  — Vinyl Brake, Echo, Spin Back.
// One DjFxChain per channel: in the performance mixer live, and in a track's TrackChain on
// the timeline, where automation drives the same parameters — so a recorded take plays back
// as it sounded. Buffers are allocated once (`allocate`, message thread) and only for tracks
// that use the effects; processing is allocation- and lock-free.

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace stemcel::djfx
{

enum class BeatFx : int
{
    delay,
    echo,
    pingPong,
    spiral,
    reverb,
    trans,
    filter,
    flanger,
    phaser,
    pitch,
    slipRoll,
    roll,
    vinylBrake,
    helix,
    spinBack, // Release FX only
    count
};
constexpr int kBeatFxSelectable = static_cast<int>(BeatFx::helix) + 1; // FX SELECT steps through these

enum class ColorFx : int
{
    filter,
    space,
    dubEcho,
    sweep,
    noise,
    crush,
    count
};

// The BEAT ◁/▷ steps, in beats.
constexpr std::array<double, 10> kBeatSteps{1.0 / 16.0, 1.0 / 8.0, 1.0 / 4.0, 1.0 / 2.0, 3.0 / 4.0, 1.0, 2.0, 4.0, 8.0, 16.0};
constexpr int kDefaultBeatStep = 5; // 1 beat

const char* beatFxName(int type) noexcept;   // "ECHO"
const char* colorFxName(int type) noexcept;  // "SPACE"
juce::String beatStepLabel(int step);        // "1/2"

// One Beat FX unit (the Beat FX section, or a deck's Pad FX).
class BeatFxUnit
{
  public:
    BeatFxUnit();
    ~BeatFxUnit();
    BeatFxUnit(BeatFxUnit&&) noexcept;
    BeatFxUnit& operator=(BeatFxUnit&&) noexcept;

    void allocate(double sampleRate);   // message thread
    bool allocated() const noexcept { return state != nullptr; }
    void reset() noexcept;

    // Audio thread (or before playback starts).
    void setType(int type) noexcept;
    void setBeatStep(int step) noexcept;
    void setLevel(float level) noexcept;
    void setOn(bool on) noexcept;
    void setBpm(double bpm) noexcept;
    bool isOn() const noexcept;
    void process(float* left, float* right, int numSamples) noexcept;

  private:
    struct State;
    std::unique_ptr<State> state;
};

class ColorFxUnit
{
  public:
    ColorFxUnit();
    ~ColorFxUnit();
    ColorFxUnit(ColorFxUnit&&) noexcept;
    ColorFxUnit& operator=(ColorFxUnit&&) noexcept;

    void allocate(double sampleRate);
    bool allocated() const noexcept { return state != nullptr; }
    void reset() noexcept;
    void setType(int type) noexcept;
    void setAmount(float amount) noexcept; // −1..1 (0 = off)
    void setBpm(double bpm) noexcept;
    void process(float* left, float* right, int numSamples) noexcept;

  private:
    struct State;
    std::unique_ptr<State> state;
};

// A channel's effects: Sound Color FX (before the fader), Beat FX and Pad FX (after it).
class DjFxChain
{
  public:
    void allocate(double sampleRate)
    {
        color.allocate(sampleRate);
        beat.allocate(sampleRate);
        pad.allocate(sampleRate);
        ready.store(true, std::memory_order_release);
    }
    bool isAllocated() const noexcept { return ready.load(std::memory_order_acquire); }
    void reset() noexcept
    {
        if (!isAllocated()) return;
        color.reset();
        beat.reset();
        pad.reset();
    }
    void setBpm(double bpm) noexcept
    {
        if (!isAllocated()) return;
        color.setBpm(bpm);
        beat.setBpm(bpm);
        pad.setBpm(bpm);
    }
    void processColor(juce::AudioBuffer<float>& b, int start, int n) noexcept
    {
        if (isAllocated() && b.getNumChannels() >= 2)
            color.process(b.getWritePointer(0, start), b.getWritePointer(1, start), n);
    }
    void processBeat(juce::AudioBuffer<float>& b, int start, int n) noexcept
    {
        if (!isAllocated() || b.getNumChannels() < 2) return;
        beat.process(b.getWritePointer(0, start), b.getWritePointer(1, start), n);
        pad.process(b.getWritePointer(0, start), b.getWritePointer(1, start), n);
    }

    ColorFxUnit color;
    BeatFxUnit beat, pad;

    DjFxChain() = default;
    DjFxChain(DjFxChain&& o) noexcept
        : color(std::move(o.color)), beat(std::move(o.beat)), pad(std::move(o.pad)), ready(o.ready.load())
    {
    }
    DjFxChain& operator=(DjFxChain&& o) noexcept
    {
        color = std::move(o.color);
        beat = std::move(o.beat);
        pad = std::move(o.pad);
        ready.store(o.ready.load());
        return *this;
    }

  private:
    std::atomic<bool> ready{false};
};

} // namespace stemcel::djfx
