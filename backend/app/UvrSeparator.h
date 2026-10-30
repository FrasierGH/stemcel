#pragma once

// UVR's models in Stemcel, through python-audio-separator (MIT): the separation engine
// behind Ultimate Vocal Remover, as a command-line tool that downloads and runs the
// community's models (BS/Mel-RoFormer, MDX23C, MDX-Net, Demucs, VR) and its ensembles.
// Stemcel does not ship it or the models: the user installs it once, and Stemcel runs
// it on a background thread and imports what it writes as ordinary stems.

#include "AppPreferences.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace stemcel::native::uvr
{

// A curated way to use the models, from deton24's separation guide: one model or one of
// audio-separator's ensemble presets, and the stems it produces.
struct Recipe
{
    juce::String id;
    juce::String label;
    juce::String help;
    juce::String model;  // a model filename, or
    juce::String preset; // an ensemble preset id
    juce::StringArray stems; // what comes out, when known (for the stem picker)
    // The vocals come from Stemcel's own vocal model instead (it scored higher), when installed.
    bool builtinVocals = false;
};

const std::vector<Recipe>& recipes();
const Recipe* findRecipe(const juce::String& id);

// The `audio-separator` executable: the one chosen in Stemcel, else PATH, else where pip
// and pipx put scripts. Non-existent when it is not installed.
juce::File findExecutable(AppPreferences& prefs);
// Where Stemcel keeps the UVR models (they persist between runs, unlike the tool's /tmp default).
juce::File modelDir();
// The install command shown when the tool is missing.
juce::String installCommand();

// Whether the tool runs its models on a GPU (CUDA or Apple's MPS) or the CPU, as its own
// log says ("CUDA is available in Torch...", "...running in CPU mode").
enum class Acceleration
{
    unknown,
    gpu,
    cpu
};
Acceleration accelerationFrom(const juce::String& toolOutput);
// Asks the tool (--env_info, a few seconds, off the message thread), once per executable;
// `done` runs on the message thread.
void checkAcceleration(const juce::File& executable, std::function<void(Acceleration)> done);
// pip's default PyTorch on Windows is CPU-only: this puts the CUDA build into the tool's pipx
// environment. Empty where it does not apply (no NVIDIA GPU, or macOS, where MPS needs nothing).
juce::String gpuInstallCommand();
// Where each run's full output goes, for when something goes wrong.
juce::File lastRunLog();

// "song_(Vocals)_model.wav" -> "vocals"; empty when the name carries no stem.
juce::String stemFromFileName(const juce::String& fileName);
// The display name for a stem key ("hh" -> "Hi-hat", "noreverb" -> "Dry").
juce::String stemLabel(const juce::String& key);
// The last progress percentage in a chunk of the tool's output, or -1.
double progressFrom(const juce::String& text);

// One run of the tool. Reports on the message thread; deleting it stops the run.
class Job final : private juce::Thread
{
  public:
    Job(juce::File executable, juce::File input, juce::File outputDir, Recipe recipe);
    ~Job() override;

    void start() { startThread(); }
    // Stops the tool at once (its output is read on a blocking pipe, so a flag alone would
    // wait for its next line).
    void cancel();

    // percent 0..100: of the download while one runs (stage "download"), else of the whole
    // separation, across an ensemble's models (stage "separate (GPU) 1/2" and the like).
    std::function<void(double percent, const juce::String& stage)> onProgress;
    // stem key -> file, or the reason it failed.
    std::function<void(bool ok, const juce::String& error, std::map<juce::String, juce::File> stems)> onDone;

  private:
    void run() override;
    void finish(bool ok, const juce::String& error, std::map<juce::String, juce::File> stems);
    // One line (or tqdm carriage-return update) of the tool's output.
    void readLine(const juce::String& line);

    juce::ChildProcess process;
    juce::CriticalSection processLock;
    int models = 1, model = 0; // an ensemble runs its models one after another
    Acceleration acceleration = Acceleration::unknown;
    bool downloading = false;

    juce::File executable, input, outputDir;
    Recipe recipe;
    std::atomic<bool> cancelled{false};
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
};

} // namespace stemcel::native::uvr
