#pragma once

#include "AppController.h"
#include "AppPreferences.h"
#include "EngineClient.h"
#include "ProjectTabs.h"
#include "StartupScreen.h"
#include "Toasts.h"
#include "Workspace.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace stemcel::native
{

namespace CommandIds
{
enum : juce::CommandID
{
    newProject = 0x2001,
    openProject,
    saveProject,
    saveProjectAs,
    preferences,
    undo,
    redo,
    about,
    playPause,
    splitAtPlayhead,
    duplicateClip,
    deleteClip,
    toggleLock,
    addMarker,
    addTrack,
    zoomIn,
    zoomOut,
    zoomFit,
    importAudio,
    showLibraryTab,
    showFilesTab,
    showRekordboxTab,
    exportMixdown,
    projectProperties,
    recordAudio,
    importFromProject,
    cutClips,
    copyClips,
    pasteClips,
    toggleMixer,
};
} // namespace CommandIds

// The window's content: startup screen or workspace, a status bar, and the toast layer.
// Owns the app's commands (menus and keyboard shortcuts both route here).
class MainComponent final : public juce::Component,
                            public juce::ApplicationCommandTarget,
                            public juce::MenuBarModel
{
  public:
    MainComponent(EngineClient& engine, AppPreferences& prefs, AppController& controller,
                  juce::ApplicationCommandManager& commands);
    ~MainComponent() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    // ApplicationCommandTarget
    // Falls through to the application, which owns Quit.
    juce::ApplicationCommandTarget* getNextCommandTarget() override { return juce::JUCEApplication::getInstance(); }
    void getAllCommands(juce::Array<juce::CommandID>& ids) override;
    void getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info) override;
    bool perform(const InvocationInfo& info) override;

    // MenuBarModel
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex(int topLevelMenuIndex, const juce::String& menuName) override;
    void menuItemSelected(int menuItemID, int topLevelMenuIndex) override;

    // Extra items for the macOS application menu (Preferences, About).
    juce::PopupMenu macAppMenuExtras();

  private:
    void stateChanged();
    juce::String engineStatusText() const;

    EngineClient& engine;
    AppPreferences& prefs;
    AppController& controller;
    juce::ApplicationCommandManager& commandManager;
    Subscriptions subscriptions;

    StartupScreen startup;
    ProjectTabStrip tabStrip{controller};
    std::unique_ptr<Workspace> workspace;
    Toasts toasts;

    bool engineReady = false;
    struct PrefsWatcher final : juce::ChangeListener
    {
        std::function<void()> fn;
        void changeListenerCallback(juce::ChangeBroadcaster*) override { if (fn) fn(); }
    } prefsWatcher;
    juce::String audioState;
    juce::String deviceName;
};

} // namespace stemcel::native
