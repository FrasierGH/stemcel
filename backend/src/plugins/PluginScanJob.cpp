#include "PluginScanJob.h"

#include <utility>

namespace stemcel::plugins
{

PluginScanJob::PluginScanJob(juce::KnownPluginList& listToAddTo, Plan formatsToScan, juce::File deadMansPedal,
                             ProgressCallback onProgress, FinishedCallback onFinished)
    : juce::Thread("stemcel-plugin-scan"), knownPlugins(listToAddTo), plan(std::move(formatsToScan)),
      deadMansPedalFile(std::move(deadMansPedal)),
      progressCallback(std::move(onProgress)), finishedCallback(std::move(onFinished))
{
}

PluginScanJob::~PluginScanJob()
{
    stopThread(5000);
}

void PluginScanJob::start()
{
    startThread();
}

void PluginScanJob::cancel()
{
    signalThreadShouldExit();
}

bool PluginScanJob::isRunning() const
{
    return isThreadRunning();
}

void PluginScanJob::run()
{
    // Walking the paths here gives the same file list each scanner builds for itself, plus a
    // total across the formats to report progress against.
    std::vector<juce::StringArray> files;
    int total = 0;
    for (const auto& [format, paths] : plan)
    {
        files.push_back(format->searchPathsForPlugins(paths, /*recursive*/ true, false));
        total += files.back().size();
    }
    int scanned = 0;

    for (size_t f = 0; f < plan.size(); ++f)
    {
        auto& format = *plan[f].first;
        juce::PluginDirectoryScanner scanner{knownPlugins, format, plan[f].second, /*searchRecursively*/ true,
                                             deadMansPedalFile,
                                             /*allowPluginsWhichRequireAsynchronousInstantiation*/ false};
        scanner.setFilesOrIdentifiersToScan(files[f]);

        for (bool more = files[f].size() > 0; more;)
        {
            if (threadShouldExit())
            {
                if (finishedCallback) finishedCallback(false);
                return;
            }

            juce::String pluginName;
            more = scanner.scanNextFile(/*dontRescanIfAlreadyInList*/ true, pluginName);
            ++scanned;

            if (progressCallback) progressCallback({scanned, total, pluginName});
        }

        // A file that yielded no types either crashed its scan child or is not a usable plugin.
        // Blacklisting it keeps later scans fast and stops it being retried on every launch.
        for (const auto& failed : scanner.getFailedFiles())
            knownPlugins.addToBlacklist(failed);
    }

    if (finishedCallback) finishedCallback(true);
}

} // namespace stemcel::plugins
