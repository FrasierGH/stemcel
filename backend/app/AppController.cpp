#include "AppController.h"
#include "AppPaths.h"

#include "Log.h"
#include "MediaStore.h"

#include <utility>

namespace stemcel::native
{
namespace
{
juce::var filePathPayload(const juce::File& file)
{
    auto* p = new juce::DynamicObject();
    p->setProperty("filePath", file.getFullPathName());
    return juce::var(p);
}
} // namespace

AppController::AppController(EngineClient& engineClient, AppPreferences& preferences)
    : engine(engineClient), prefs(preferences), subscriptions(engineClient), autosave(engineClient, preferences)
{
    subscriptions.on("PROJECT_STATE",
                     [this](const juce::var& p)
                     {
                         path = p.getProperty("filePath", {}).toString();
                         name = p.getProperty("name", {}).toString();
                         dirty = static_cast<bool>(p.getProperty("dirty", false));
                         awaitingTabState = false; // this is the project the active tab now holds
                         changed();
                     });
    subscriptions.on("PROJECT_DIRTY",
                     [this](const juce::var& p)
                     {
                         dirty = static_cast<bool>(p.getProperty("dirty", false));
                         changed();
                     });
    subscriptions.on("EDIT_UNDO_STATE",
                     [this](const juce::var& p)
                     {
                         undoAvailable = static_cast<bool>(p.getProperty("canUndo", false));
                         redoAvailable = static_cast<bool>(p.getProperty("canRedo", false));
                         undoText = p.getProperty("undoLabel", {}).toString();
                         redoText = p.getProperty("redoLabel", {}).toString();
                         changed();
                     });
    subscriptions.on("PROJECT_SAVED",
                     [this](const juce::var& p)
                     {
                         const bool ok = static_cast<bool>(p.getProperty("ok", false));
                         const bool quiet = std::exchange(quietSave, false);
                         if (ok)
                         {
                             path = p.getProperty("filePath", path).toString();
                             prefs.addRecentProject(path, name.isNotEmpty() ? name
                                                                            : juce::File(path).getFileNameWithoutExtension());
                             prefs.save();
                             if (!quiet) notify("Saved " + juce::File(path).getFileName(), false);
                         }
                         else
                         {
                             notify("Save failed: " + p.getProperty("error", "unknown error").toString(), true);
                         }
                         if (auto done = std::exchange(pendingSave, nullptr)) done(ok);
                         changed();
                     });
    startTimer(kSeamlessSaveSeconds * 1000);
    subscriptions.on("PROJECT_LOAD_FAILED",
                     [this](const juce::var& p)
                     {
                         const auto failed = p.getProperty("filePath", {}).toString();
                         notify("Could not open " + juce::File(failed).getFileName() + ": "
                                    + p.getProperty("error", "unknown error").toString(),
                                true);
                     });
}

void AppController::changed()
{
    // Until a switched-to tab's project reports in, name/path/dirty still describe the last one.
    if (!awaitingTabState) syncActiveTab();
    if (onStateChanged) onStateChanged();
}

void AppController::notify(const juce::String& message, bool isError)
{
    if (onNotify) onNotify(message, isError);
}

juce::File AppController::projectFolderPath(const juce::File& chosen)
{
    const auto withExt = chosen.hasFileExtension("stemcel;legacydaw") ? chosen : chosen.withFileExtension("stemcel");
    const auto projectName = withExt.getFileNameWithoutExtension();
    if (projectName.isEmpty()) return withExt;
    const auto parent = withExt.getParentDirectory();
    if (parent.getFileName().equalsIgnoreCase(projectName)) return withExt;
    return parent.getChildFile(projectName).getChildFile(projectName + ".stemcel");
}

void AppController::newProject()
{
    // A new mix gets its own tab; the open one is parked as it is.
    if (workspaceOpen) parkActive();
    addTab();
    awaitingTabState = true;
    engine.send("PROJECT_NEW");
    workspaceOpen = true;
    changed();
}

void AppController::openProjectDialog()
{
    // Opens in a tab of its own, so nothing open needs saving first.
    chooser = std::make_unique<juce::FileChooser>("Open Stemcel Project", prefs.defaultProjectDirectory(), "*.stemcel;*.legacydaw");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                         [this](const juce::FileChooser& fc)
                         {
                             const auto file = fc.getResult();
                             if (file.existsAsFile()) openProject(file);
                         });
}

void AppController::openProject(const juce::File& file)
{
    if (!file.existsAsFile())
    {
        notify("Project not found: " + file.getFullPathName(), true);
        prefs.removeRecentProject(file.getFullPathName());
        prefs.save();
        changed();
        return;
    }
    // Already open in a tab: go there.
    for (int i = 0; i < static_cast<int>(tabs.size()); ++i)
        if (juce::File(tabs[static_cast<size_t>(i)].path) == file)
        {
            switchToTab(i);
            return;
        }
    if (workspaceOpen) parkActive();
    // The startup screen's first project, or a fresh untitled tab, is replaced rather than kept.
    const bool reuse = activeTab >= 0 && tabs[static_cast<size_t>(activeTab)].path.isEmpty()
                       && !tabs[static_cast<size_t>(activeTab)].dirty && !tabs[static_cast<size_t>(activeTab)].parked.existsAsFile();
    if (!reuse) addTab();
    awaitingTabState = true;
    engine.send("PROJECT_LOAD", filePathPayload(file));
    // PROJECT_LOAD_FAILED reports a failure; success arrives as a reset PROJECT_STATE.
    workspaceOpen = true;
    tabs[static_cast<size_t>(activeTab)].path = file.getFullPathName();
    tabs[static_cast<size_t>(activeTab)].name = file.getFileNameWithoutExtension();
    prefs.addRecentProject(file.getFullPathName(), file.getFileNameWithoutExtension());
    prefs.save();
    changed();
}

void AppController::restoreAutosave(const Autosave::Entry& entry)
{
    if (workspaceOpen) parkActive();
    if (workspaceOpen || tabs.empty()) addTab();
    autosave.restore(entry);
    workspaceOpen = true;
    changed();
}

void AppController::save(std::function<void(bool)> done)
{
    if (path.isEmpty())
    {
        saveAs(std::move(done));
        return;
    }
    pendingSave = std::move(done);
    engine.send("PROJECT_SAVE");
}

void AppController::timerCallback()
{
    if (!static_cast<bool>(prefs.get("autosave", "saveProjectFile", true)) || !workspaceOpen || !dirty || path.isEmpty())
        return;
    // Not in the middle of something: a drag, a dialog, or a save already on its way.
    const bool busy = juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown() || pendingSave != nullptr || quietSave
                      || juce::ModalComponentManager::getInstance()->getNumModalComponents() > 0;
    if (busy)
    {
        startTimer(10 * 1000);
        return;
    }
    startTimer(kSeamlessSaveSeconds * 1000);
    quietSave = true;
    if (saveViewState) saveViewState();
    engine.send("PROJECT_SAVE");
}

void AppController::saveAs(std::function<void(bool)> done)
{
    const auto suggested = name.isNotEmpty() ? name : juce::String("Untitled");
    chooser = std::make_unique<juce::FileChooser>(
        "Save Stemcel Project", prefs.defaultProjectDirectory().getChildFile(suggested + ".stemcel"),
        "*.stemcel");
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::warnAboutOverwriting,
                         [this, done = std::move(done)](const juce::FileChooser& fc) mutable
                         {
                             const auto chosen = fc.getResult();
                             if (chosen == juce::File())
                             {
                                 if (done) done(false);
                                 return;
                             }
                             const auto target = projectFolderPath(chosen);
                             if (!target.getParentDirectory().createDirectory().wasOk())
                             {
                                 notify("Can't save here: " + target.getParentDirectory().getFullPathName(), true);
                                 if (done) done(false);
                                 return;
                             }
                             pendingSave = std::move(done);
                             // Carry the media store (tags + covers) to the new project folder.
                             media::migrate(media::rootFor(path), media::rootFor(target.getFullPathName()));
                             engine.send("PROJECT_SAVE_AS", filePathPayload(target));
                         });
}

void AppController::guardUnsavedChanges(std::function<void()> proceed)
{
    if (!dirty || !workspaceOpen)
    {
        // Nothing to save, but the zoom and scroll go into the file so the project reopens
        // where it was left (the renderer's persistCleanViewState).
        if (workspaceOpen && saveViewState) saveViewState();
        proceed();
        return;
    }

    const auto options = juce::MessageBoxOptions()
                             .withIconType(juce::MessageBoxIconType::WarningIcon)
                             .withTitle("Unsaved changes")
                             .withMessage("Save changes to \"" + (name.isNotEmpty() ? name : juce::String("Untitled"))
                                          + "\" before continuing?")
                             .withButton("Save")
                             .withButton("Don't Save")
                             .withButton("Cancel")
                             .withAssociatedComponent(dialogParent);
    juce::AlertWindow::showAsync(options,
                                 [this, proceed = std::move(proceed)](int result)
                                 {
                                     if (result == 1)
                                         save([proceed](bool saved) { if (saved) proceed(); });
                                     else if (result == 2)
                                         proceed();
                                 });
}

void AppController::requestQuit()
{
    // Other tabs' unsaved changes are only parked: say so before they go.
    int others = 0;
    for (int i = 0; i < static_cast<int>(tabs.size()); ++i)
        if (i != activeTab && tabs[static_cast<size_t>(i)].dirty) ++others;
    // Parked tabs go with a deliberate quit; after a crash they stay, ordinary projects to open.
    const auto quit = [this]
    {
        guardUnsavedChanges([]
                            {
                                for (const auto& f : tabStore().findChildFiles(juce::File::findFiles, false, "*.stemcel;*.legacydaw"))
                                    f.deleteFile();
                                juce::JUCEApplicationBase::quit();
                            });
    };
    if (others == 0)
    {
        quit();
        return;
    }
    juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                     .withIconType(juce::MessageBoxIconType::WarningIcon)
                                     .withTitle("Unsaved tabs")
                                     .withMessage(juce::String(others) + (others == 1 ? " other tab has" : " other tabs have")
                                                  + " unsaved changes. Quit and lose them?")
                                     .withButton("Quit")
                                     .withButton("Cancel")
                                     .withAssociatedComponent(dialogParent),
                                 [quit](int result) { if (result == 1) quit(); });
}

juce::File AppController::tabStore()
{
    return juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(appFolderName()).getChildFile("Tabs");
}

void AppController::addTab()
{
    Tab tab;
    tab.id = juce::Uuid().toDashedString();
    tab.name = "Untitled";
    tabs.push_back(tab);
    activeTab = static_cast<int>(tabs.size()) - 1;
}

void AppController::syncActiveTab()
{
    if (activeTab < 0 || activeTab >= static_cast<int>(tabs.size())) return;
    auto& tab = tabs[static_cast<size_t>(activeTab)];
    tab.name = name.isNotEmpty() ? name : juce::String("Untitled");
    tab.path = path;
    tab.dirty = dirty;
}

void AppController::parkActive()
{
    if (activeTab < 0 || activeTab >= static_cast<int>(tabs.size())) return;
    syncActiveTab();
    auto& tab = tabs[static_cast<size_t>(activeTab)];
    if (dirty)
    {
        // The project as it stands, unsaved, for when this tab is opened again.
        tabStore().createDirectory();
        tab.parked = tabStore().getChildFile(tab.id + ".stemcel");
        auto* p = new juce::DynamicObject();
        p->setProperty("filePath", tab.parked.getFullPathName());
        engine.send("PROJECT_AUTOSAVE", juce::var(p));
    }
    else
    {
        tab.parked.deleteFile();
        tab.parked = juce::File();
        if (saveViewState) saveViewState(); // reopens where it was left
    }
}

void AppController::switchToTab(int index)
{
    if (index < 0 || index >= static_cast<int>(tabs.size()) || index == activeTab) return;
    parkActive();
    activeTab = index;
    awaitingTabState = true;
    auto& tab = tabs[static_cast<size_t>(index)];
    if (tab.parked.existsAsFile())
    {
        // Restored unsaved, saving back to its own file (if it has one).
        auto* p = new juce::DynamicObject();
        p->setProperty("autosavePath", tab.parked.getFullPathName());
        p->setProperty("originalPath", tab.path.isNotEmpty() ? juce::var(tab.path) : juce::var());
        engine.send("PROJECT_LOAD_RECOVERY", juce::var(p));
        tab.parked.deleteFile();
        tab.parked = juce::File();
    }
    else if (tab.path.isNotEmpty())
        engine.send("PROJECT_LOAD", filePathPayload(juce::File(tab.path)));
    else
        engine.send("PROJECT_NEW");
    changed();
}

void AppController::closeTab(int index)
{
    if (index < 0 || index >= static_cast<int>(tabs.size())) return;
    const auto remove = [this, index]
    {
        tabs[static_cast<size_t>(index)].parked.deleteFile();
        const bool wasActive = index == activeTab;
        tabs.erase(tabs.begin() + index);
        if (tabs.empty())
        {
            // The last one: an empty untitled mix stays open in its place.
            activeTab = -1;
            addTab();
            awaitingTabState = true;
            engine.send("PROJECT_NEW");
        }
        else if (wasActive)
        {
            activeTab = -1; // nothing to park: its project is the one being closed
            const int next = juce::jmin(index, static_cast<int>(tabs.size()) - 1);
            activeTab = next;
            awaitingTabState = true;
            auto& tab = tabs[static_cast<size_t>(next)];
            if (tab.parked.existsAsFile())
            {
                auto* p = new juce::DynamicObject();
                p->setProperty("autosavePath", tab.parked.getFullPathName());
                p->setProperty("originalPath", tab.path.isNotEmpty() ? juce::var(tab.path) : juce::var());
                engine.send("PROJECT_LOAD_RECOVERY", juce::var(p));
                tab.parked.deleteFile();
                tab.parked = juce::File();
            }
            else if (tab.path.isNotEmpty())
                engine.send("PROJECT_LOAD", filePathPayload(juce::File(tab.path)));
            else
                engine.send("PROJECT_NEW");
        }
        else if (index < activeTab)
            --activeTab;
        changed();
    };
    if (index == activeTab)
    {
        guardUnsavedChanges(remove);
        return;
    }
    if (!tabs[static_cast<size_t>(index)].dirty)
    {
        remove();
        return;
    }
    juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                     .withIconType(juce::MessageBoxIconType::WarningIcon)
                                     .withTitle("Close tab")
                                     .withMessage("Close \"" + tabs[static_cast<size_t>(index)].name + "\" and lose its unsaved changes?")
                                     .withButton("Close")
                                     .withButton("Cancel")
                                     .withAssociatedComponent(dialogParent),
                                 [remove](int result) { if (result == 1) remove(); });
}

void AppController::undo()
{
    if (undoAvailable) engine.send("EDIT_UNDO");
}

void AppController::redo()
{
    if (redoAvailable) engine.send("EDIT_REDO");
}

} // namespace stemcel::native
