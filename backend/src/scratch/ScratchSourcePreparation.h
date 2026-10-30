#pragma once

#include "WarpModeDefaults.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <functional>
#include <memory>

namespace stemcel
{
class AudioEngine;
}

namespace stemcel::scratch
{

struct SourcePreparationSettings
{
    juce::File sourceFile;
    double inMs = 0.0;
    double durationMs = 0.0;
    bool reversed = false;
    bool warpEnabled = false;
    juce::String warpMode{kDefaultWarpMode};
    double tempoRatio = 1.0;
    double semitones = 0.0;
    double cents = 0.0;
};

struct PreparedSource
{
    std::shared_ptr<const juce::AudioBuffer<float>> audio;
    double sampleRate = 0.0;
    juce::File cacheFile;
};

bool prepareSourceToCache(const SourcePreparationSettings& settings,
                          const juce::File& cacheDirectory,
                          AudioEngine& engine,
                          PreparedSource& result,
                          juce::String& error,
                          const std::function<bool()>& shouldCancel = {},
                          const std::function<void(double)>& reportProgress = {});

} // namespace stemcel::scratch
