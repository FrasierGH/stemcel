#include "PluginCatalogue.h"

#include "AppPaths.h"
#include "Log.h"
#include "PluginFormats.h"
#include "PluginScanCoordinator.h"

#include <utility>

namespace stemcel::plugins
{
namespace
{
juce::File resolveDataDir(const juce::File& requested)
{
    if (requested != juce::File{}) return requested;

    const auto appData = userAppDataDirectory();
    return appData.getChildFile(appFolderName()).getChildFile("plugins");
}
} // namespace

PluginCatalogue::PluginCatalogue(const juce::File& dataDirectory) : dataDir(resolveDataDir(dataDirectory))
{
    if (const auto created = dataDir.createDirectory(); !created.wasOk())
    {
        log::error("plugins", "failed to create plugin data dir " + dataDir.getFullPathName() + ": "
                                  + created.getErrorMessage());
    }

    addHostedFormats(formatManager);
    knownPlugins.setCustomScanner(std::make_unique<PluginScanCoordinator>());

    if (const auto cached = juce::parseXML(knownPluginsFile()); cached != nullptr)
        knownPlugins.recreateFromXml(*cached);

    // Reconcile at load as well as after a scan. This is only an existence check per cached
    // entry, not a scan — nothing is loaded or instantiated — so it costs nothing measurable
    // at startup, and it means a plugin uninstalled since the last run is gone from the picker
    // immediately rather than lingering until the user happens to rescan.
    if (removeUninstalledPlugins() > 0) saveKnownPlugins();

    log::info("plugins", "catalogue loaded with " + juce::String(knownPlugins.getNumTypes()) + " plugin(s), "
                             + juce::String(knownPlugins.getBlacklistedFiles().size()) + " blacklisted");
}

PluginCatalogue::~PluginCatalogue()
{
    // Destroy the job before the list it writes into.
    scanJob.reset();
}

juce::Array<juce::PluginDescription> PluginCatalogue::getKnownPlugins() const
{
    return knownPlugins.getTypes();
}

juce::StringArray PluginCatalogue::getBlacklistedFiles() const
{
    return knownPlugins.getBlacklistedFiles();
}

bool PluginCatalogue::hasPlugin(const juce::String& fileOrIdentifier) const
{
    for (const auto& type : knownPlugins.getTypes())
        if (type.fileOrIdentifier == fileOrIdentifier) return true;

    return false;
}

juce::FileSearchPath PluginCatalogue::getSearchPaths() const
{
    // The two locations the VST3 spec defines on Windows, and deliberately nothing else:
    // every VST3 installer targets them by default, so a configurable search path would be
    // a setting almost nobody needs, in exchange for "my plugin is missing" reports from
    // everybody who installed one somewhere unusual.
    auto* format = formatManager.getFormat(0);
    return format != nullptr ? format->getDefaultLocationsToSearch() : juce::FileSearchPath{};
}

PluginScanJob::Plan PluginCatalogue::scanPlan(const juce::FileSearchPath& vst3Paths) const
{
    // VST3 in the paths asked for; every other format in its own standard places (Audio Units
    // come from the system's component registry, so their paths are unused).
    PluginScanJob::Plan plan;
    for (auto* format : formatManager.getFormats())
    {
        auto paths = format->getName() == "VST3" ? vst3Paths : format->getDefaultLocationsToSearch();
#if JUCE_WINDOWS
        // JUCE's VST 2 defaults stop at Program Files\(Steinberg\)VstPlugins and the registry's
        // path; most installers now use the Common Files folders.
        if (format->getName() == "VST")
        {
            const auto common = juce::File::getSpecialLocation(juce::File::globalApplicationsDirectory).getChildFile("Common Files");
            paths.addIfNotAlreadyThere(common.getChildFile("VST2"));
            paths.addIfNotAlreadyThere(common.getChildFile("Steinberg").getChildFile("VST2"));
        }
#endif
        plan.emplace_back(format, paths);
    }
    return plan;
}

bool PluginCatalogue::startScan(const juce::FileSearchPath& pathsToScan,
                                PluginScanJob::ProgressCallback onProgress,
                                PluginScanJob::FinishedCallback onFinished)
{
    PluginScanJob::Plan plan;
    // Audio Units ignore paths (they list the whole system), so a scan of given folders leaves them out.
    for (auto* format : formatManager.getFormats())
        if (format->getName() != "AudioUnit") plan.emplace_back(format, pathsToScan);
    return startPlannedScan(std::move(plan), std::move(onProgress), std::move(onFinished));
}

bool PluginCatalogue::startDefaultScan(PluginScanJob::ProgressCallback onProgress, PluginScanJob::FinishedCallback onFinished)
{
    return startPlannedScan(scanPlan(getSearchPaths()), std::move(onProgress), std::move(onFinished));
}

bool PluginCatalogue::startPlannedScan(PluginScanJob::Plan plan, PluginScanJob::ProgressCallback onProgress,
                                       PluginScanJob::FinishedCallback onFinished)
{
    if (isScanning()) return false;
    if (plan.empty()) return false;

    scanJob = std::make_unique<PluginScanJob>(
        knownPlugins, std::move(plan), deadMansPedalFile(), std::move(onProgress),
        [this, finished = std::move(onFinished)](bool completed)
        {
            removeUninstalledPlugins();
            saveKnownPlugins();
            // Cleared before the caller's callback so anything that callback broadcasts —
            // notably the refreshed plugin list — already reports the scan as over.
            scanning.store(false, std::memory_order_release);
            if (finished) finished(completed);
        });

    scanning.store(true, std::memory_order_release);
    scanJob->start();
    return true;
}

void PluginCatalogue::cancelScan()
{
    if (scanJob != nullptr) scanJob->cancel();
}

bool PluginCatalogue::isScanning() const
{
    return scanning.load(std::memory_order_acquire);
}

void PluginCatalogue::clearBlacklist()
{
    knownPlugins.clearBlacklistedFiles();
    // The pedal file is replayed into the blacklist at the start of every scan, so it has to
    // go too or the next scan restores what was just cleared.
    deadMansPedalFile().deleteFile();
    saveKnownPlugins();
}

std::unique_ptr<juce::AudioPluginInstance> PluginCatalogue::createInstance(
    const juce::PluginDescription& description, double sampleRate, int blockSize, juce::String& errorMessage)
{
    return formatManager.createPluginInstance(description, sampleRate, blockSize, errorMessage);
}

int PluginCatalogue::forgetPluginsIn(const juce::File& folder)
{
    int removed = 0;
    for (const auto& type : knownPlugins.getTypes())
    {
        if (!juce::File::isAbsolutePath(type.fileOrIdentifier)) continue;
        const juce::File file(type.fileOrIdentifier);
        if (file != folder && !file.isAChildOf(folder)) continue;
        knownPlugins.removeType(type);
        ++removed;
    }
    if (removed > 0) saveKnownPlugins();
    return removed;
}

int PluginCatalogue::removeUninstalledPlugins()
{
    // The catalogue is a cache that JUCE only ever adds to, so a plugin uninstalled from the
    // machine would otherwise stay in the picker forever — offering the user something that
    // can no longer load. Reconciling costs nothing worth measuring: this only asks the
    // filesystem whether each cached path is still there, and never loads or instantiates
    // anything, which is why it can run at load as well as after a scan. Existence is a sound
    // test because we search two fixed local folders, so a path that has gone really has gone,
    // rather than being a removable drive that happens to be unplugged.
    int removed = 0;
    for (const auto& type : knownPlugins.getTypes())
    {
        // Only path-like identifiers can be checked; anything else is left alone rather than
        // guessed at.
        if (!juce::File::isAbsolutePath(type.fileOrIdentifier)) continue;
        if (juce::File(type.fileOrIdentifier).exists()) continue;

        knownPlugins.removeType(type);
        ++removed;
        log::info("plugins", "forgetting uninstalled plugin " + type.name + " (" + type.fileOrIdentifier + ")");
    }

    if (removed > 0)
        log::info("plugins", "removed " + juce::String(removed) + " uninstalled plugin(s) from the catalogue");

    return removed;
}

juce::File PluginCatalogue::knownPluginsFile() const
{
    return dataDir.getChildFile("known-plugins.xml");
}

juce::File PluginCatalogue::deadMansPedalFile() const
{
    return dataDir.getChildFile("scan-crashes.txt");
}

void PluginCatalogue::saveKnownPlugins() const
{
    const auto xml = knownPlugins.createXml();
    if (xml == nullptr) return;

    if (!xml->writeTo(knownPluginsFile()))
        log::error("plugins", "failed to write " + knownPluginsFile().getFullPathName());
}

} // namespace stemcel::plugins
