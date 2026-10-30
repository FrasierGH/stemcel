#include "WarpProcessor.h"

#include <samplerate.h>
#include <signalsmith-stretch/signalsmith-stretch.h>

#include <algorithm>

namespace stemcel
{

using RubberBand::RubberBandStretcher;

// Rubber Band publishes no mask constant, so derive one from the flags themselves.
namespace
{
constexpr auto kTransientsMask = RubberBandStretcher::OptionTransientsCrisp
                                 | RubberBandStretcher::OptionTransientsMixed
                                 | RubberBandStretcher::OptionTransientsSmooth;
// The fastest a clip is ever played relative to its source (setTempoRatio's clamp).
constexpr double kMaxRate = 4.0;
} // namespace

// Signalsmith Stretch for "pro" (and for repitch with a pitch shift on top), and a
// libsamplerate varispeed for repitch. Owned by the processor; touched only on the
// thread that calls `process`, apart from construction.
struct WarpProcessor::AltEngines
{
    signalsmith::stretch::SignalsmithStretch<float> stretch;
    SRC_STATE* resampler = nullptr;
    Path path = Path::signalsmith;
    bool needsReset = true;
    std::vector<std::vector<float>> in, seekIn;
    std::vector<float*> inPtrs, seekPtrs;
    std::vector<float> interleavedIn, interleavedOut;
    // The source reader of the `process` call under way, for the resampler's callback.
    const std::function<void(float* const*, juce::int64, int)>* read = nullptr;

    AltEngines() = default;
    AltEngines(const AltEngines&) = delete;
    AltEngines& operator=(const AltEngines&) = delete;
    ~AltEngines()
    {
        if (resampler != nullptr) src_delete(resampler);
    }

    // libsamplerate pulls its input through this callback, a feed-sized chunk at a time.
    static long pullInput(void* data, float** samples)
    {
        auto& self = *static_cast<WarpProcessor*>(data);
        auto& a = *self.alt;
        const int n = self.processFeedSamples;
        if (a.read == nullptr)
        {
            *samples = nullptr;
            return 0;
        }
        (*a.read)(a.inPtrs.data(), self.nextSourceSample, n);
        self.nextSourceSample += n;
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < self.numChannels; ++c)
                a.interleavedIn[static_cast<size_t>(i * self.numChannels + c)] =
                    a.in[static_cast<size_t>(c)][static_cast<size_t>(i)];
        *samples = a.interleavedIn.data();
        return n;
    }
};

WarpProcessor::WarpProcessor(int numChannelsArg, double sampleRateArg,
                             RubberBandStretcher::Options modeOptions,
                             double initialPitchScale,
                             int maxProcessSamples)
    : WarpProcessor(numChannelsArg, sampleRateArg, WarpModeSpec{WarpEngine::rubberBand, modeOptions},
                    initialPitchScale, maxProcessSamples)
{
}

WarpProcessor::WarpProcessor(int numChannelsArg, double sampleRateArg, const WarpModeSpec& spec,
                             double initialPitchScale, int maxProcessSamples)
    : numChannels(juce::jlimit(1, kMaxChannels, numChannelsArg)),
      sampleRate(sampleRateArg),
      processFeedSamples(juce::jlimit(64, 65536, maxProcessSamples)),
      engine(spec.engine)
{
    const double clampedPitchScale = juce::jlimit(0.25, 4.0, initialPitchScale);
    if (engine != WarpEngine::rubberBand)
    {
        pendingPitchScale.store(clampedPitchScale, std::memory_order_relaxed);
        alt = std::make_unique<AltEngines>();
        alt->stretch.presetDefault(numChannels, static_cast<float>(sampleRate));
        const auto feed = static_cast<size_t>(std::ceil(processFeedSamples * kMaxRate)) + 16;
        const auto seekLength =
            static_cast<size_t>(alt->stretch.outputSeekLength(static_cast<float>(kMaxRate))) + 16;
        alt->in.assign(static_cast<size_t>(numChannels), std::vector<float>(feed));
        alt->seekIn.assign(static_cast<size_t>(numChannels), std::vector<float>(seekLength));
        for (int c = 0; c < numChannels; ++c)
        {
            alt->inPtrs.push_back(alt->in[static_cast<size_t>(c)].data());
            alt->seekPtrs.push_back(alt->seekIn[static_cast<size_t>(c)].data());
        }
        alt->interleavedIn.resize(static_cast<size_t>(processFeedSamples * numChannels));
        alt->interleavedOut.resize(static_cast<size_t>(processFeedSamples * numChannels));
        if (engine == WarpEngine::repitch)
        {
            int error = 0;
            // The best of libsamplerate's sinc converters: flat to 96% of Nyquist with
            // 97 dB of stop-band rejection, so a turntable-style speed change adds nothing
            // audible of its own.
            alt->resampler =
                src_callback_new(&AltEngines::pullInput, SRC_SINC_BEST_QUALITY, numChannels, &error, this);
        }
        alt->path = pathFor(clampedPitchScale);
        outputScratchPtrs.resize(static_cast<size_t>(numChannels));
        return;
    }
    const auto modeOptions = spec.rubberBandOptions;
    const auto options = realtimeOptionsFor(modeOptions, clampedPitchScale);
    canChangePitchOption =
        (modeOptions & RubberBandStretcher::OptionEngineFiner) == 0;
    highConsistencyPitch = !canChangePitchOption ||
        (options & RubberBandStretcher::OptionPitchHighConsistency) != 0;
    // Whether this mode is subject to the pitch-dependent transients choice, and where that
    // choice currently stands. Pitch can change after construction without a rebuild, so the
    // choice has to be revisited live rather than fixed here.
    crispTransientsMode = canChangePitchOption
                       && (modeOptions & kTransientsMask)
                              == RubberBandStretcher::OptionTransientsCrisp;
    transientsMixedForPitch =
        (options & kTransientsMask) == RubberBandStretcher::OptionTransientsMixed;
    pendingPitchScale.store(clampedPitchScale, std::memory_order_relaxed);
    appliedPitchScale = clampedPitchScale;
    stretcher = std::make_unique<RubberBandStretcher>(static_cast<size_t>(sampleRate),
                                                       static_cast<size_t>(numChannels),
                                                       options,
                                                       1.0, // initial time ratio (output / input)
                                                       clampedPitchScale
    );
    stretcher->setMaxProcessSize(static_cast<size_t>(processFeedSamples));
    outputScratchPtrs.resize(static_cast<size_t>(numChannels));
    doReset();
}

WarpProcessor::~WarpProcessor() = default;

RubberBandStretcher::Options WarpProcessor::realtimeOptionsFor(
    RubberBandStretcher::Options modeOptions, double initialPitchScale) noexcept
{
    auto options = modeOptions | RubberBandStretcher::OptionProcessRealTime;
    const bool finerEngine =
        (modeOptions & RubberBandStretcher::OptionEngineFiner) != 0;
    const bool pitchShifting = std::abs(initialPitchScale - 1.0) > 1.0e-4;

    // Rubber Band publishes no mask constant, so derive one from the flags themselves.
    constexpr auto transientsMask = kTransientsMask;

    // R2's crisp transient handling resets component phases at every detected
    // onset, which Rubber Band warns "may cause interruptions in stable sounds".
    // While resampling for a pitch shift it fires on sustained tones too, smearing
    // them and detuning the result by up to a quarter of a semitone. Mixed resets
    // phases only outside the range of musical fundamentals, so it keeps crisp's
    // attack definition on percussive material while leaving tones intact — the
    // right trade when the clip could be a vocal, a guitar, a synth or a drum take.
    // Only the crisp default is overridden: "tonal" and "complex" pick their own.
    if (pitchShifting && !finerEngine && (options & transientsMask) == RubberBandStretcher::OptionTransientsCrisp)
    {
        options |= RubberBandStretcher::OptionTransientsMixed;
    }

    if (finerEngine)
    {
        // R3 can only set a pitch option on construction, so it keeps the option
        // that tolerates a later live pitch drag.
        options |= RubberBandStretcher::OptionPitchHighConsistency;
    }
    else if (pitchShifting)
    {
        // A shift supplied at construction is a fixed one, which is what
        // HighQuality is for. `applyPendingParams` escalates to HighConsistency
        // if the pitch is later changed live.
        options |= RubberBandStretcher::OptionPitchHighQuality;
    }
    return options;
}

void WarpProcessor::prepareToPlay(int maxBlockSamples)
{
    juce::ignoreUnused(maxBlockSamples);
    if (allocatedBlockSamples == processFeedSamples) return;
    allocatedBlockSamples = processFeedSamples;
    sourceScratch.assign(static_cast<size_t>(numChannels),
                         std::vector<float>(static_cast<size_t>(processFeedSamples)));
    discardScratch.assign(static_cast<size_t>(numChannels),
                          std::vector<float>(static_cast<size_t>(processFeedSamples)));
    sourceScratchPtrs.resize(static_cast<size_t>(numChannels));
    discardScratchPtrs.resize(static_cast<size_t>(numChannels));
    for (int c = 0; c < numChannels; ++c)
    {
        sourceScratchPtrs[c] = sourceScratch[c].data();
        discardScratchPtrs[c] = discardScratch[c].data();
    }
    doReset();
}

void WarpProcessor::doReset()
{
    if (alt != nullptr) alt->needsReset = true;
    if (stretcher == nullptr) return;
    (*stretcher).reset();
    // Feed Rubber Band's start pad and discard its priming delay after reset/seek.
    const int pad = static_cast<int>(stretcher->getPreferredStartPad());
    outputDelayToDiscard = static_cast<int>(stretcher->getStartDelay());
    if (pad > 0 && allocatedBlockSamples > 0)
    {
        int remaining = pad;
        while (remaining > 0)
        {
            const int chunk = std::min({remaining, allocatedBlockSamples,
                                        processFeedSamples});
            for (int c = 0; c < numChannels; ++c)
            {
                std::fill(sourceScratch[c].begin(), sourceScratch[c].begin() + chunk, 0.0f);
            }
            stretcher->process(sourceScratchPtrs.data(), static_cast<size_t>(chunk), false);
            remaining -= chunk;
        }
    }
}

void WarpProcessor::updateTransientsForPitch(double pitchScale) noexcept
{
    // `realtimeOptionsFor` makes this same choice from the pitch supplied at construction,
    // which is all the offline render ever needs. Live, warp is often enabled before any
    // pitch is dialled in and the stretcher is deliberately not rebuilt for a pitch change,
    // so without this the crisp default would survive into a shift it is unsuited to —
    // smearing sustained tones and detuning them, but only during playback.
    if (!crispTransientsMode) return;
    const bool wantMixed = std::abs(pitchScale - 1.0) > 1.0e-4;
    if (wantMixed == transientsMixedForPitch) return;
    stretcher->setTransientsOption(wantMixed ? RubberBandStretcher::OptionTransientsMixed
                                             : RubberBandStretcher::OptionTransientsCrisp);
    transientsMixedForPitch = wantMixed;
}

void WarpProcessor::applyPendingParams() noexcept
{
    if (stretcher == nullptr) return;
    const double tr = pendingTempoRatio.load(std::memory_order_acquire);
    if (tr != appliedTempoRatio)
    {
        // Rubber Band wants output/input, the inverse of Stemcel's project/source ratio.
        stretcher->setTimeRatio(1.0 / tr);
        appliedTempoRatio = tr;
    }
    const double ps = pendingPitchScale.load(std::memory_order_acquire);
    if (ps != appliedPitchScale)
    {
        updateTransientsForPitch(ps);
        if (canChangePitchOption && !highConsistencyPitch)
        {
            stretcher->setPitchOption(RubberBandStretcher::OptionPitchHighConsistency);
            highConsistencyPitch = true;
        }
        stretcher->setPitchScale(ps);
        appliedPitchScale = ps;
    }
}

bool WarpProcessor::isActive() const noexcept
{
    const double tr = pendingTempoRatio.load(std::memory_order_acquire);
    const double ps = pendingPitchScale.load(std::memory_order_acquire);
    return std::abs(tr - 1.0) > 1e-4 || std::abs(ps - 1.0) > 1e-4;
}

int WarpProcessor::process(float* const* output, int numOutputSamples,
                           const std::function<void(float* const*, juce::int64, int)>& readSource)
{
    if (alt != nullptr) return processAlt(output, numOutputSamples, readSource);
    if (stretcher == nullptr || numOutputSamples <= 0 || allocatedBlockSamples == 0)
    {
        for (int c = 0; c < numChannels; ++c)
        {
            std::fill(output[c], output[c] + numOutputSamples, 0.0f);
        }
        return 0;
    }

    applyPendingParams();

    // Wall-clock cost of the stretch, measured against the audio it yields. Cheap and
    // allocation-free; `process` runs on the read-ahead or render thread, never the
    // device callback.
    const auto processStartTicks = juce::Time::getHighResolutionTicks();

    const bool wantsSeek = seekPending.exchange(false, std::memory_order_acq_rel);
    const bool wantsReset = resetPending.exchange(false, std::memory_order_acq_rel);
    if (wantsSeek)
    {
        nextSourceSample = pendingSourceSeek.load(std::memory_order_acquire);
        logicalSourceSample = static_cast<double>(nextSourceSample);
        doReset();
    }
    else if (wantsReset)
    {
        doReset();
    }

    int produced = 0;
    const int feedChunk = std::min(allocatedBlockSamples, processFeedSamples);
    const auto expectedInput = static_cast<int64_t>(
        std::ceil(static_cast<double>(numOutputSamples) * appliedTempoRatio));
    const auto primingInput = static_cast<int64_t>(
        std::ceil(static_cast<double>(outputDelayToDiscard) * appliedTempoRatio));
    const auto maxInputToFeed =
        expectedInput + primingInput + static_cast<int64_t>(feedChunk) * 2;
    int64_t inputFed = 0;
    int64_t iterations = 0;
    const int64_t maxIterations =
        static_cast<int64_t>(numOutputSamples) + maxInputToFeed
        + outputDelayToDiscard + 8;
    while (produced < numOutputSamples && iterations++ < maxIterations)
    {
        const int available = stretcher->available();
        if (available > 0 && outputDelayToDiscard > 0)
        {
            const int drop =
                std::min({available, outputDelayToDiscard, processFeedSamples});
            stretcher->retrieve(discardScratchPtrs.data(), static_cast<size_t>(drop));
            outputDelayToDiscard -= drop;
            continue;
        }
        if (available > 0)
        {
            const int want = std::min(available, numOutputSamples - produced);
            for (int c = 0; c < numChannels; ++c) outputScratchPtrs[c] = output[c] + produced;
            const size_t got = stretcher->retrieve(outputScratchPtrs.data(), static_cast<size_t>(want));
            produced += static_cast<int>(got);
            continue;
        }

        const auto required = static_cast<int64_t>(stretcher->getSamplesRequired());
        const auto remainingInput = maxInputToFeed - inputFed;
        if (required <= 0 || remainingInput <= 0) break;
        const int sourceSamples =
            static_cast<int>(std::min({required, remainingInput,
                                       static_cast<int64_t>(feedChunk)}));
        readSource(sourceScratchPtrs.data(), nextSourceSample, sourceSamples);
        stretcher->process(sourceScratchPtrs.data(), static_cast<size_t>(sourceSamples), false);
        nextSourceSample += sourceSamples;
        inputFed += sourceSamples;
    }

    logicalSourceSample += static_cast<double>(numOutputSamples) * appliedTempoRatio;

    // Silence-fill rare priming shortfalls to keep the output contract stable.
    if (produced < numOutputSamples)
    {
        shortfallCount.fetch_add(1, std::memory_order_relaxed);
        for (int c = 0; c < numChannels; ++c)
        {
            std::fill(output[c] + produced, output[c] + numOutputSamples, 0.0f);
        }
        // Do not carry delayed output across the inserted silence. Resume from the
        // audible timeline cursor rather than Rubber Band's read-ahead frontier.
        nextSourceSample = static_cast<juce::int64>(std::llround(logicalSourceSample));
        doReset();
    }

    processTicks.fetch_add(juce::Time::getHighResolutionTicks() - processStartTicks,
                           std::memory_order_relaxed);
    processedOutputSamples.fetch_add(numOutputSamples, std::memory_order_relaxed);
    return produced;
}

WarpProcessor::Path WarpProcessor::pathFor(double pitchScale) const noexcept
{
    if (engine == WarpEngine::rubberBand) return Path::rubberBand;
    if (engine == WarpEngine::repitch && std::abs(pitchScale - 1.0) <= 1.0e-4) return Path::resample;
    return Path::signalsmith;
}

void WarpProcessor::applyAltParams() noexcept
{
    const double tr = pendingTempoRatio.load(std::memory_order_acquire);
    const double ps = pendingPitchScale.load(std::memory_order_acquire);
    const auto path = pathFor(ps);
    if (path != alt->path)
    {
        // A pitch shift on a repitch clip moves it between the resampler and the stretcher;
        // the new one starts from where the audible output had got to.
        alt->path = path;
        nextSourceSample = static_cast<juce::int64>(std::llround(logicalSourceSample));
        alt->needsReset = true;
    }
    if (path == Path::signalsmith && (tr != appliedTempoRatio || ps != appliedPitchScale || alt->needsReset))
        // Repitch keeps the turntable's pitch change and adds the shift on top of it.
        alt->stretch.setTransposeFactor(static_cast<float>(engine == WarpEngine::repitch ? tr * ps : ps));
    appliedTempoRatio = tr;
    appliedPitchScale = ps;
}

void WarpProcessor::resetAlt(const std::function<void(float* const*, juce::int64, int)>& readSource)
{
    alt->needsReset = false;
    if (alt->path == Path::resample)
    {
        src_reset(alt->resampler);
        return;
    }
    // Pre-roll from the source so the next output sample is the one at `nextSourceSample`.
    const auto rate = static_cast<float>(appliedTempoRatio);
    const int length =
        juce::jmin(alt->stretch.outputSeekLength(rate), static_cast<int>(alt->seekIn[0].size()));
    readSource(alt->seekPtrs.data(), nextSourceSample, length);
    alt->stretch.outputSeek(alt->seekPtrs.data(), length);
    nextSourceSample += length;
}

int WarpProcessor::processAlt(float* const* output, int numOutputSamples,
                              const std::function<void(float* const*, juce::int64, int)>& readSource)
{
    if (numOutputSamples <= 0) return 0;
    const auto processStartTicks = juce::Time::getHighResolutionTicks();
    if (seekPending.exchange(false, std::memory_order_acq_rel))
    {
        nextSourceSample = pendingSourceSeek.load(std::memory_order_acquire);
        logicalSourceSample = static_cast<double>(nextSourceSample);
        alt->needsReset = true;
    }
    else if (resetPending.exchange(false, std::memory_order_acq_rel))
    {
        nextSourceSample = static_cast<juce::int64>(std::llround(logicalSourceSample));
        alt->needsReset = true;
    }
    applyAltParams();
    alt->read = &readSource;
    if (alt->needsReset) resetAlt(readSource);

    int produced = 0;
    double audible = logicalSourceSample; // the source position the output has reached
    while (produced < numOutputSamples)
    {
        const int n = juce::jmin(processFeedSamples, numOutputSamples - produced);
        if (alt->path == Path::resample)
        {
            // libsamplerate's ratio is output over input, the inverse of the tempo ratio.
            // Set as a step, not left to its in-block ramp: a ramp consumes the average of
            // the old and new ratios, which along a tempo map lags the timeline by up to half
            // a block's worth of every change. The steps (a block apart) are inaudibly small.
            src_set_ratio(alt->resampler, 1.0 / appliedTempoRatio);
            const long got =
                src_callback_read(alt->resampler, 1.0 / appliedTempoRatio, n, alt->interleavedOut.data());
            if (got <= 0) break;
            for (int i = 0; i < static_cast<int>(got); ++i)
                for (int c = 0; c < numChannels; ++c)
                    output[c][produced + i] = alt->interleavedOut[static_cast<size_t>(i * numChannels + c)];
            produced += static_cast<int>(got);
            continue;
        }
        // Feed what keeps the stretcher's read-ahead at its size for this rate: its input
        // latency plus the rate times its output latency (what outputSeek primes). Feeding
        // n * rate alone keeps the read-ahead primed for the rate at the last seek, so a
        // tempo change would leave the audio late by the difference; measured from absolute
        // positions, any rounding or rate change corrects itself on the next block.
        audible += n * appliedTempoRatio;
        const double lead = alt->stretch.inputLatency() + appliedTempoRatio * alt->stretch.outputLatency();
        const double wanted = audible + lead - static_cast<double>(nextSourceSample);
        const int inputSamples = juce::jlimit(0, static_cast<int>(alt->in[0].size()), static_cast<int>(wanted));
        readSource(alt->inPtrs.data(), nextSourceSample, inputSamples);
        nextSourceSample += inputSamples;
        for (int c = 0; c < numChannels; ++c) outputScratchPtrs[static_cast<size_t>(c)] = output[c] + produced;
        alt->stretch.process(alt->inPtrs.data(), inputSamples, outputScratchPtrs.data(), n);
        produced += n;
    }
    alt->read = nullptr;
    logicalSourceSample += static_cast<double>(numOutputSamples) * appliedTempoRatio;

    if (produced < numOutputSamples)
    {
        shortfallCount.fetch_add(1, std::memory_order_relaxed);
        for (int c = 0; c < numChannels; ++c) std::fill(output[c] + produced, output[c] + numOutputSamples, 0.0f);
        nextSourceSample = static_cast<juce::int64>(std::llround(logicalSourceSample));
        alt->needsReset = true;
    }
    processTicks.fetch_add(juce::Time::getHighResolutionTicks() - processStartTicks, std::memory_order_relaxed);
    processedOutputSamples.fetch_add(numOutputSamples, std::memory_order_relaxed);
    return produced;
}

} // namespace stemcel
