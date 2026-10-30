#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <utility>
#include <vector>

namespace stemcel::plugins
{

struct ScanProgress
{
    int scanned = 0;
    int total = 0;
    juce::String currentPlugin;
};

// One background scan pass over the search paths. Progress and completion are reported on
// the scan thread, like the peaks worker pool; a caller that needs the message thread
// marshals there itself. The destructor joins the thread, so a callback can never outlive
// what it captures.
class PluginScanJob final : private juce::Thread
{
  public:
    using ProgressCallback = std::function<void(ScanProgress)>;
    using FinishedCallback = std::function<void(bool completed)>;

    // Each format with the paths to search for it, scanned one after another.
    using Plan = std::vector<std::pair<juce::AudioPluginFormat*, juce::FileSearchPath>>;

    PluginScanJob(juce::KnownPluginList& listToAddTo, Plan formatsToScan, juce::File deadMansPedalFile,
                  ProgressCallback onProgress, FinishedCallback onFinished);
    ~PluginScanJob() override;

    void start();
    void cancel();
    bool isRunning() const;

  private:
    void run() override;

    juce::KnownPluginList& knownPlugins;
    Plan plan;
    juce::File deadMansPedalFile;
    ProgressCallback progressCallback;
    FinishedCallback finishedCallback;
};

} // namespace stemcel::plugins
