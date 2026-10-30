#include "AppPreferences.h"

#include "AppPaths.h"
#include "Log.h"

namespace stemcel::native
{
namespace
{
juce::File homeStemcel()
{
    return juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(appFolderName());
}
} // namespace

AppPreferences::AppPreferences()
    : prefsFile(userAppDataDirectory().getChildFile(appFolderName()).getChildFile("preferences.json")),
      data(new juce::DynamicObject())
{
}

void AppPreferences::load()
{
    if (!prefsFile.existsAsFile()) return;

    const auto parsed = juce::JSON::parse(prefsFile.loadFileAsString());
    if (parsed.getDynamicObject() != nullptr)
    {
        data = parsed;
        return;
    }
    log::warn("prefs", "preferences.json is not a JSON object; starting from defaults");
}

void AppPreferences::save() const
{
    // Listeners (open workspaces, the toast layer) re-read what they show.
    const_cast<AppPreferences*>(this)->sendChangeMessage();
    prefsFile.getParentDirectory().createDirectory();
    // Write a sibling and move it over, so a crash mid-write never leaves a truncated file.
    const auto tmp = prefsFile.getSiblingFile("preferences.json.tmp");
    if (!tmp.replaceWithText(juce::JSON::toString(data, false)) || !tmp.moveFileTo(prefsFile))
        log::warn("prefs", "could not write " + prefsFile.getFullPathName());
}

juce::DynamicObject* AppPreferences::section(const juce::String& name) const
{
    return data.getProperty(name, {}).getDynamicObject();
}

juce::DynamicObject& AppPreferences::sectionForWrite(const juce::String& name)
{
    if (auto* existing = section(name)) return *existing;
    auto* created = new juce::DynamicObject();
    data.getDynamicObject()->setProperty(name, juce::var(created));
    return *created;
}

juce::var AppPreferences::get(const juce::String& sectionName, const juce::String& key,
                              const juce::var& fallback) const
{
    if (auto* s = section(sectionName); s != nullptr && s->hasProperty(key)) return s->getProperty(key);
    return fallback;
}

juce::var AppPreferences::getSection(const juce::String& name) const
{
    auto* s = section(name);
    return s != nullptr ? juce::var(s) : juce::var();
}

void AppPreferences::set(const juce::String& sectionName, const juce::String& key, const juce::var& value)
{
    sectionForWrite(sectionName).setProperty(key, value);
}

juce::String AppPreferences::outputTypeName() const
{
    const auto v = get("audioOutput", "typeName");
    return v.isString() ? v.toString() : juce::String();
}

juce::String AppPreferences::outputDeviceName() const
{
    const auto v = get("audioOutput", "deviceName");
    return v.isString() ? v.toString() : juce::String();
}

void AppPreferences::setAudioOutput(const juce::String& typeName, const juce::String& deviceName)
{
    set("audioOutput", "typeName", typeName.isEmpty() ? juce::var() : juce::var(typeName));
    set("audioOutput", "deviceName", deviceName.isEmpty() ? juce::var() : juce::var(deviceName));
}

bool AppPreferences::keepAwakeFor(const juce::String& deviceName) const
{
    if (deviceName.isEmpty()) return false;
    if (auto* map = data.getProperty("keepAwakeByDevice", {}).getDynamicObject())
        return static_cast<bool>(map->getProperty(deviceName));
    return false;
}

void AppPreferences::setKeepAwakeFor(const juce::String& deviceName, bool enabled)
{
    if (deviceName.isEmpty()) return;
    sectionForWrite("keepAwakeByDevice").setProperty(deviceName, enabled);
}

std::vector<RecentProject> AppPreferences::recentProjects() const
{
    std::vector<RecentProject> out;
    if (const auto* list = data.getProperty("recentProjects", {}).getArray())
    {
        for (const auto& entry : *list)
        {
            const auto path = entry.getProperty("path", {}).toString();
            if (path.isEmpty()) continue;
            out.push_back({path, entry.getProperty("name", {}).toString()});
        }
    }
    return out;
}

void AppPreferences::addRecentProject(const juce::String& path, const juce::String& name)
{
    juce::Array<juce::var> list;
    auto* entry = new juce::DynamicObject();
    entry->setProperty("path", path);
    entry->setProperty("name", name);
    list.add(juce::var(entry));
    for (const auto& existing : recentProjects())
    {
        if (existing.path.equalsIgnoreCase(path)) continue;
        if (list.size() >= kMaxRecentProjects) break;
        auto* e = new juce::DynamicObject();
        e->setProperty("path", existing.path);
        e->setProperty("name", existing.name);
        list.add(juce::var(e));
    }
    data.getDynamicObject()->setProperty("recentProjects", juce::var(list));
}

void AppPreferences::removeRecentProject(const juce::String& path)
{
    juce::Array<juce::var> list;
    for (const auto& existing : recentProjects())
    {
        if (existing.path.equalsIgnoreCase(path)) continue;
        auto* e = new juce::DynamicObject();
        e->setProperty("path", existing.path);
        e->setProperty("name", existing.name);
        list.add(juce::var(e));
    }
    data.getDynamicObject()->setProperty("recentProjects", juce::var(list));
}

void AppPreferences::clearRecentProjects()
{
    data.getDynamicObject()->setProperty("recentProjects", juce::var(juce::Array<juce::var>()));
}

juce::Rectangle<int> AppPreferences::windowBounds() const
{
    const int w = static_cast<int>(get("window", "width", 1400));
    const int h = static_cast<int>(get("window", "height", 900));
    const auto x = get("window", "x");
    const auto y = get("window", "y");
    if (x.isVoid() || y.isVoid()) return {0, 0, w, h};
    return {static_cast<int>(x), static_cast<int>(y), w, h};
}

bool AppPreferences::windowMaximised() const
{
    return static_cast<bool>(get("window", "maximized", false));
}

void AppPreferences::setWindowState(juce::Rectangle<int> bounds, bool maximised)
{
    auto& w = sectionForWrite("window");
    w.setProperty("x", bounds.getX());
    w.setProperty("y", bounds.getY());
    w.setProperty("width", bounds.getWidth());
    w.setProperty("height", bounds.getHeight());
    w.setProperty("maximized", maximised);
}

juce::File AppPreferences::defaultProjectDirectory() const
{
    const auto configured = get("paths", "defaultProjectDir").toString();
    if (configured.isNotEmpty() && juce::File::isAbsolutePath(configured)) return juce::File(configured);
    return homeStemcel().getChildFile("Projects");
}

juce::File AppPreferences::defaultClipDirectory() const
{
    const auto configured = get("paths", "defaultClipDir").toString();
    if (configured.isNotEmpty() && juce::File::isAbsolutePath(configured)) return juce::File(configured);
    return juce::File::getSpecialLocation(juce::File::userMusicDirectory);
}

bool AppPreferences::seedTempoFromFirstClip() const
{
    return static_cast<bool>(get("ui", "seedProjectTempoFromFirstClip", true));
}

bool AppPreferences::followPlayback() const
{
    return static_cast<bool>(get("ui", "followPlayback", true));
}

} // namespace stemcel::native
