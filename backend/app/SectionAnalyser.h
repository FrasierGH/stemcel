#pragma once

#include "AppPreferences.h"
#include "EngineClient.h"
#include "ProjectModel.h"

#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <memory>
#include <set>

namespace stemcel::native
{

// Runs Stemcel's own section analysis (sections::analyseFile) off the message thread and
// stores the result on the library item (LIBRARY_ITEM_SET_SECTIONS), reusing the per-file
// cache under ~/Stemcel/Analysis. On request from the menus, and on its own for every source
// track without sections once it has a tempo when Preferences > Timeline > "Detect song
// sections" is on. Items from rekordbox already carry rekordbox's own.
class SectionAnalyser final : private juce::ChangeListener
{
  public:
    SectionAnalyser(EngineClient& engine, ProjectMirror& mirror, AppPreferences& prefs);
    ~SectionAnalyser() override;

    // Analyses an item (again, when `force`), replacing its sections.
    void analyse(const juce::String& itemId, bool force);
    bool busy() const { return running > 0; }
    std::function<void(const juce::String&, bool)> onNotify;

  private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    EngineClient& engine;
    ProjectMirror& mirror;
    AppPreferences& prefs;
    std::set<juce::String> tried; // automatic analysis, once per item per session
    int running = 0;
    juce::ThreadPool pool{1};
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
};

} // namespace stemcel::native
