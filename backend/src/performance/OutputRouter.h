#pragma once

// The device's output: the master (the whole engine, with its gain, limiter and meters) on
// outputs 1–2, and the performance mode's headphone cue on 3–4 when the device has them — as
// the DDJ-400's sound card expects (master 1/2, headphones 3/4).

#include "PerformanceEngine.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace stemcel::perf
{

class OutputRouter final : public juce::AudioSource
{
  public:
    OutputRouter(juce::AudioSource& masterSource, PerformanceEngine& perf) : master(masterSource), performance(perf) {}

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override
    {
        master.prepareToPlay(samplesPerBlockExpected, sampleRate);
    }
    void releaseResources() override { master.releaseResources(); }

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        if (info.buffer == nullptr) return;
        const int channels = info.buffer->getNumChannels();
        if (channels <= 2)
        {
            master.getNextAudioBlock(info);
            return;
        }
        // The master sees a stereo buffer; the rest carry the cue.
        float* stereo[2] = {info.buffer->getWritePointer(0), info.buffer->getWritePointer(1)};
        juce::AudioBuffer<float> view(stereo, 2, info.buffer->getNumSamples());
        master.getNextAudioBlock(juce::AudioSourceChannelInfo(&view, info.startSample, info.numSamples));
        performance.renderCue(*info.buffer, 2, info.startSample, info.numSamples);
    }

  private:
    juce::AudioSource& master;
    PerformanceEngine& performance;
};

} // namespace stemcel::perf
