#pragma once

// The device's output: the master (the whole engine, with its gain, limiter and meters) on
// outputs 1–2, and the performance mode's headphone cue on 3–4 when the device has them — as
// the DDJ-400's sound card expects (master 1/2, headphones 3/4).

#include "CueOutput.h"
#include "PerformanceEngine.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>

namespace stemcel::perf
{

class OutputRouter final : public juce::AudioSource
{
  public:
    OutputRouter(juce::AudioSource& masterSource, PerformanceEngine& perf, CueOutput& cue)
        : master(masterSource), performance(perf), cueOutput(cue)
    {
    }

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override
    {
        master.prepareToPlay(samplesPerBlockExpected, sampleRate);
        cueScratch.setSize(2, std::max(4096, samplesPerBlockExpected * 4));
    }
    void releaseResources() override { master.releaseResources(); }

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        if (info.buffer == nullptr) return;
        const int channels = info.buffer->getNumChannels();
        if (channels <= 2)
        {
            master.getNextAudioBlock(info);
            feedCueDevice(info);
            return;
        }
        // The master sees a stereo buffer; the rest carry the cue.
        float* stereo[2] = {info.buffer->getWritePointer(0), info.buffer->getWritePointer(1)};
        juce::AudioBuffer<float> view(stereo, 2, info.buffer->getNumSamples());
        master.getNextAudioBlock(juce::AudioSourceChannelInfo(&view, info.startSample, info.numSamples));
        performance.renderCue(*info.buffer, 2, info.startSample, info.numSamples);
        feedCueDevice(info);
    }

  private:
    // The controller's sound card as the headphone output: the master and the cue go to it.
    void feedCueDevice(const juce::AudioSourceChannelInfo& info)
    {
        if (!cueOutput.isOpen() || info.numSamples > cueScratch.getNumSamples()) return;
        performance.renderCue(cueScratch, 0, 0, info.numSamples);
        const int last = info.buffer->getNumChannels() - 1;
        cueOutput.push(info.buffer->getReadPointer(0, info.startSample), info.buffer->getReadPointer(std::min(1, last), info.startSample),
                       cueScratch.getReadPointer(0), cueScratch.getReadPointer(1), info.numSamples);
    }

    juce::AudioSource& master;
    PerformanceEngine& performance;
    CueOutput& cueOutput;
    juce::AudioBuffer<float> cueScratch;
};

} // namespace stemcel::perf
