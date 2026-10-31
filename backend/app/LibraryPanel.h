#pragma once

#include "AppPreferences.h"
#include "AudioImport.h"
#include "EngineClient.h"
#include "LiveState.h"
#include "ProjectModel.h"
#include "Rekordbox.h"
#include "TimelineActions.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace stemcel::native
{

// Drag payloads the timeline understands besides "libraryItem:<id>": files or rekordbox
// tracks that still need importing.
juce::var importDragDescription(const std::vector<ImportRequest>& requests);
std::vector<ImportRequest> importRequestsFromDrag(const juce::var& description);

// Plays a file through the engine's preview player (the renderer's audition bar).
class Audition final : public juce::ChangeBroadcaster
{
  public:
    Audition(EngineClient& engine, AudioImporter& importer);
    ~Audition() override;

    void toggle(const juce::File& file);
    void stop();
    juce::File current() const { return playing ? file : juce::File(); }

  private:
    EngineClient& engine;
    AudioImporter& importer;
    Subscriptions subscriptions;
    juce::File file;
    bool waitingForLoad = false;
    bool playing = false;
    juce::ThreadPool pool{1};
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
};

struct LibraryContext
{
    EngineClient& engine;
    ProjectMirror& mirror;
    TimelineActions& actions;
    TimelineViewState& view;
    LiveState& live;
    AppPreferences& prefs;
    AudioImporter& importer;
    Rekordbox& rekordbox;
    Audition& audition;
    std::function<void(const juce::String&, bool)> notify;
    std::function<void(const juce::String& itemId)> separateStems;
    std::function<void(const juce::String& itemId)> openScratch;
    std::function<void(const juce::String& itemId)> openEditor;
    // Lays the tracks out as a DJ mix (automix::build): harmonically reordered or as given.
    std::function<void(std::vector<juce::String> itemIds, bool reorder, const juce::String& style, bool matchKeys)> autoMix;
    // The vocals of one track over the instrumental of another (MashupBuilder).
    std::function<void(const juce::String& vocalItemId, const juce::String& beatItemId)> mashup;
    // Stemcel's own analysis of a track's sections (SectionAnalyser), run again.
    std::function<void(const juce::String& itemId)> analyseSections;
};

class LibraryItemsView;
class FilesBrowser;
class RekordboxBrowser;

// The panel beneath the timeline: the project library, a Files browser over chosen
// folders, and the rekordbox library. Accepts files dropped from the OS (imported into
// the library).
class LibraryPanel final : public juce::Component, public juce::FileDragAndDropTarget
{
  public:
    explicit LibraryPanel(LibraryContext context);
    ~LibraryPanel() override;
    // The Track FX and Project FX tabs' panels (TrackFxView, ProjectFxView).
    juce::Component* trackFxPanel() const { return trackFx.get(); }
    juce::Component* projectFxPanel() const { return projectFx.get(); }

    void paint(juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    // 0 Library, 1 Files, 2 rekordbox, 3 Transitions, 4 Track FX, 5 Project FX, 6 Plugins.
    void showTab(int index);
    static constexpr int kTabCount = 7;
    // Opens the in-place name field on a library item's tile.
    void renameItem(const juce::String& itemId);
    void importWithDialog();
    // A line of background activity (stem separation) beside the tabs; empty clears it.
    void setActivity(const juce::String& text);
    static constexpr int kDefaultHeight = 260;

  private:
    LibraryContext ctx;
    juce::OwnedArray<juce::TextButton> tabs;
    std::vector<juce::Component*> views; // in tab order
    juce::TextButton importButton{"Import..."};
    juce::Label status;
    std::unique_ptr<LibraryItemsView> library;
    std::unique_ptr<FilesBrowser> files;
    std::unique_ptr<RekordboxBrowser> rekordbox;
    std::unique_ptr<juce::Component> transitions, trackFx, projectFx, plugins;
    std::unique_ptr<juce::FileChooser> chooser;
    int activeTab = 0;
    bool dragOver = false;
    juce::String activity;
};

} // namespace stemcel::native
