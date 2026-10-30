#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace stemcel
{

juce::AudioBuffer<float> denormaliseStemMixture(
    const juce::AudioBuffer<float>& mixture, int channelCount, float mean, float standardDeviation);

bool shouldBuildRawStemMixture(
    bool haveVocalPack, bool vocalsSelected, bool useRhythmPack) noexcept;

} // namespace stemcel
