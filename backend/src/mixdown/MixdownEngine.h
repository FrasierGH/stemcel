#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <juce_core/juce_core.h>

#include "AutomationShape.h"
#include "EdgeFadeSnapshot.h"
#include "ProjectStateTypes.h"
#include "TempoCurve.h"
#include "BrakeSnapshot.h"
#include "BackspinSnapshot.h"

namespace stemcel
{

class BridgeServer;
class ProjectState;

// Worker renders read an immutable message-thread snapshot, never the live ValueTree.
struct MixdownSnapshot
{
    struct ClipSnapshot
    {
        juce::String id;
        juce::String libraryItemId;
        juce::String filePath;
        double offsetMs{0.0};
        double inMs{0.0};
        double durationMs{0.0};
        double effectiveDurationMs{0.0};
        juce::Array<juce::var> envelopePoints;
        double gainDb{0.0};
        bool edgeFadeIn{false};
        double edgeFadeInStartMs{0.0};
        double edgeFadeInEndMs{0.0};
        EdgeFadeCurve edgeFadeInCurve{EdgeFadeCurve::equalPower};
        bool edgeFadeOut{false};
        double edgeFadeOutStartMs{0.0};
        double edgeFadeOutEndMs{0.0};
        EdgeFadeCurve edgeFadeOutCurve{EdgeFadeCurve::equalPower};
        double brakeSeconds{0.0};
        double brakeCurve{BrakeSnapshot::kDefaultCurvePower};
        double backspinSeconds{0.0};
        double backspinSpeed{BackspinSnapshot::kDefaultSpinSpeed};
        double backspinCurve{BackspinSnapshot::kDefaultCurvePower};
        // Scratch pattern applied to this clip (empty = none).
        juce::String scratchPatternId;
        bool warpEnabled{false};
        juce::String warpMode; // "rhythmic" / "tonal" / "complex" / "pro" / "repitch" (kDefaultWarpMode)
        double tempoRatio{1.0};
        // Following the tempo map (live's OffsetSource::setTempoFollow): the curve, shared by
        // every following clip, and the source's tempo. Null / 0 plays at `tempoRatio`.
        std::shared_ptr<const TempoCurve> tempoCurve;
        double followBpm{0.0};
        double semitones{0.0};
        double cents{0.0};
        int sourceSampleRate{0};
        int sourceChannelCount{0};
    };

    struct TrackSnapshot
    {
        juce::String id;
        float gain{1.0F};
        // Snapshot live per-track parameters so offline render stays in parity with playback.
        float toneBassDb{0.0F};
        float toneMidDb{0.0F};
        float toneTrebleDb{0.0F};
        float toneFilter{0.0F};
        float punchAmount{0.0F};
        float saturationDrive{0.0F};
        float saturationMix{1.0F};
        float bitCrusherRate{1.0F};
        int bitCrusherBits{16};
        float bitCrusherBoost{0.0F};
        float bitCrusherMix{0.0F};
        float reverbSend{0.0F};
        float delaySend{0.0F};
        float pan{0.0F};
        // Per-track effect automation lanes (paramId + breakpoints), for export parity.
        struct AutomationLane
        {
            juce::String paramId;
            std::vector<std::pair<double, float>> points; // (timeMs, value)
            // Each point's segment shape and tension, parallel to `points`.
            std::vector<std::pair<stemcel::AutomationShape, float>> shapes;
        };
        std::vector<AutomationLane> automation;
        std::vector<BeatRepeatRegion> beatRepeats;
        // VST3 inserts, in chain order, with the state the project saved (ADR 0025).
        std::vector<TrackPluginSlot> plugins;
        std::vector<ClipSnapshot> clips;
    };

    int projectSampleRate{44100};
    // Loudness normalization uses a measured pass before final gain, limiting, dither, and
    // encode.
    float masterGain{1.0F};
    bool safetyLimiterEnabled{true};
    float mixGlueAmount{0.0F};
    float mixGlueMakeupDb{0.0F};
    // Offline render uses the same SharedFx settings as playback for export parity.
    float reverbSize{0.0F};
    float reverbDecay{0.0F};
    float reverbTone{0.0F};
    float reverbMix{0.0F};
    juce::String delayNoteValue{"1/8"};
    float delayFeedback{0.0F};
    float delayTone{0.0F};
    float delayMix{0.0F};
    double bpm{120.0};
    std::shared_ptr<const TempoCurve> tempoCurve; // null without a tempo map
    std::vector<TrackSnapshot> tracks;
    // The master bus's inserts (ADR 0025), rendered over the summed mix like playback, and its
    // automation (level, reverb and echo, plugin parameters).
    std::vector<TrackPluginSlot> masterPlugins;
    std::vector<TrackSnapshot::AutomationLane> masterAutomation;
};

// JUCE 8 lacks some tag hooks, so FLAC/AIFF metadata is post-processed after encode.
struct ExportMetadata
{
    juce::String title;
    juce::String artist;
    juce::String album;
    juce::String year;
    juce::String genre;
    juce::String comment;

    bool isEmpty() const noexcept
    {
        return title.isEmpty() && artist.isEmpty() && album.isEmpty()
            && year.isEmpty()  && genre.isEmpty()  && comment.isEmpty();
    }
};

struct MixdownOptions
{
    enum class Format { Wav, Mp3, Flac, Aiff };

    juce::File outputFile;
    int outputSampleRate{44100};
    Format format{Format::Wav};
    int bitDepth{16};
    // Shared TPDF dither keeps 16-bit output identical across render paths.
    bool dither{true};
    double tailSeconds{0.0};
    // Silence written before the first sample, so a player that clips a file's first moment
    // (some do) clips nothing audible. 0..2000 ms.
    double leadInMs{0.0};
    enum class LoudnessMode { Off, AnalyzeOnly, Normalize };
    LoudnessMode loudnessMode{LoudnessMode::Off};
    double targetLufs{-14.0};
    double ceilingDbtp{-1.0};
    int bitrateKbps{192};
    double lengthMs{0.0};
    // Project-time offset to begin rendering from; earlier audio is rendered then
    // discarded so clip positions and FX tails stay correct. 0 = project origin.
    double startMs{0.0};
    ExportMetadata metadata;
};

enum class MixdownFailureCode
{
    Cancelled,
    Io,
    Decode,
    Encode,
    Invalid
};

const char* mixdownFailureCodeToString(MixdownFailureCode code) noexcept;

MixdownSnapshot snapshotProjectForMixdown(
    const ProjectState& project,
    double brakeSeconds = BrakeSnapshot::kPlatterStopSeconds,
    double brakeCurve = BrakeSnapshot::kDefaultCurvePower,
    double backspinSeconds = BackspinSnapshot::kDefaultSpinSeconds,
    double backspinSpeed = BackspinSnapshot::kDefaultSpinSpeed,
    double backspinCurve = BackspinSnapshot::kDefaultCurvePower);

double computeLastClipEndMs(const MixdownSnapshot& snapshot);

// Cancellation is checked at block boundaries and before encoder writes.
void renderMixdownAsync(MixdownSnapshot snapshot,
                        MixdownOptions options,
                        juce::ThreadPool& pool,
                        BridgeServer& bridge,
                        std::atomic<bool>& cancelFlag,
                        std::atomic<bool>& busyFlag);

} // namespace stemcel
