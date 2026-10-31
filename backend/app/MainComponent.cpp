#include "MainComponent.h"

#include "ExportDialog.h"
#include "ProjectDialogs.h"
#include "RecordDialog.h"
#include "PreferencesDialog.h"
#include "RecoveryDialog.h"
#include "Theme.h"
#include "Version.h"
#include "Workspace.h"

namespace stemcel::native
{
namespace
{
constexpr int kStatusBarHeight = 22;
constexpr int kRecentMenuBase = 1000;
constexpr int kClearRecentMenuId = 1999;
} // namespace

MainComponent::MainComponent(EngineClient& engineClient, AppPreferences& preferences, AppController& appController,
                             juce::ApplicationCommandManager& commands)
    : engine(engineClient), prefs(preferences), controller(appController), commandManager(commands),
      subscriptions(engineClient), startup(appController, preferences)
{
    addAndMakeVisible(startup);
    addAndMakeVisible(toasts);
    toasts.setAlwaysOnTop(true);

    controller.setDialogParent(this);
    controller.onStateChanged = [this] { stateChanged(); };
    controller.onNotify = [this](const juce::String& message, bool isError) { toasts.show(message, isError); };

    subscriptions.on("ENGINE_AUDIO_STATUS",
                     [this](const juce::var& p)
                     {
                         audioState = p.getProperty("state", {}).toString();
                         // Once the engine is up, offer to recover unsaved work from a crash.
                         if (!engineReady && !juce::JUCEApplication::getCommandLineParameters().contains("--smoke-test"))
                             juce::MessageManager::callAsync([this] { RecoveryDialog::showIfNeeded(controller); });
                         engineReady = true;
                         stateChanged();
                     });
    subscriptions.on("AUDIO_DEVICES_LIST",
                     [this](const juce::var& p)
                     {
                         deviceName = p.getProperty("currentDeviceName", {}).toString();
                         repaint();
                     });
    subscriptions.on("AUDIO_DEVICE_CHANGED",
                     [this](const juce::var& p)
                     {
                         if (!static_cast<bool>(p.getProperty("ok", true)))
                             toasts.show("Could not open the audio device: " + p.getProperty("error", {}).toString(),
                                         true);
                     });
    subscriptions.on("ENGINE_ERROR",
                     [this](const juce::var& p)
                     { toasts.show("Engine error: " + p.getProperty("message", {}).toString(), true); });

    prefsWatcher.fn = [this] { toasts.enabled = static_cast<bool>(prefs.get("toasts", "enabled", true)); };
    prefsWatcher.fn();
    prefs.addChangeListener(&prefsWatcher);

    commandManager.registerAllCommandsForTarget(this);
    // Menus and shortcuts reach these commands wherever keyboard focus is (e.g. in a dialog).
    commandManager.setFirstCommandTarget(this);
    setApplicationCommandManagerToWatch(&commandManager);
    stateChanged();
}

MainComponent::~MainComponent()
{
    commandManager.setFirstCommandTarget(nullptr);
    prefs.removeChangeListener(&prefsWatcher);
    controller.onStateChanged = nullptr;
    controller.saveViewState = nullptr;
    controller.onNotify = nullptr;
    controller.setDialogParent(nullptr);
}

void MainComponent::stateChanged()
{
    const bool showWorkspace = controller.isWorkspaceOpen();
    if (showWorkspace && workspace == nullptr)
    {
        workspace = std::make_unique<Workspace>(
            engine, controller, prefs, [this](const juce::String& message, bool isError) { toasts.show(message, isError); });
        controller.saveViewState = [this] { if (workspace != nullptr) workspace->saveViewState(); };
        addAndMakeVisible(*workspace);
        toasts.toFront(false);
    }
    startup.setVisible(!showWorkspace);
    if (workspace != nullptr) workspace->setVisible(showWorkspace);
    if (tabStrip.getParentComponent() == nullptr) addChildComponent(tabStrip);
    tabStrip.setVisible(showWorkspace);
    tabStrip.refresh();
    resized();
    if (!showWorkspace)
        startup.refresh(engineReady, engineReady ? juce::String() : juce::String("Starting the audio engine..."));

    if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
    {
        const auto name = controller.projectName().isNotEmpty() ? controller.projectName() : juce::String("Untitled");
        window->setName(showWorkspace ? (controller.isDirty() ? "* " : "") + name + " - Stemcel"
                                      : juce::String("Stemcel"));
        // The macOS close button's "unsaved" dot.
        if (auto* peer = window->getPeer()) peer->setDocumentEditedStatus(showWorkspace && controller.isDirty());
    }
    commandManager.commandStatusChanged();
    menuItemsChanged();
    resized();
    repaint();
}

juce::String MainComponent::engineStatusText() const
{
    if (!engineReady) return "Starting the audio engine...";
    if (audioState == "ready") return deviceName.isNotEmpty() ? "Output: " + deviceName : juce::String("Audio ready");
    return "No audio output device";
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
    const auto bar = getLocalBounds().removeFromBottom(kStatusBarHeight);
    g.setColour(colours::zinc950);
    g.fillRect(bar);
    g.setColour(colours::border);
    g.drawHorizontalLine(bar.getY(), 0.0f, static_cast<float>(getWidth()));
    g.setColour(audioState == "ready" || !engineReady ? colours::textFaint : colours::amber400);
    g.setFont(uiFont(12.0f));
    g.drawText(engineStatusText(), bar.reduced(10, 0), juce::Justification::centredLeft);
    g.setColour(colours::textFaint);
    g.drawText(juce::String("v") + kBackendVersion, bar.reduced(10, 0), juce::Justification::centredRight);
}

void MainComponent::resized()
{
    auto area = getLocalBounds();
    area.removeFromBottom(kStatusBarHeight);
    startup.setBounds(area);
    if (tabStrip.isVisible()) tabStrip.setBounds(area.removeFromTop(ProjectTabStrip::kHeight));
    if (workspace != nullptr) workspace->setBounds(area);
    toasts.setBounds(getLocalBounds());
}

void MainComponent::getAllCommands(juce::Array<juce::CommandID>& ids)
{
    ids.addArray({CommandIds::newProject, CommandIds::openProject, CommandIds::saveProject, CommandIds::saveProjectAs,
                  CommandIds::preferences, CommandIds::undo, CommandIds::redo, CommandIds::about,
                  CommandIds::playPause, CommandIds::splitAtPlayhead, CommandIds::duplicateClip,
                  CommandIds::deleteClip, CommandIds::toggleLock, CommandIds::addMarker, CommandIds::addTrack,
                  CommandIds::zoomIn, CommandIds::zoomOut, CommandIds::zoomFit, CommandIds::importAudio,
                  CommandIds::showLibraryTab, CommandIds::showFilesTab, CommandIds::showRekordboxTab,
                  CommandIds::exportMixdown, CommandIds::projectProperties, CommandIds::recordAudio,
                  CommandIds::importFromProject, CommandIds::cutClips, CommandIds::copyClips, CommandIds::pasteClips,
                  CommandIds::toggleMixer, CommandIds::selectAllClips, CommandIds::joinClips, CommandIds::toggleAutomation,
                  CommandIds::toggleLoop, CommandIds::toggleMetronome, CommandIds::toggleSnap, CommandIds::openClipEditor});
}

void MainComponent::getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info)
{
    const auto cmd = juce::ModifierKeys::commandModifier;
    const bool open = controller.isWorkspaceOpen();
    switch (id)
    {
    case CommandIds::newProject:
        info.setInfo("New Project", "Start a new project", "File", 0);
        info.addDefaultKeypress('n', cmd);
        info.setActive(engineReady);
        break;
    case CommandIds::openProject:
        info.setInfo("Open Project...", "Open a project", "File", 0);
        info.addDefaultKeypress('o', cmd);
        info.setActive(engineReady);
        break;
    case CommandIds::saveProject:
        info.setInfo("Save", "Save the project", "File", 0);
        info.addDefaultKeypress('s', cmd);
        info.setActive(open);
        break;
    case CommandIds::saveProjectAs:
        info.setInfo("Save As...", "Save the project under a new name", "File", 0);
        info.addDefaultKeypress('s', cmd | juce::ModifierKeys::shiftModifier);
        info.setActive(open);
        break;
    case CommandIds::preferences:
        info.setInfo("Preferences...", "Open Preferences", "Edit", 0);
        info.addDefaultKeypress(',', cmd);
        break;
    case CommandIds::undo:
        info.setInfo(controller.undoLabel().isNotEmpty() ? "Undo " + controller.undoLabel() : juce::String("Undo"),
                     "Undo", "Edit", 0);
        info.addDefaultKeypress('z', cmd);
        info.setActive(open && controller.canUndo());
        break;
    case CommandIds::redo:
        info.setInfo(controller.redoLabel().isNotEmpty() ? "Redo " + controller.redoLabel() : juce::String("Redo"),
                     "Redo", "Edit", 0);
        info.addDefaultKeypress('z', cmd | juce::ModifierKeys::shiftModifier);
        info.addDefaultKeypress('y', cmd);
        info.setActive(open && controller.canRedo());
        break;
    case CommandIds::about:
        info.setInfo("About Stemcel", "About Stemcel", "Help", 0);
        break;
    // Single-key shortcuts (Space, S, D, M, Delete) are handled by the workspace so text
    // fields keep them; the menu items show them for discoverability.
    case CommandIds::playPause:
        info.setInfo("Play / Pause", "Play or pause", "Transport", 0);
        info.setActive(open);
        break;
    case CommandIds::splitAtPlayhead:
        info.setInfo("Split Clip at Playhead	S", "Split at the playhead", "Edit", 0);
        info.setActive(open);
        break;
    case CommandIds::duplicateClip:
        info.setInfo("Duplicate Clip	D", "Duplicate the selected clip", "Edit", 0);
        info.setActive(open && workspace != nullptr && workspace->hasClipSelection());
        break;
    case CommandIds::deleteClip:
        info.setInfo("Delete Clip	Del", "Delete the selected clips", "Edit", 0);
        info.setActive(open && workspace != nullptr && workspace->hasClipSelection());
        break;
    case CommandIds::toggleLock:
        info.setInfo("Lock / Unlock Clip", "Toggle the lock on the selected clips", "Edit", 0);
        info.addDefaultKeypress('l', cmd);
        info.setActive(open && workspace != nullptr && workspace->hasClipSelection());
        break;
    case CommandIds::addMarker:
        info.setInfo("Add Marker at Playhead	M", "Add or remove a marker at the playhead", "Edit", 0);
        info.setActive(open);
        break;
    case CommandIds::addTrack:
        info.setInfo("Add Track", "Add a track", "Track", 0);
        info.addDefaultKeypress('t', cmd);
        info.setActive(open);
        break;
    case CommandIds::zoomIn:
        info.setInfo("Zoom In", "Zoom in", "View", 0);
        info.addDefaultKeypress('=', cmd);
        info.setActive(open);
        break;
    case CommandIds::zoomOut:
        info.setInfo("Zoom Out", "Zoom out", "View", 0);
        info.addDefaultKeypress('-', cmd);
        info.setActive(open);
        break;
    case CommandIds::zoomFit:
        info.setInfo("Zoom to Fit", "Fit the arrangement in view", "View", 0);
        info.addDefaultKeypress('f', cmd);
        info.setActive(open);
        break;
    case CommandIds::cutClips:
        info.setInfo("Cut", "Cut the selected clips", "Edit", 0);
        info.addDefaultKeypress('x', cmd);
        info.setActive(open && workspace != nullptr && workspace->hasClipSelection());
        break;
    case CommandIds::copyClips:
        info.setInfo("Copy", "Copy the selected clips", "Edit", 0);
        info.addDefaultKeypress('c', cmd);
        info.setActive(open && workspace != nullptr && workspace->hasClipSelection());
        break;
    case CommandIds::pasteClips:
        info.setInfo("Paste", "Paste clips at the playhead on the selected track", "Edit", 0);
        info.addDefaultKeypress('v', cmd);
        info.setActive(open && workspace != nullptr && workspace->timelineActions().hasClipboard());
        break;
    case CommandIds::importFromProject:
        info.setInfo("Import from Project...", "Bring stems, samples and recordings from another project", "File", 0);
        info.setActive(open);
        break;
    case CommandIds::recordAudio:
        info.setInfo("Record Audio...", "Record from an input onto the timeline or into the library", "Track", 0);
        info.addDefaultKeypress('r', cmd);
        info.setActive(open);
        break;
    case CommandIds::projectProperties:
        info.setInfo("Project Properties...", "Name, tempo, length, sample rate and output", "File", 0);
        info.setActive(open);
        break;
    case CommandIds::exportMixdown:
        info.setInfo("Export Mixdown...", "Render the project to an audio file", "File", 0);
        info.addDefaultKeypress('e', cmd);
        info.setActive(open);
        break;
    case CommandIds::importAudio:
        info.setInfo("Import to Library...", "Import audio files into the library", "File", 0);
        info.addDefaultKeypress('i', cmd);
        info.setActive(open);
        break;
    case CommandIds::showLibraryTab:
        info.setInfo("Library", "Show the project library", "View", 0);
        info.addDefaultKeypress('1', cmd);
        info.setActive(open);
        break;
    case CommandIds::showFilesTab:
        info.setInfo("Files", "Show the file browser", "View", 0);
        info.addDefaultKeypress('2', cmd);
        info.setActive(open);
        break;
    case CommandIds::showRekordboxTab:
        info.setInfo("rekordbox Library", "Show the rekordbox library", "View", 0);
        info.addDefaultKeypress('3', cmd);
        info.setActive(open);
        break;
    case CommandIds::toggleMixer:
        info.setInfo("Mixer", "Show or hide the mixer", "View", 0);
        info.addDefaultKeypress(juce::KeyPress::F9Key, 0);
        info.setActive(open);
        info.setTicked(workspace != nullptr && workspace->isMixerShown());
        break;
    case CommandIds::selectAllClips:
        info.setInfo("Select All Clips", "Select every clip", "Edit", 0);
        info.addDefaultKeypress('a', cmd);
        info.setActive(open);
        break;
    case CommandIds::joinClips:
        info.setInfo("Join Clips", "Join selected clips split from one another", "Edit", 0);
        info.addDefaultKeypress('j', cmd);
        info.setActive(open && workspace != nullptr && workspace->hasClipSelection());
        break;
    case CommandIds::openClipEditor:
        info.setInfo("Open Clip Editor\tE", "Open the selected clip in the clip editor", "Edit", 0);
        info.setActive(open && workspace != nullptr && workspace->hasClipSelection());
        break;
    case CommandIds::toggleAutomation:
        info.setInfo("Show / Hide Automation\tA", "Show or hide the selected track's automation lanes", "Track", 0);
        info.setActive(open);
        break;
    case CommandIds::toggleLoop:
        info.setInfo("Loop Selection\tC", "Loop the timeline selection", "View", 0);
        info.setActive(open && workspace != nullptr && workspace->projectMirror().model().timelineSelection.has_value());
        info.setTicked(workspace != nullptr && workspace->projectMirror().model().loopSelection);
        break;
    case CommandIds::toggleMetronome:
        info.setInfo("Metronome\tK", "Click on every beat", "View", 0);
        info.setActive(open);
        info.setTicked(workspace != nullptr
                       && static_cast<bool>(workspace->projectMirror().model().raw.getProperty("metronomeEnabled", false)));
        break;
    case CommandIds::toggleSnap:
        info.setInfo("Snap to Grid\tN", "Turn snapping on or off", "View", 0);
        info.setActive(open);
        info.setTicked(workspace != nullptr && workspace->timelineActions().snapGrid() != "free");
        break;
    default:
        break;
    }
}

bool MainComponent::perform(const InvocationInfo& info)
{
    switch (info.commandID)
    {
    case CommandIds::newProject: controller.newProject(); return true;
    case CommandIds::openProject: controller.openProjectDialog(); return true;
    case CommandIds::saveProject: controller.save(); return true;
    case CommandIds::saveProjectAs: controller.saveAs(); return true;
    case CommandIds::preferences: PreferencesDialog::show(engine, prefs); return true;
    case CommandIds::undo: controller.undo(); return true;
    case CommandIds::playPause: if (workspace != nullptr) workspace->togglePlay(); return true;
    case CommandIds::splitAtPlayhead: if (workspace != nullptr) workspace->splitAtPlayhead(); return true;
    case CommandIds::duplicateClip: if (workspace != nullptr) workspace->duplicateSelection(); return true;
    case CommandIds::deleteClip: if (workspace != nullptr) workspace->deleteSelection(); return true;
    case CommandIds::toggleLock: if (workspace != nullptr) workspace->toggleLockSelection(); return true;
    case CommandIds::addMarker: if (workspace != nullptr) workspace->addMarkerAtPlayhead(); return true;
    case CommandIds::addTrack: if (workspace != nullptr) workspace->addTrack(); return true;
    case CommandIds::zoomIn: if (workspace != nullptr) workspace->zoom(1.25); return true;
    case CommandIds::zoomOut: if (workspace != nullptr) workspace->zoom(0.8); return true;
    case CommandIds::zoomFit: if (workspace != nullptr) workspace->zoomToFit(); return true;
    case CommandIds::cutClips: if (workspace != nullptr) workspace->cutSelection(); return true;
    case CommandIds::copyClips: if (workspace != nullptr) workspace->copySelection(); return true;
    case CommandIds::pasteClips: if (workspace != nullptr) workspace->pasteAtPlayhead(); return true;
    case CommandIds::importFromProject:
        ProjectImportDialog::chooseAndShow(engine, [this](const juce::String& m, bool e) { toasts.show(m, e); });
        return true;
    case CommandIds::recordAudio:
        if (workspace != nullptr)
            RecordDialog::show(engine, workspace->projectMirror(), workspace->timelineActions(), workspace->viewState(), prefs);
        return true;
    case CommandIds::projectProperties:
        if (workspace != nullptr) ProjectPropertiesDialog::show(engine, prefs, workspace->projectMirror().model());
        return true;
    case CommandIds::exportMixdown:
        if (workspace != nullptr)
            ExportDialog::show(engine, prefs, workspace->projectMirror().model(),
                               [this](const juce::String& m, bool e) { toasts.show(m, e); });
        return true;
    case CommandIds::importAudio: if (workspace != nullptr) workspace->importWithDialog(); return true;
    case CommandIds::showLibraryTab: if (workspace != nullptr) workspace->showLibraryTab(0); return true;
    case CommandIds::showFilesTab: if (workspace != nullptr) workspace->showLibraryTab(1); return true;
    case CommandIds::showRekordboxTab: if (workspace != nullptr) workspace->showLibraryTab(2); return true;
    case CommandIds::toggleMixer: if (workspace != nullptr) workspace->toggleMixer(); return true;
    case CommandIds::selectAllClips: if (workspace != nullptr) workspace->selectAllClips(); return true;
    case CommandIds::joinClips: if (workspace != nullptr) workspace->joinSelection(); return true;
    case CommandIds::toggleAutomation: if (workspace != nullptr) workspace->toggleAutomationOnSelectedTrack(); return true;
    case CommandIds::toggleLoop: if (workspace != nullptr) workspace->toggleLoop(); return true;
    case CommandIds::toggleMetronome: if (workspace != nullptr) workspace->toggleMetronome(); return true;
    case CommandIds::toggleSnap: if (workspace != nullptr) workspace->toggleSnap(); return true;
    case CommandIds::openClipEditor: if (workspace != nullptr) workspace->openClipEditorOnSelection(); return true;
    case CommandIds::redo: controller.redo(); return true;
    case CommandIds::about:
        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                         .withIconType(juce::MessageBoxIconType::InfoIcon)
                                         .withTitle("Stemcel")
                                         .withMessage(juce::String("Stemcel ") + kBackendVersion
                                                      + "\nA remix-first DAW.\n\nNative build.")
                                         .withButton("OK")
                                         .withAssociatedComponent(this),
                                     nullptr);
        return true;
    default: return false;
    }
}

juce::StringArray MainComponent::getMenuBarNames()
{
    return {"File", "Edit", "View", "Track", "Help"};
}

juce::PopupMenu MainComponent::getMenuForIndex(int topLevelMenuIndex, const juce::String&)
{
    juce::PopupMenu menu;
    if (topLevelMenuIndex == 0)
    {
        menu.addCommandItem(&commandManager, CommandIds::newProject);
        menu.addCommandItem(&commandManager, CommandIds::openProject);
        juce::PopupMenu recent;
        const auto projects = prefs.recentProjects();
        for (int i = 0; i < static_cast<int>(projects.size()); ++i)
        {
            const auto& p = projects[static_cast<size_t>(i)];
            recent.addItem(kRecentMenuBase + i, p.name.isNotEmpty() ? p.name : juce::File(p.path).getFileName(),
                           engineReady);
        }
        if (!projects.empty())
        {
            recent.addSeparator();
            recent.addItem(kClearRecentMenuId, "Clear Recent Projects");
        }
        menu.addSubMenu("Open Recent", recent, !projects.empty());
        menu.addSeparator();
        menu.addCommandItem(&commandManager, CommandIds::importAudio);
        menu.addCommandItem(&commandManager, CommandIds::importFromProject);
        menu.addCommandItem(&commandManager, CommandIds::exportMixdown);
        menu.addCommandItem(&commandManager, CommandIds::projectProperties);
        menu.addSeparator();
        menu.addCommandItem(&commandManager, CommandIds::saveProject);
        menu.addCommandItem(&commandManager, CommandIds::saveProjectAs);
#if !JUCE_MAC
        menu.addSeparator();
        menu.addCommandItem(&commandManager, CommandIds::preferences);
        menu.addSeparator();
        menu.addCommandItem(&commandManager, juce::StandardApplicationCommandIDs::quit);
#endif
    }
    else if (topLevelMenuIndex == 1)
    {
        menu.addCommandItem(&commandManager, CommandIds::undo);
        menu.addCommandItem(&commandManager, CommandIds::redo);
        menu.addSeparator();
        menu.addCommandItem(&commandManager, CommandIds::cutClips);
        menu.addCommandItem(&commandManager, CommandIds::copyClips);
        menu.addCommandItem(&commandManager, CommandIds::pasteClips);
        menu.addCommandItem(&commandManager, CommandIds::selectAllClips);
        menu.addSeparator();
        menu.addCommandItem(&commandManager, CommandIds::splitAtPlayhead);
        menu.addCommandItem(&commandManager, CommandIds::joinClips);
        menu.addCommandItem(&commandManager, CommandIds::duplicateClip);
        menu.addCommandItem(&commandManager, CommandIds::deleteClip);
        menu.addCommandItem(&commandManager, CommandIds::toggleLock);
        menu.addCommandItem(&commandManager, CommandIds::openClipEditor);
        menu.addSeparator();
        menu.addCommandItem(&commandManager, CommandIds::addMarker);
    }
    else if (topLevelMenuIndex == 2)
    {
        menu.addCommandItem(&commandManager, CommandIds::zoomIn);
        menu.addCommandItem(&commandManager, CommandIds::zoomOut);
        menu.addCommandItem(&commandManager, CommandIds::zoomFit);
        menu.addSeparator();
        menu.addCommandItem(&commandManager, CommandIds::toggleSnap);
        menu.addCommandItem(&commandManager, CommandIds::toggleLoop);
        menu.addCommandItem(&commandManager, CommandIds::toggleMetronome);
        menu.addSeparator();
        menu.addCommandItem(&commandManager, CommandIds::showLibraryTab);
        menu.addCommandItem(&commandManager, CommandIds::showFilesTab);
        menu.addCommandItem(&commandManager, CommandIds::showRekordboxTab);
        menu.addSeparator();
        menu.addCommandItem(&commandManager, CommandIds::toggleMixer);
    }
    else if (topLevelMenuIndex == 3)
    {
        menu.addCommandItem(&commandManager, CommandIds::addTrack);
        menu.addCommandItem(&commandManager, CommandIds::recordAudio);
        menu.addCommandItem(&commandManager, CommandIds::toggleAutomation);
    }
    else if (topLevelMenuIndex == 4)
    {
#if !JUCE_MAC
        menu.addCommandItem(&commandManager, CommandIds::about);
#endif
        menu.addItem(9001, "Stemcel Documentation");
    }
    return menu;
}

void MainComponent::menuItemSelected(int menuItemID, int)
{
    if (menuItemID == kClearRecentMenuId)
    {
        prefs.clearRecentProjects();
        prefs.save();
        stateChanged();
        return;
    }
    if (menuItemID >= kRecentMenuBase && menuItemID < kClearRecentMenuId)
    {
        const auto projects = prefs.recentProjects();
        const auto index = static_cast<size_t>(menuItemID - kRecentMenuBase);
        if (index < projects.size())
        {
            const juce::File file(projects[index].path);
            controller.guardUnsavedChanges([this, file] { controller.openProject(file); });
        }
        return;
    }
    if (menuItemID == 9001) juce::URL("https://github.com/FrasierGH/stemcel/tree/main/docs").launchInDefaultBrowser();
}

juce::PopupMenu MainComponent::macAppMenuExtras()
{
    juce::PopupMenu extras;
    extras.addCommandItem(&commandManager, CommandIds::about);
    extras.addSeparator();
    extras.addCommandItem(&commandManager, CommandIds::preferences);
    return extras;
}

} // namespace stemcel::native
