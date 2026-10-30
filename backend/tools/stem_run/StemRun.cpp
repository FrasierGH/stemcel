// Offline run of the built-in stem separator (dev tool, not shipped, not a CTest test).
// Separates one file the way the app's Separate Stems does, so its output can be scored
// with StemcelStemEval against reference stems (e.g. MUSDB18), or against another
// separator's (UVR's models through audio-separator).
//
// Usage:
//   StemcelStemRun [--quality fast|balanced|best] [--htdemucs-only] <modelsRoot> <inputWav> <outputDir>
// <modelsRoot> is the folder the app downloads models into (~/Stemcel/Models): the
// htdemucs-ft files are required, and the vocal and rhythm packs are used when present,
// as the app does, unless --htdemucs-only. Writes vocals.wav, drums.wav, bass.wav and
// other.wav to <outputDir>. STEMCEL_STEM_USE_GPU=1 asks for the GPU provider.

#include "../../src/stems/StemSeparator.h"

#include <cstdio>
#include <iostream>

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

namespace
{
// The commands layer's per-quality resolution (StemSeparationCommands.cpp).
double overlapFor(const juce::String& quality) { return quality == "fast" ? 0.10 : 0.25; }
int shiftsFor(const juce::String& quality) { return quality == "best" ? 4 : 1; }
} // namespace

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::String quality = "best";
    bool htdemucsOnly = false;
    juce::StringArray positional;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String arg(argv[i]);
        if (arg == "--quality" && i + 1 < argc) quality = juce::String(argv[++i]);
        else if (arg == "--htdemucs-only") htdemucsOnly = true;
        else positional.add(arg);
    }
    if (positional.size() != 3)
    {
        std::cerr << "usage: StemcelStemRun [--quality fast|balanced|best] [--htdemucs-only] "
                     "<modelsRoot> <inputWav> <outputDir>\n";
        return 1;
    }
    const juce::File modelsRoot(positional[0]);
    const juce::File input(positional[1]);
    const juce::File outputDir(positional[2]);
    outputDir.createDirectory();

    stemcel::StemSeparationRequest request;
    request.jobId = "stem-run";
    request.sourceName = "est";
    request.sourceFile = input;
    request.modelDir = modelsRoot.getChildFile("htdemucs-ft");
    request.outputDir = outputDir;
    request.stems = {"vocals", "drums", "bass", "other"};
    request.overlap = overlapFor(quality);
    request.shifts = shiftsFor(quality);
    request.useGpu = juce::SystemStats::getEnvironmentVariable("STEMCEL_STEM_USE_GPU", {}) == "1";
    if (!htdemucsOnly)
    {
        // The packs' file names, as the app's model manifests have them.
        const auto vocal = modelsRoot.getChildFile("mel-band-roformer-vocals").getChildFile("syhft_core_folded_fp16_webgpu.onnx");
        const auto rhythm = modelsRoot.getChildFile("bs-roformer-rhythm").getChildFile("bs_roformer_4stem_rhythm_fp16.onnx");
        if (vocal.existsAsFile()) request.roformerModelFile = vocal;
        if (rhythm.existsAsFile()) request.rhythmModelFile = rhythm;
    }
    std::printf("models: htdemucs-ft%s%s quality=%s\n", request.roformerModelFile.existsAsFile() ? " + vocal pack" : "",
                request.rhythmModelFile.existsAsFile() ? " + rhythm pack" : "", quality.toRawUTF8());

    const auto separator = stemcel::createDefaultStemSeparator();
    const auto started = juce::Time::getMillisecondCounterHiRes();
    try
    {
        const auto result = separator->separate(
            request, [](const char*, double, const char*) {}, [](const stemcel::StemResultFile&) {}, [] { return false; });
        for (const auto& stem : result.stems)
        {
            const auto target = outputDir.getChildFile(stem.stem + ".wav");
            if (target != stem.file)
            {
                target.deleteFile();
                stem.file.moveFileTo(target);
            }
            std::printf("wrote %s\n", target.getFullPathName().toRawUTF8());
        }
    }
    catch (const stemcel::StemSeparationError& e)
    {
        std::cerr << "separation failed (" << stemcel::stemFailureCodeToString(e.code) << "): " << e.what() << "\n";
        return 2;
    }
    std::printf("seconds %.1f\n", (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0);
    return 0;
}
