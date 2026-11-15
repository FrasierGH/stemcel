#pragma once

// The headphone cue on a second device: the DJ controller's own sound card (its headphone
// socket), when the master plays somewhere else — speakers on the computer's sound card, an
// interface. Outputs 3–4 carry the cue and 1–2 a copy of the master, so the controller's
// HEADPHONES MIX knob still blends the two in its hardware (a two-output device gets the cue
// alone). The engine's callback pushes each block into a ring; this device's own callback
// reads it back, resampling for a different rate and gently for clock drift between the two.

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <memory>
#include <vector>

namespace stemcel::perf
{

class CueOutput final : private juce::AudioIODeviceCallback
{
  public:
    CueOutput();
    ~CueOutput() override;
    CueOutput(const CueOutput&) = delete;
    CueOutput& operator=(const CueOutput&) = delete;

    // Message thread. Opens the output device `deviceName` of driver `typeName` (the main
    // device's type) and starts it; `sourceRate` is the engine's rate.
    bool open(const juce::String& typeName, const juce::String& deviceName, double sourceRate, juce::String& error);
    void close();
    bool isOpen() const noexcept { return running.load(std::memory_order_acquire); }
    juce::String deviceName() const;
    bool hasMasterPair() const noexcept { return outputChannels >= 4; }

    // The engine's audio thread: one block of master and cue.
    void push(const float* masterL, const float* masterR, const float* cueL, const float* cueR, int numSamples) noexcept;

  private:
    void audioDeviceIOCallbackWithContext(const float* const* inputs, int numInputs, float* const* outputs, int numOutputs,
                                          int numSamples, const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* /*device*/) override {}
    void audioDeviceStopped() override {}

    juce::AudioDeviceManager factory; // a device-type factory only, never initialised
    std::unique_ptr<juce::AudioIODevice> device;
    int outputChannels = 0;
    std::atomic<bool> running{false};

    // Ring of frames: master L, master R, cue L, cue R.
    static constexpr int kRingFrames = 1 << 15;
    std::vector<float> ring = std::vector<float>(static_cast<size_t>(kRingFrames) * 4);
    std::atomic<juce::int64> written{0}; // frames pushed so far
    double readPos = 0.0;                // frames read so far (fractional)
    double baseRatio = 1.0;              // source frames per output frame
    double drift = 0.0;
    bool primed = false;
    int targetFill = 2048;
};

} // namespace stemcel::perf
