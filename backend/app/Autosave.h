#pragma once

#include "AppPreferences.h"
#include "EngineClient.h"

#include <juce_events/juce_events.h>

#include <functional>
#include <vector>

namespace stemcel::native
{

// Background autosave and crash recovery, on the Electron app's storage so either app
// recovers the other's work: `<userData>/Stemcel/autosave/<projectId>/` holding
// `autosave.stemcel` and `manifest.json` ({projectId, originalPath, projectName,
// savedAtIso, pending, appVersion}). A saved project's id is the first 8 bytes of the
// SHA-1 of its lower-cased path; an untitled one gets a random id. A tick marks the
// manifest pending, asks the engine for PROJECT_AUTOSAVE, and confirms it on
// PROJECT_AUTOSAVED; recovery ignores pending (possibly partial) buckets.
class Autosave final : private juce::Timer
{
  public:
    struct Entry
    {
        juce::String projectId;
        juce::String originalPath; // empty = never saved
        juce::String projectName;
        juce::File autosaveFile;
        juce::String savedAtIso;
        bool originalExists = false;
    };

    Autosave(EngineClient& engine, AppPreferences& prefs);
    ~Autosave() override;

    // Buckets newer than their project file (or whose project was never saved / is gone).
    static std::vector<Entry> listRecoverable();
    static void clear(const juce::String& projectId);
    static juce::File root();
    static juce::String projectIdForPath(const juce::String& path);

    // Loads an autosave in place of its project (PROJECT_LOAD_RECOVERY), keeping its
    // bucket's id so further autosaves land in the same place.
    void restore(const Entry& entry);

    // Re-reads the autosave preferences (enabled, interval).
    void preferencesChanged();

  private:
    void timerCallback() override;
    void tick();
    void writeManifest(bool pending);
    void identityChanged(const juce::var& state);

    EngineClient& engine;
    AppPreferences& prefs;
    Subscriptions subscriptions;
    juce::String projectId;
    juce::String pendingRecoveredId;
    juce::String filePath;
    juce::String projectName;
    bool dirty = false;
    bool inFlight = false;
};

} // namespace stemcel::native
