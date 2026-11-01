#pragma once

#include "BackspinSnapshot.h"
#include "BrakeSnapshot.h"
#include "EdgeFadeSnapshot.h"
#include "EnvelopeSnapshot.h"
#include "OffsetSource.h"
#include "PreviewMetronomeSource.h"
#include "WarpModeDefaults.h"
#include "WarpProcessor.h"
#include "scratch/ScratchPatternEvaluator.h"

#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

namespace stemcel
{

// Timeline-clip and preview source ownership, including read-ahead state.
class AudioEnginePlaybackState
{
protected:
    struct Track
    {
        juce::String trackId;
        std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
        std::unique_ptr<OffsetSource> offsetSource;
        std::unique_ptr<juce::BufferingAudioSource> bufferingSource;
        std::unique_ptr<juce::AudioTransportSource> transportSource;
        std::unique_ptr<WarpProcessor> warp;
        juce::String warpMode;
        std::vector<std::unique_ptr<WarpProcessor>> retiredWarps;
        std::unique_ptr<EnvelopeSnapshot> envelopeSnapshot;
        std::vector<std::unique_ptr<EnvelopeSnapshot>> retiredEnvelopes;
        std::unique_ptr<EdgeFadeSnapshot> edgeFadeSnapshot;
        std::vector<std::unique_ptr<EdgeFadeSnapshot>> retiredEdgeFades;
        std::unique_ptr<BrakeSnapshot> brakeSnapshot;
        std::vector<std::unique_ptr<BrakeSnapshot>> retiredBrakes;
        std::unique_ptr<BackspinSnapshot> backspinSnapshot;
        std::vector<std::unique_ptr<BackspinSnapshot>> retiredBackspins;
        std::shared_ptr<const scratch::PatternReplaySnapshot> patternSnapshot;
        double sampleRate = 44100.0;
        int numChannels = 2;
        bool prefetchDirty = false;
        // The source tempo when the clip follows the tempo map; 0 when it does not.
        double followBpm = 0.0;
        // The source's own tempo when it drifts (owned by the engine's curve list); null when steady.
        const TempoCurve* sourceCurve = nullptr;
    };

    struct Preview
    {
        std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
        std::unique_ptr<OffsetSource> offsetSource;
        std::unique_ptr<juce::AudioTransportSource> transportSource;
        std::unique_ptr<WarpProcessor> warp;
        std::vector<std::unique_ptr<WarpProcessor>> retiredWarps;
        std::unique_ptr<EnvelopeSnapshot> envelopeSnapshot;
        std::vector<std::unique_ptr<EnvelopeSnapshot>> retiredEnvelopes;
        std::unique_ptr<BrakeSnapshot> brakeSnapshot;
        std::vector<std::unique_ptr<BrakeSnapshot>> retiredBrakes;
        std::unique_ptr<BackspinSnapshot> backspinSnapshot;
        std::vector<std::unique_ptr<BackspinSnapshot>> retiredBackspins;
        juce::String warpMode{kDefaultWarpMode};
        double sampleRate = 44100.0;
        double inMs = 0.0;
        double durationMs = 0.0;
        double sourceDurationMs = 0.0;
        juce::File sourceFile;
    };

    // Every tempo curve ever published: a clip's read-ahead may still hold the previous one
    // when a new one is published, and curves are tiny, so none is freed while the engine runs.
    std::vector<std::unique_ptr<TempoCurve>> tempoCurves;
    const TempoCurve* tempoCurve = nullptr; // null: no tempo map
    // The project's constant tempo as a curve: what a clip with a drifting source follows
    // when there is no tempo map.
    const TempoCurve* flatCurve = nullptr;

    juce::TimeSliceThread readAheadThread{"stemcel-readahead"};
    std::unordered_map<juce::String, std::unique_ptr<Track>> tracks;
    Preview preview;
    std::unique_ptr<PreviewMetronomeSource> previewMetronomeSource;
    bool previewMetronomeEnabled = false;
    double previewMetronomeBpm = 0.0;
    double previewMetronomeAnchorSec = 0.0;
    std::atomic<juce::int64> previewGeneration{0};

    // Running total already reported by `logWarpShortfalls`, so each stop reports only
    // what this run added rather than repeating the session's history.
    juce::uint32 reportedWarpShortfalls{0};
};

} // namespace stemcel
