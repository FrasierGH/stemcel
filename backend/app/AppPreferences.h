#pragma once

#include <juce_events/juce_events.h>

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>

#include <vector>

namespace stemcel::native
{

struct RecentProject
{
    juce::String path;
    juce::String name;
};

// The user's preferences, stored in the same `preferences.json` the Electron app uses
// (`<app data>/Stemcel/preferences.json`), so both apps share settings and recent projects
// while the native app reaches parity. The whole document is kept, unknown keys included, so
// fields only the Electron app understands survive a native save.
class AppPreferences : public juce::ChangeBroadcaster
{
  public:
    static constexpr int kMaxRecentProjects = 10;

    AppPreferences();

    void load();
    void save() const;

    juce::File file() const { return prefsFile; }

    // Section/key access for everything without a typed accessor, e.g. ("ui", "followPlayback").
    juce::var get(const juce::String& section, const juce::String& key, const juce::var& fallback = {}) const;
    void set(const juce::String& section, const juce::String& key, const juce::var& value);
    // A whole top-level object (e.g. "enabledMidiInputs"), or void.
    juce::var getSection(const juce::String& name) const;

    // Audio output the engine opens at start (empty = system default).
    juce::String outputTypeName() const;
    juce::String outputDeviceName() const;
    void setAudioOutput(const juce::String& typeName, const juce::String& deviceName);

    bool keepAwakeFor(const juce::String& deviceName) const;
    void setKeepAwakeFor(const juce::String& deviceName, bool enabled);

    std::vector<RecentProject> recentProjects() const;
    // Newest first, de-duplicated case-insensitively by path, capped at kMaxRecentProjects.
    void addRecentProject(const juce::String& path, const juce::String& name);
    void removeRecentProject(const juce::String& path);
    void clearRecentProjects();

    juce::Rectangle<int> windowBounds() const;
    bool windowMaximised() const;
    void setWindowState(juce::Rectangle<int> bounds, bool maximised);

    juce::File defaultProjectDirectory() const;
    juce::File defaultClipDirectory() const;
    bool seedTempoFromFirstClip() const;
    bool followPlayback() const;

  private:
    juce::DynamicObject* section(const juce::String& name) const;
    juce::DynamicObject& sectionForWrite(const juce::String& name);

    juce::File prefsFile;
    juce::var data;
};

} // namespace stemcel::native
