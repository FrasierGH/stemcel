#include "AppPaths.h"

#include <utility>
#include <vector>

namespace stemcel
{

juce::File userAppDataDirectory()
{
    const auto appData = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
#if JUCE_MAC
    return appData.getChildFile("Application Support");
#else
    return appData;
#endif
}

juce::File systemTempDirectory()
{
#if JUCE_MAC || JUCE_LINUX
    // Mirror Node's os.tmpdir(): $TMPDIR, then $TMP, then $TEMP, else /tmp.
    for (const auto* name : {"TMPDIR", "TMP", "TEMP"})
    {
        const auto value = juce::SystemStats::getEnvironmentVariable(name, {});
        if (value.isNotEmpty() && juce::File::isAbsolutePath(value)) return juce::File(value);
    }
    return juce::File("/tmp");
#else
    return juce::File::getSpecialLocation(juce::File::tempDirectory);
#endif
}

namespace
{
juce::String& folderName()
{
    static juce::String name{"Stemcel"};
    return name;
}
} // namespace

juce::String appFolderName()
{
    return folderName();
}

void setAppFolderName(const juce::String& name)
{
    if (name.isNotEmpty()) folderName() = name;
}

namespace
{
std::vector<std::pair<juce::File, juce::File>>& movedFolders()
{
    static std::vector<std::pair<juce::File, juce::File>> moved;
    return moved;
}
} // namespace

void migrateAppFolders(const juce::String& oldName, const juce::String& newName)
{
    for (const auto& root : {juce::File::getSpecialLocation(juce::File::userHomeDirectory), userAppDataDirectory()})
    {
        const auto from = root.getChildFile(oldName);
        const auto to = root.getChildFile(newName);
        if (from.isDirectory() && !to.exists()) from.moveFileTo(to);
        // Remembered whether it moved now or on an earlier run, for paths saved before.
        if (!from.exists() && to.isDirectory()) movedFolders().emplace_back(from, to);
    }
}

juce::String remapMovedAppPath(const juce::String& path)
{
    if (path.isEmpty() || !juce::File::isAbsolutePath(path)) return path;
    const juce::File file(path);
    if (file.exists()) return path;
    for (const auto& [from, to] : movedFolders())
        if (file.isAChildOf(from)) return to.getChildFile(file.getRelativePathFrom(from)).getFullPathName();
    return path;
}

} // namespace stemcel
