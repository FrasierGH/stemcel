#pragma once

// Offline RNNoise-based denoiser for a recorded vocal take (RecordingCleanup). It runs
// on a worker thread, so it may allocate and is NOT real-time code. RNNoise is an RNN
// noise suppressor that operates on mono 48 kHz / 480-sample frames, so this wrapper
// resamples each channel to 48 kHz, runs the network, compensates the model's
// one-frame algorithmic delay, blends the denoised signal with the dry signal at
// 48 kHz (so a partial blend never combs against a differently-delayed dry copy),
// and resamples back to the take's sample rate. It targets broadband noise; it does
// NOT remove pitched instrument bleed or reverb.

#include <juce_audio_basics/juce_audio_basics.h>

#include <functional>

namespace stemcel
{

// Stateless offline vocal denoiser. `process` mutates `buffer` in place at
// `sampleRate`, mixing in `wetAmount` (0 = bypass, 1 = fully denoised) of the
// RNNoise output. It is a guaranteed no-op when `wetAmount <= 0`, the buffer is
// empty, or any resampling/allocation step fails, so it can never corrupt or
// drop the stem. Non-finite input samples are sanitised to zero. Each channel is
// denoised independently with its own network state.
//
// `onProgress`, when set, is called with a monotonic 0..1 fraction as the
// (potentially multi-second) denoise advances, so callers can keep a progress
// bar moving instead of freezing for the whole pass.
class VocalDenoiser
{
public:
    static void process(juce::AudioBuffer<float>& buffer, double sampleRate, float wetAmount,
                        const std::function<void(double)>& onProgress = {});
};

} // namespace stemcel
