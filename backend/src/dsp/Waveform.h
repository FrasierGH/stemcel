#pragma once

#include <algorithm>

#include <cstdint>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <vector>

namespace stemcel::waveform
{

// Keep in lockstep with renderer `PEAKS_PER_SECOND` so cached peaks splice cleanly.
constexpr int kDefaultPeaksPerSecond = 500;

// Flat channel-major peaks; lane 0 is always the mono summary for legacy compatibility.
struct PeaksResult
{
    std::vector<float> peaks;
    double sampleRate = 0.0;
    int peaksPerSecond = kDefaultPeaksPerSecond;
    int laneCount = 1;

    // How many peaks there are per second of audio: each is a whole number of samples
    // (int(sampleRate / peaksPerSecond)), so 44.1 kHz at 500 a second has 501.14. Reading
    // them at `peaksPerSecond` drew the waveform later and later into a file.
    double bucketsPerSecond() const
    {
        if (sampleRate <= 0.0 || peaksPerSecond <= 0) return static_cast<double>(peaksPerSecond);
        const int samplesPerPeak = std::max(1, static_cast<int>(sampleRate / peaksPerSecond));
        return sampleRate / static_cast<double>(samplesPerPeak);
    }

    int bucketsPerLane() const
    {
        return laneCount > 0 ? static_cast<int>(peaks.size() / (2U * static_cast<std::size_t>(laneCount))) : 0;
    }
};

// Worker-thread decode; disk-bound scans must not freeze the UI.
PeaksResult computePeaks(const juce::File& file, juce::AudioFormatManager& formatManager,
                         int peaksPerSecond = kDefaultPeaksPerSecond);

} // namespace stemcel::waveform
