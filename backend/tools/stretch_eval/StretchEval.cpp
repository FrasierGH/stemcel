// Offline time-stretch comparison harness (dev tool, not shipped, not a CTest test).
//
// Runs one input through every warp mode at a set of tempo ratios, the way the engine
// does (WarpProcessor, 512-sample blocks, real-time options), and writes for each
// mode and ratio:
//   <mode>_<ratio>.wav     the input played at `ratio` times its speed
//   <mode>_<ratio>_rt.wav  that result played back at 1/ratio: the round trip
// and prints how many times faster than real time each stretch ran. Scoring the files
// (round-trip spectral distance to the input, and so on) is left to a script.
//
// Usage: StemcelStretchEval [--experimental] <input.wav> <outputDir> [ratio ...]   (default 0.8 0.9 1.1 1.25)
// --experimental runs the candidate stretches in Experimental.h instead of the engine's modes.

#include "../../src/dsp/WarpProcessor.h"
#include "Experimental.h"

#include <cstdio>
#include <iostream>
#include <memory>

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

namespace
{
constexpr int kBlock = 512;

struct Result
{
    juce::AudioBuffer<float> audio;
    double realtimeFactor = 0.0;
};

Result stretch(const juce::AudioBuffer<float>& in, double sampleRate, const juce::String& mode, double ratio)
{
    const int channels = in.getNumChannels();
    stemcel::WarpProcessor warp(channels, sampleRate, stemcel::warpModeSpec(mode), 1.0, 1024);
    warp.prepareToPlay(kBlock);
    warp.setTempoRatio(ratio);
    warp.seekSource(0);
    const int length = static_cast<int>(stemcel::WarpProcessor::timelineSamplesForSourceSamples(in.getNumSamples(), ratio));
    Result out;
    out.audio.setSize(channels, length);
    out.audio.clear();
    auto read = [&in, channels](float* const* dest, juce::int64 pos, int n)
    {
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < n; ++i)
            {
                const auto at = pos + i;
                dest[c][i] = at >= 0 && at < in.getNumSamples() ? in.getSample(c, static_cast<int>(at)) : 0.0f;
            }
    };
    float* ptrs[stemcel::WarpProcessor::kMaxChannels] = {};
    for (int done = 0; done < length; done += kBlock)
    {
        const int n = juce::jmin(kBlock, length - done);
        for (int c = 0; c < channels; ++c) ptrs[c] = out.audio.getWritePointer(c, done);
        warp.process(ptrs, n, read);
    }
    out.realtimeFactor = warp.getRealtimeFactor();
    return out;
}

void write(const juce::File& file, const juce::AudioBuffer<float>& audio, double sampleRate)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate(sampleRate)
                             .withNumChannels(audio.getNumChannels())
                             .withBitsPerSample(32)
                             .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    std::unique_ptr<juce::AudioFormatWriter> writer(juce::WavAudioFormat().createWriterFor(stream, options));
    if (writer != nullptr) writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples());
}
} // namespace

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const bool experimentalModes = argc > 1 && juce::String(argv[1]) == "--experimental";
    if (experimentalModes)
    {
        --argc;
        ++argv;
    }
    if (argc < 3)
    {
        std::cerr << "usage: StemcelStretchEval <input.wav> <outputDir> [ratio ...]\n";
        return 1;
    }
    const juce::File input{juce::String(argv[1])};
    const juce::File outDir{juce::String(argv[2])};
    std::vector<double> ratios;
    for (int i = 3; i < argc; ++i) ratios.push_back(juce::String(argv[i]).getDoubleValue());
    if (ratios.empty()) ratios = {0.8, 0.9, 1.1, 1.25};
    outDir.createDirectory();

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(input));
    if (reader == nullptr)
    {
        std::cerr << "cannot read " << input.getFullPathName() << "\n";
        return 1;
    }
    juce::AudioBuffer<float> source(static_cast<int>(reader->numChannels), static_cast<int>(reader->lengthInSamples));
    reader->read(&source, 0, source.getNumSamples(), 0, true, true);
    const double sampleRate = reader->sampleRate;

    if (experimentalModes)
    {
        for (const juce::String mode : {"pro-long", "hybrid"})
            for (const double ratio : ratios)
            {
                const auto run = [&](const juce::AudioBuffer<float>& in, double speed)
                {
                    return mode == "hybrid" ? experimental::hybrid(in, sampleRate, speed)
                                            : experimental::signalsmith(in, sampleRate, speed, 0.2, 0.025);
                };
                const auto started = juce::Time::getMillisecondCounterHiRes();
                const auto once = run(source, ratio);
                const double seconds = (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0;
                const auto back = run(once, 1.0 / ratio);
                const auto name = mode + "_" + juce::String(ratio, 2);
                write(outDir.getChildFile(name + ".wav"), once, sampleRate);
                write(outDir.getChildFile(name + "_rt.wav"), back, sampleRate);
                std::printf("%-10s ratio %.2f  %.1fx real time\n", mode.toRawUTF8(), ratio,
                            once.getNumSamples() / sampleRate / juce::jmax(1.0e-6, seconds));
            }
        return 0;
    }
    for (const juce::String mode : {"rhythmic", "tonal", "complex", "pro", "repitch"})
        for (const double ratio : ratios)
        {
            const auto name = mode + "_" + juce::String(ratio, 2);
            const auto once = stretch(source, sampleRate, mode, ratio);
            const auto back = stretch(once.audio, sampleRate, mode, 1.0 / ratio);
            write(outDir.getChildFile(name + ".wav"), once.audio, sampleRate);
            write(outDir.getChildFile(name + "_rt.wav"), back.audio, sampleRate);
            std::printf("%-10s ratio %.2f  %.1fx real time\n", mode.toRawUTF8(), ratio, once.realtimeFactor);
        }
    return 0;
}
