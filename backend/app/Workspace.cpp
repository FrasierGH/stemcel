#include "Workspace.h"

#include "AutoMix.h"
#include "ClipEditor.h"
#include "ProjectDialogs.h"
#include "ScratchEditor.h"
#include "Theme.h"

namespace stemcel::native
{

Workspace::Workspace(EngineClient& engineClient, AppController& appController, AppPreferences& preferences,
                     std::function<void(const juce::String&, bool)> notify)
    : engine(engineClient), controller(appController), prefs(preferences), mirror(engineClient), live(engineClient),
      actions(engineClient, mirror), waveforms(engineClient.host().peaksCache()), subscriptions(engineClient),
      importer(engineClient, mirror, sharedRekordbox()), audition(engineClient, importer),
      stems(engineClient, mirror, actions, preferences), sectionAnalyser(engineClient, mirror, preferences),
      transport(mirror, view, live, actions, preferences), headers(mirror, view, live, actions),
      timeline(mirror, view, live, actions, waveforms)
{
    // A loaded or new project brings its own zoom, scroll and selected track.
    subscriptions.on("PROJECT_STATE",
                     [this](const juce::var& p)
                     {
                         if (!static_cast<bool>(p.getProperty("reset", false))) return;
                         const double px = static_cast<double>(p.getProperty("viewPxPerSecond", 0.0));
                         view.pxPerSecond = px > 0.0 ? px : TimelineViewState::kDefaultPxPerSecond;
                         view.scrollXPx = static_cast<double>(p.getProperty("viewScrollX", 0.0));
                         view.scrollY = 0.0;
                         view.selectedTrackId = p.getProperty("viewSelectedTrack", {}).toString();
                         view.selectedClipIds.clear();
                         view.changed();
                         // A loaded project with missing audio offers to relink it.
                         if (!juce::JUCEApplicationBase::getCommandLineParameters().contains("--smoke-test"))
                             juce::MessageManager::callAsync([this]
                                                             {
                                                                 mirror.refreshNow();
                                                                 RelinkDialog::showIfNeeded(engine, mirror);
                                                             });
                     });
    actions.onNotify = notify;
    importer.onNotify = notify;
    stems.onNotify = notify;
    sectionAnalyser.onNotify = notify;
    timeline.onAnalyseSections = [this](const juce::String& itemId) { sectionAnalyser.analyse(itemId, true); };
    stems.onProgress = [this](double percent, const juce::String& stage)
    {
        if (libraryPanel == nullptr) return;
        libraryPanel->setActivity(percent < 0.0 ? juce::String()
                                                : "Separating stems: " + juce::String(juce::roundToInt(percent)) + "% (" + stage + ")");
    };
    timeline.onSeparateStems = [this](const juce::String& clipId)
    {
        if (const auto* c = mirror.model().findClip(clipId)) stems.openDialog(c->libraryItemId, clipId);
    };
    timeline.onSplitChannels = [this](const juce::String& clipId, const juce::StringArray& channels) { stems.splitChannels(clipId, channels); };
    timeline.onOpenScratch = [this, notify](const juce::String& clipId) { ScratchEditor::show(engine, mirror, waveforms, clipId, {}, notify); };
    timeline.onOpenClipEditor = [this](const juce::String& clipId) { ClipEditor::show(engine, mirror, actions, waveforms, clipId); };
    timeline.onImportDrop = [this](std::vector<ImportRequest> requests, const juce::String& trackId, double ms)
    { importAndPlace(std::move(requests), trackId, ms); };

    mashup = std::make_unique<MashupBuilder>(engine, mirror, actions, stems);
    mashup->onNotify = notify;
    mashup->onPlaced = [this](double from, double to, const juce::String& trackId) { timeline.revealRange(from, to, trackId); };
    auto panel = std::make_unique<LibraryPanel>(LibraryContext{engine, mirror, actions, view, live, prefs, importer,
                                                               sharedRekordbox(), audition, notify,
                                                               [this](const juce::String& itemId) { stems.openDialog(itemId, {}); },
                                                               [this, notify](const juce::String& itemId)
                                                               { ScratchEditor::show(engine, mirror, waveforms, {}, itemId, notify); },
                                                               [this](const juce::String& itemId)
                                                               { ClipEditor::showLibraryItem(engine, mirror, actions, waveforms, itemId); },
                                                               [this, notify](std::vector<juce::String> ids, bool reorder, const juce::String& style, bool matchKeys)
                                                               {
                                                                   const double before = mirror.model().contentEndMs();
                                                                   notify(automix::build(engine, mirror, actions, waveforms, std::move(ids), reorder, style, matchKeys), false);
                                                                   mirror.refreshNow();
                                                                   // Show the new mix, from its first deck down.
                                                                   const auto& tracks = mirror.model().tracks;
                                                                   if (tracks.size() >= 2 && mirror.model().contentEndMs() > before)
                                                                       timeline.revealRange(before, mirror.model().contentEndMs(), tracks[tracks.size() - 2].id);
                                                               },
                                                               [this](const juce::String& vocal, const juce::String& beat) { mashup->start(vocal, beat); },
                                                               [this](const juce::String& itemId) { sectionAnalyser.analyse(itemId, true); }});
    libraryPanel = panel.get();
    setBottomPanel(std::move(panel),
                   juce::jlimit(120, 700, static_cast<int>(prefs.get("ui", "nativeLibraryHeight", LibraryPanel::kDefaultHeight))));
    view.followPlayback = prefs.followPlayback();
    prefsWatcher.fn = [this]
    {
        view.stereoWaveforms = prefs.get("ui", "waveformDisplayMode", "stereo").toString() != "summary";
        view.skipToMarkers = prefs.get("ui", "skipButtonTarget", "markers").toString() == "markers";
        actions.autoWarpOnDrop = static_cast<bool>(prefs.get("ui", "matchProjectTempoOnDrop", true));
        importer.defaultProjectSampleRate = static_cast<int>(prefs.get("ui", "defaultProjectSampleRate", 44100)) == 48000 ? 48000 : 44100;
        view.changed();
    };
    prefsWatcher.fn();
    prefs.addChangeListener(&prefsWatcher);
    view.selectedTrackId = mirror.model().selectedTrackId;
    transport.setTimeline(&timeline);
    transport.onToggleMixer = [this] { toggleMixer(); };
    addAndMakeVisible(transport);
    addAndMakeVisible(headers);
    addAndMakeVisible(timeline);
    setWantsKeyboardFocus(true);

    MidiControllerActions::Hooks hooks;
    hooks.togglePlay = [this] { togglePlay(); };
    hooks.reveal = [this](double ms) { timeline.reveal(ms); };
    hooks.revealTrack = [this](const juce::String& trackId)
    {
        for (const auto& row : layoutRows(mirror.model()))
        {
            if (row.trackId != trackId) continue;
            const double visible = juce::jmax(0, timeline.getHeight() - view.headerHeight());
            if (row.top < view.scrollY) view.scrollY = row.top;
            else if (row.top + row.height > view.scrollY + visible) view.scrollY = row.top + row.height - visible;
            view.changed();
            return;
        }
    };
    hooks.zoom = [this](double factor) { zoom(factor); };
    hooks.showTrackFx = [this] { showLibraryTab(4); };
    hooks.blocked = [] { return juce::ModalComponentManager::getInstance()->getNumModalComponents() > 0; };
    hooks.cueScratch = [] { return ScratchEditor::cueBuildBacking(); };
    midi = std::make_unique<MidiControllerActions>(engine, mirror, actions, view, live, prefs, std::move(hooks));

    toast = notify;
    // Preferences > Timeline "Align clips to the beat grid after analysis".
    // Both handlers below refresh the mirror, so they run after the current message: these
    // events can arrive in the middle of another action's sends (the first clip added seeds
    // the project tempo), and rebuilding the model there would pull it out from under it.
    subscriptions.on("LIBRARY_ITEM_ANALYSIS",
                     [safe = juce::Component::SafePointer<Workspace>(this)](const juce::var& p)
                     {
                         juce::MessageManager::callAsync([safe, p] { if (safe != nullptr) safe->alignAfterAnalysis(p); });
                     });
    subscriptions.on("PROJECT_BPM_APPLIED",
                     [safe = juce::Component::SafePointer<Workspace>(this)](const juce::var&)
                     {
                         juce::MessageManager::callAsync([safe] { if (safe != nullptr) safe->alignAfterTempoSeed(); });
                     });
}

void Workspace::alignAfterAnalysis(const juce::var& p)
{
    if (static_cast<double>(p.getProperty("bpm", 0.0)) <= 0.0 || static_cast<bool>(p.getProperty("manual", false))) return;
    if (!static_cast<bool>(prefs.get("ui", "alignClipsToGridOnAnalysis", true))) return;
    const auto itemId = p.getProperty("itemId", {}).toString();
    mirror.refreshNow();
    const int blocked = actions.alignItemClipsToGrid(itemId);
    if (blocked > 0 && toast)
        toast(juce::String(blocked) + (blocked == 1 ? " clip" : " clips")
                  + " could not be aligned to the beat grid without overlapping; move it by hand.",
              false);
    // A first-clip tempo seed lands just after; align against it then too.
    gridAlignPending[itemId] = juce::Time::getMillisecondCounter();
}

void Workspace::alignAfterTempoSeed()
{
    const auto now = juce::Time::getMillisecondCounter();
    auto pending = std::exchange(gridAlignPending, {});
    if (!static_cast<bool>(prefs.get("ui", "alignClipsToGridOnAnalysis", true))) return;
    mirror.refreshNow();
    for (const auto& [itemId, at] : pending)
        if (now - at < 1500) actions.alignItemClipsToGrid(itemId);
}

Workspace::~Workspace()
{
    midi.reset();
    prefs.removeChangeListener(&prefsWatcher);
    audition.stop();
}

void Workspace::importWithDialog()
{
    if (libraryPanel != nullptr) libraryPanel->importWithDialog();
}

void Workspace::saveViewState()
{
    const auto file = mirror.model().filePath;
    if (file.isEmpty()) return;
    auto* p = new juce::DynamicObject();
    p->setProperty("filePath", file);
    p->setProperty("viewScrollX", view.scrollXPx);
    p->setProperty("viewPxPerSecond", view.pxPerSecond);
    engine.send("PROJECT_SAVE_VIEW_STATE", juce::var(p));
}

void Workspace::showLibraryTab(int index)
{
    if (libraryPanel != nullptr) libraryPanel->showTab(index);
}

void Workspace::importAndPlace(std::vector<ImportRequest> requests, const juce::String& trackId, double positionMs)
{
    importer.import(std::move(requests),
                    [this, trackId, positionMs](const juce::StringArray& itemIds)
                    {
                        // Runs inside the import's undo group, so import + placement undo together.
                        if (itemIds.isEmpty()) return;
                        auto target = trackId;
                        if (mirror.model().findTrack(target) == nullptr) target = actions.addTrack();
                        double at = positionMs;
                        std::set<juce::String> placed;
                        for (const auto& id : itemIds)
                        {
                            mirror.refreshNow();
                            const auto* item = mirror.model().findLibraryItem(id);
                            if (item == nullptr) continue;
                            const auto clipId = actions.addClip(target, *item, at);
                            mirror.refreshNow();
                            if (const auto* clip = mirror.model().findClip(clipId))
                            {
                                placed.insert(clipId);
                                at = clip->endMs();
                            }
                        }
                        view.selectedTrackId = target;
                        view.selectedClipIds = placed;
                        view.changed();
                    });
}

void Workspace::toggleMixer()
{
    if (mixer == nullptr)
    {
        mixer = std::make_unique<MixerPanel>(engine, mirror, actions, view, live, toast);
        addChildComponent(*mixer);
    }
    const bool show = !mixer->isVisible();
    mixer->setVisible(show);
    if (bottomPanel != nullptr) bottomPanel->setVisible(!show);
    transport.setMixerShown(show);
    resized();
}

void Workspace::setBottomPanel(std::unique_ptr<juce::Component> panel, int height)
{
    bottomPanel = std::move(panel);
    bottomPanelHeight = height;
    if (bottomPanel != nullptr) addAndMakeVisible(*bottomPanel);
    resized();
}

void Workspace::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
}

void Workspace::resized()
{
    auto area = getLocalBounds();
    transport.setBounds(area.removeFromTop(TransportBar::kHeight));
    if (isMixerShown())
    {
        mixer->setBounds(area.removeFromBottom(juce::jmax(bottomPanelHeight, 340)));
        area.removeFromBottom(1);
    }
    else if (bottomPanel != nullptr)
    {
        bottomPanel->setBounds(area.removeFromBottom(bottomPanelHeight));
        area.removeFromBottom(1);
    }
    const int headerWidth = static_cast<int>(prefs.get("ui", "trackHeaderWidth", TrackHeaders::kDefaultWidth));
    headers.setBounds(area.removeFromLeft(juce::jlimit(160, 320, headerWidth)));
    area.removeFromLeft(1);
    timeline.setBounds(area);
    timeline.clampScroll();
}

void Workspace::togglePlay()
{
    transport.togglePlay();
}

void Workspace::splitAtPlayhead()
{
    std::vector<juce::String> targets(view.selectedClipIds.begin(), view.selectedClipIds.end());
    if (targets.empty())
    {
        // No selection: split whatever is under the playhead on the selected track.
        if (const auto* track = mirror.model().findTrack(view.selectedTrackId))
            for (const auto& c : track->clips)
                if (live.positionMs > c.offsetMs && live.positionMs < c.endMs()) targets.push_back(c.id);
    }
    if (targets.empty()) return;
    engine.inUndoGroup("Split clips",
                       [&]
                       {
                           for (const auto& id : targets) actions.splitClipAt(id, live.positionMs);
                       });
}

void Workspace::duplicateSelection()
{
    std::set<juce::String> created;
    for (const auto& id : view.selectedClipIds)
        if (const auto copy = actions.duplicateClip(id); copy.isNotEmpty()) created.insert(copy);
    juce::ignoreUnused(created); // the source stays selected so repeated D keeps appending
}

void Workspace::deleteSelection()
{
    std::vector<juce::String> ids(view.selectedClipIds.begin(), view.selectedClipIds.end());
    actions.removeClips(ids);
    view.selectedClipIds.clear();
    view.changed();
}

void Workspace::copySelection()
{
    actions.copyClips({view.selectedClipIds.begin(), view.selectedClipIds.end()});
}

void Workspace::cutSelection()
{
    std::vector<juce::String> ids(view.selectedClipIds.begin(), view.selectedClipIds.end());
    if (actions.cutClips(ids))
    {
        view.selectedClipIds.clear();
        view.changed();
    }
}

void Workspace::pasteAtPlayhead()
{
    const auto created = actions.paste(view.selectedTrackId, actions.snap(live.positionMs));
    if (created.empty()) return;
    view.selectedClipIds = {created.begin(), created.end()};
    view.changed();
}

void Workspace::addMarkerAtPlayhead()
{
    // Toggle: a marker already at the playhead is removed instead (renderer behaviour).
    for (const auto& m : mirror.model().markers)
    {
        if (std::abs(m.positionMs - live.positionMs) < 1.0)
        {
            actions.removeMarker(m.id);
            return;
        }
    }
    actions.addMarker(live.positionMs);
}

void Workspace::toggleLockSelection()
{
    if (view.selectedClipIds.empty()) return;
    bool allLocked = true;
    for (const auto& id : view.selectedClipIds)
        if (const auto* c = mirror.model().findClip(id)) allLocked = allLocked && c->locked;
    engine.inUndoGroup(allLocked ? "Unlock clips" : "Lock clips",
                       [&]
                       {
                           for (const auto& id : view.selectedClipIds) actions.setClipLocked(id, !allLocked);
                       });
}

void Workspace::addTrack()
{
    view.selectedTrackId = actions.addTrack();
    view.changed();
}

void Workspace::zoom(double factor)
{
    timeline.zoomBy(factor);
}

void Workspace::zoomToFit()
{
    timeline.zoomToFit();
}

bool Workspace::keyPressed(const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    const int code = key.getKeyCode();
    const bool plain = !mods.isCommandDown() && !mods.isCtrlDown() && !mods.isAltDown();

    if (code == juce::KeyPress::spaceKey && plain && !mods.isShiftDown())
    {
        togglePlay();
        return true;
    }
    if (plain && !mods.isShiftDown())
    {
        switch (juce::CharacterFunctions::toLowerCase(key.getTextCharacter()))
        {
        case 's': splitAtPlayhead(); return true;
        case 'd': duplicateSelection(); return true;
        case 'm': addMarkerAtPlayhead(); return true;
        default: break;
        }
    }
    if ((mods.isCommandDown()) && juce::CharacterFunctions::toLowerCase(key.getTextCharacter()) == 'l')
    {
        toggleLockSelection();
        return true;
    }
    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey)
    {
        deleteSelection();
        return true;
    }
    if (code == juce::KeyPress::homeKey)
    {
        actions.seek(0.0);
        timeline.reveal(0.0);
        return true;
    }
    if (code == juce::KeyPress::endKey)
    {
        const double end = mirror.model().contentEndMs();
        actions.seek(end);
        timeline.reveal(end);
        return true;
    }
    if ((code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey) && !mods.isShiftDown())
    {
        const double step = actions.snapStepMs() > 0.0 ? actions.snapStepMs() : 500.0;
        const double target = actions.snap(live.positionMs + (code == juce::KeyPress::leftKey ? -step : step));
        actions.seek(target);
        timeline.reveal(target);
        return true;
    }
    if ((code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey) && mods.isShiftDown())
    {
        // Nudge selected clips by one grid step (Shift+Alt: 10 ms).
        const double step = mods.isAltDown() ? 10.0 : (actions.snapStepMs() > 0.0 ? actions.snapStepMs() : 10.0);
        const double delta = code == juce::KeyPress::leftKey ? -step : step;
        engine.inUndoGroup("Nudge clips",
                           [&]
                           {
                               for (const auto& id : view.selectedClipIds)
                                   if (const auto* c = mirror.model().findClip(id); c != nullptr && !c->locked)
                                       actions.moveClip(id, c->offsetMs + delta, c->trackId);
                           });
        return true;
    }
    return false;
}

} // namespace stemcel::native
