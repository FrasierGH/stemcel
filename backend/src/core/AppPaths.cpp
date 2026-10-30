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

} // namespace stemcel
