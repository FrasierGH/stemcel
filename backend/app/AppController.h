#pragma once

#include "AppPreferences.h"
#include "Autosave.h"
#include "EngineClient.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace stemcel::native
{

// Project lifecycle shared by the menus, shortcuts and startup screen: new, open, save,
// save-as and quit, each behind the unsaved-changes guard, plus recent projects. Mirrors the
// renderer's useAppMenuActions/useUnsavedChangesGuard and Electron main's project handlers.
class AppController : private juce::Timer
{
  public:
    // How often a saved project with changes writes itself back to its file, quietly.
    static constexpr int kSeamlessSaveSeconds = 180;

    AppController(EngineClient& engine, AppPreferences& prefs);

    // The component that dialogs are centred on.
    void setDialogParent(juce::Component* parent) { dialogParent = parent; }

    void newProject();
    void openProjectDialog();
    void openProject(const juce::File& file);
    void save(std::function<void(bool saved)> done = {});
    void saveAs(std::function<void(bool saved)> done = {});
    // Runs `proceed` once unsaved changes are saved or discarded; does nothing on cancel.
    void guardUnsavedChanges(std::function<void()> proceed);
    void requestQuit();
    // Opens an autosave in place of its project (crash recovery).
    void restoreAutosave(const Autosave::Entry& entry);
    Autosave& autosaver() { return autosave; }
    void undo();
    void redo();

    // True once a project is open in the workspace (after New or Open), as opposed to the
    // startup screen.
    bool isWorkspaceOpen() const { return workspaceOpen; }
    bool isDirty() const { return dirty; }
    bool canUndo() const { return undoAvailable; }
    bool canRedo() const { return redoAvailable; }
    juce::String undoLabel() const { return undoText; }
    juce::String redoLabel() const { return redoText; }
    juce::String projectName() const { return name; }
    juce::String projectPath() const { return path; }

    // Any change to the above; listeners re-read what they show.
    std::function<void()> onStateChanged;
    // Writes the open project's view state into its file (set while a workspace is open).
    std::function<void()> saveViewState;
    // Brief user-facing message (toast).
    std::function<void(const juce::String& message, bool isError)> onNotify;

    // `<chosen dir>/<Name>/<Name>.stemcel`, unless the user already picked a same-named
    // folder (Electron main's projectFolderPath).
    static juce::File projectFolderPath(const juce::File& chosen);

    // Project tabs (docs/native-app.md, "Project tabs"): several mixes open at once, one in the
    // engine. Switching parks the open one - a project with unsaved changes (or never saved)
    // is written to the tab store and restored as it was, unsaved; a clean one reopens from its
    // file - and opens the other. New and Open add a tab instead of replacing the project.
    struct Tab
    {
        juce::String id;
        juce::String name;
        juce::String path;
        bool dirty = false;
        juce::File parked; // the unsaved state while another tab is open
    };
    const std::vector<Tab>& projectTabs() const { return tabs; }
    int activeTabIndex() const { return activeTab; }
    void switchToTab(int index);
    // Asks before discarding a tab's unsaved changes.
    void closeTab(int index);
    // ~/Stemcel/Tabs: where tabs park unsaved projects.
    static juce::File tabStore();

  private:
    void parkActive();
    void syncActiveTab();
    void addTab();
    void changed();
    void notify(const juce::String& message, bool isError);

    EngineClient& engine;
    AppPreferences& prefs;
    Subscriptions subscriptions;
    Autosave autosave;
    juce::Component* dialogParent = nullptr;
    std::unique_ptr<juce::FileChooser> chooser;

    bool workspaceOpen = false;
    bool dirty = false;
    bool undoAvailable = false;
    bool redoAvailable = false;
    juce::String undoText;
    juce::String redoText;
    juce::String name;
    juce::String path;

    // Save requests waiting for PROJECT_SAVED.
    std::function<void(bool)> pendingSave;
    // The seamless save: every kSeamlessSaveSeconds a saved project with changes is written to
    // its own file with no message; it waits a moment while a mouse button is down, a dialog is
    // open or another save is running.
    void timerCallback() override;
    bool quietSave = false;

    std::vector<Tab> tabs;
    int activeTab = -1;
    bool awaitingTabState = false;
};

} // namespace stemcel::native
