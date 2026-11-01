#include "Workspace.h"

#include "AutoMix.h"
#include "ClipEditor.h"
#include "ProjectDialogs.h"
#include "ScratchEditor.h"
#include "Theme.h"

#include <algorithm>

namespace stemcel::native
{

Workspace::Workspace(EngineClient& engineClient, AppController& appController, AppPreferences& preferences,
                     std::function<void(const juce::String&, bool)> notify)
    : engine(engineClient), controller(appController), prefs(preferences), mirror(engineClient), live(engineClient),
      actions(engineClient, mirror), waveforms(engineClient.host().peaksCache()), subscriptions(engineClient),
      importer(engineClient, mirror, sharedRekordbox()), audition(engineClient, importer),
      stems(engineClient, mirror, actions, preferences), sectionAnalyser(engineClient, mirror, preferences),
      tempoAnalyser(engineClient, mirror),
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
    tempoAnalyser.onNotify = notify;
    timeline.onTempoReport = [this](const juce::String& itemId, const juce::String& clipId)
    { TempoReport::show(mirror, actions, tempoAnalyser, itemId, clipId); };
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
                                                               [this](std::vector<juce::String> ids, bool reorder, const juce::String& style, bool matchKeys)
                                                               { runAutoMix(std::move(ids), reorder, style, matchKeys); },
                                                               [this](const juce::String& vocal, const juce::String& beat) { mashup->start(vocal, beat); },
                                                               [this](const juce::String& itemId) { sectionAnalyser.analyse(itemId, true); },
                                                               [this](const juce::String& itemId)
                                                               { TempoReport::show(mirror, actions, tempoAnalyser, itemId, {}); }});
    libraryPanel = panel.get();
    setBottomPanel(std::move(panel),
                   juce::jlimit(120, 1200, static_cast<int>(prefs.get("ui", "nativeLibraryHeight", LibraryPanel::kDefaultHeight))));
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
    setupResizeBars();
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

void Workspace::runAutoMix(std::vector<juce::String> ids, bool reorder, const juce::String& style, bool matchKeys)
{
    const double before = mirror.model().contentEndMs();
    const auto said = automix::build(engine, mirror, actions, waveforms, std::move(ids), reorder, style, matchKeys);
    if (toast) toast(said, false);
    mirror.refreshNow();
    // Show the new mix, from its first deck down.
    const auto& tracks = mirror.model().tracks;
    if (tracks.size() >= 2 && mirror.model().contentEndMs() > before)
        timeline.revealRange(before, mirror.model().contentEndMs(), tracks[tracks.size() - 2].id);
}

void Workspace::importAndPlace(std::vector<ImportRequest> requests, const juce::String& trackId, double positionMs)
{
    if (requests.size() < 2)
    {
        placeImported(std::move(requests), trackId, positionMs);
        return;
    }
    // Several tracks at once: mix them, or just put them down (Escape) as one alone would be.
    auto pending = std::make_shared<std::vector<ImportRequest>>(std::move(requests));
    juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                     .withIconType(juce::MessageBoxIconType::QuestionIcon)
                                     .withTitle("Auto Mix")
                                     .withMessage(juce::String(static_cast<int>(pending->size()))
                                                  + " tracks dropped. Mix them after the arrangement, on two decks, "
                                                    "with transitions chosen from their tempo, key and energy?")
                                     .withButton("Auto Mix (Harmonic Order)")
                                     .withButton("Auto Mix (As Dropped)")
                                     .withButton("Just Place Them")
                                     .withAssociatedComponent(this),
                                 [safe = juce::Component::SafePointer<Workspace>(this), pending, trackId, positionMs](int result)
                                 {
                                     if (safe == nullptr) return;
                                     // 1 and 2 are the mixes; the last button and Escape are 0.
                                     if (result == 0) safe->placeImported(std::move(*pending), trackId, positionMs);
                                     else safe->importAndMix(std::move(*pending), result == 1);
                                 });
}

void Workspace::importAndMix(std::vector<ImportRequest> requests, bool harmonic)
{
    if (toast) toast("Importing " + juce::String(static_cast<int>(requests.size())) + " tracks for the mix...", false);
    importer.import(std::move(requests),
                    [safe = juce::Component::SafePointer<Workspace>(this), harmonic](const juce::StringArray& itemIds)
                    {
                        if (safe == nullptr) return;
                        std::vector<juce::String> ids(itemIds.begin(), itemIds.end());
                        // After the import's undo group closes: the mix is a step of its own.
                        juce::MessageManager::callAsync(
                            [safe, ids, harmonic]
                            {
                                if (safe != nullptr) safe->mixWhenAnalysed(ids, harmonic, juce::Time::getMillisecondCounter() + 30000);
                            });
                    });
}

// The mix needs each track's tempo (and key, for the harmonic order): wait for the analysis.
void Workspace::mixWhenAnalysed(std::vector<juce::String> ids, bool harmonic, juce::uint32 deadlineMs)
{
    mirror.refreshNow();
    const bool ready = std::all_of(ids.begin(), ids.end(),
                                   [this](const juce::String& id)
                                   {
                                       const auto* item = mirror.model().findLibraryItem(id);
                                       return item == nullptr || item->bpm > 0.0;
                                   });
    if (!ready && juce::Time::getMillisecondCounter() < deadlineMs)
    {
        juce::Timer::callAfterDelay(250,
                                    [safe = juce::Component::SafePointer<Workspace>(this), ids, harmonic, deadlineMs]
                                    {
                                        if (safe != nullptr) safe->mixWhenAnalysed(ids, harmonic, deadlineMs);
                                    });
        return;
    }
    runAutoMix(std::move(ids), harmonic, "Auto", false);
}

void Workspace::placeImported(std::vector<ImportRequest> requests, const juce::String& trackId, double positionMs)
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

void Workspace::setupResizeBars()
{
    static constexpr int kMinTimeline = 160; // the timeline keeps at least this much height
    mixerHeight = juce::jlimit(200, 1200, static_cast<int>(prefs.get("ui", "nativeMixerHeight", 340)));
    headerWidth = juce::jlimit(160, 480, static_cast<int>(prefs.get("ui", "trackHeaderWidth", TrackHeaders::kDefaultWidth)));
    bottomBar.setTooltip("Drag to resize");
    bottomBar.onDragStart = [this] { dragStartSize = isMixerShown() ? mixerHeight : bottomPanelHeight; };
    bottomBar.onDrag = [this](int delta)
    {
        const int room = getHeight() - TransportBar::kHeight - kMinTimeline;
        const int next = juce::jlimit(120, juce::jmax(120, room), dragStartSize - delta);
        (isMixerShown() ? mixerHeight : bottomPanelHeight) = next;
        resized();
    };
    bottomBar.onDragEnd = [this]
    {
        prefs.set("ui", isMixerShown() ? "nativeMixerHeight" : "nativeLibraryHeight", isMixerShown() ? mixerHeight : bottomPanelHeight);
        prefs.save();
    };
    headerBar.setTooltip("Drag to resize the track headers");
    headerBar.onDragStart = [this] { dragStartSize = headerWidth; };
    headerBar.onDrag = [this](int delta)
    {
        headerWidth = juce::jlimit(160, juce::jmin(480, getWidth() / 2), dragStartSize + delta);
        resized();
    };
    headerBar.onDragEnd = [this]
    {
        prefs.set("ui", "trackHeaderWidth", headerWidth);
        prefs.save();
    };
    addAndMakeVisible(bottomBar);
    addAndMakeVisible(headerBar);
}

void Workspace::resized()
{
    auto area = getLocalBounds();
    transport.setBounds(area.removeFromTop(TransportBar::kHeight));
    // Whatever sits below the timeline never takes all of it.
    const int room = juce::jmax(120, area.getHeight() - 160);
    if (isMixerShown())
    {
        mixer->setBounds(area.removeFromBottom(juce::jmin(mixerHeight, room)));
        bottomBar.setBounds(area.removeFromBottom(5));
    }
    else if (bottomPanel != nullptr)
    {
        bottomPanel->setBounds(area.removeFromBottom(juce::jmin(bottomPanelHeight, room)));
        bottomBar.setBounds(area.removeFromBottom(5));
    }
    headers.setBounds(area.removeFromLeft(juce::jlimit(160, juce::jmax(160, getWidth() / 2), headerWidth)));
    headerBar.setBounds(area.removeFromLeft(4));
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

void Workspace::selectAllClips()
{
    view.selectedClipIds.clear();
    for (const auto& track : mirror.model().tracks)
        for (const auto& c : track.clips) view.selectedClipIds.insert(c.id);
    view.changed();
}

void Workspace::joinSelection()
{
    actions.joinClips({view.selectedClipIds.begin(), view.selectedClipIds.end()});
}

void Workspace::toggleAutomationOnSelectedTrack()
{
    if (mirror.model().findTrack(view.selectedTrackId) != nullptr) actions.toggleAutomation(view.selectedTrackId);
}

void Workspace::toggleLoop()
{
    const auto& m = mirror.model();
    if (!m.timelineSelection.has_value())
    {
        if (actions.onNotify) actions.onNotify("Drag across the ruler to choose what to loop", false);
        return;
    }
    actions.setTimelineSelection(m.timelineSelection, !m.loopSelection);
}

void Workspace::toggleMetronome()
{
    const auto& m = mirror.model();
    const bool on = !static_cast<bool>(m.raw.getProperty("metronomeEnabled", false));
    auto* payload = new juce::DynamicObject();
    payload->setProperty("enabled", on);
    engine.send("PROJECT_SET_METRONOME", juce::var(payload));
    // Not an edit (as the Master module's button): a clean project writes it straight away.
    if (!m.dirty && m.filePath.isNotEmpty())
    {
        auto* save = new juce::DynamicObject();
        save->setProperty("filePath", m.filePath);
        engine.send("PROJECT_SAVE_VIEW_STATE", juce::var(save));
    }
    if (actions.onNotify) actions.onNotify(on ? "Metronome on" : "Metronome off", false);
}

void Workspace::toggleSnap()
{
    const auto grid = actions.snapGrid();
    if (grid != "free")
    {
        lastSnapGrid = grid;
        actions.setSnapGrid("free");
        if (actions.onNotify) actions.onNotify("Snap off", false);
    }
    else
    {
        actions.setSnapGrid(lastSnapGrid);
        if (actions.onNotify) actions.onNotify("Snap to " + lastSnapGrid, false);
    }
}

void Workspace::openClipEditorOnSelection()
{
    if (view.selectedClipIds.empty() || timeline.onOpenClipEditor == nullptr) return;
    timeline.onOpenClipEditor(*view.selectedClipIds.begin());
}

// The playhead to the previous or next bar line.
void Workspace::stepBars(int bars)
{
    const auto& m = mirror.model();
    const double bpm = m.tempoMap.empty() ? m.bpm : m.tempoCurve().bpmAt(live.positionMs);
    const double bar = 240000.0 / juce::jmax(1.0, bpm);
    const double index = live.positionMs / bar;
    // From just off a bar line, the first step lands on the line itself.
    const double target = juce::jmax(0.0, (bars < 0 ? std::ceil(index - 1e-6) - 1.0 : std::floor(index + 1e-6) + 1.0) * bar);
    actions.seek(target);
    timeline.reveal(target);
}

void Workspace::selectAdjacentTrack(int direction)
{
    const auto& tracks = mirror.model().tracks;
    if (tracks.empty()) return;
    int index = 0;
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i)
        if (tracks[static_cast<size_t>(i)].id == view.selectedTrackId) index = juce::jlimit(0, static_cast<int>(tracks.size()) - 1, i + direction);
    view.selectedTrackId = tracks[static_cast<size_t>(index)].id;
    view.changed();
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
        case 'a': toggleAutomationOnSelectedTrack(); return true;
        case 'c': toggleLoop(); return true;
        case 'k': toggleMetronome(); return true;
        case 'n': toggleSnap(); return true;
        case 'e': openClipEditorOnSelection(); return true;
        case 'x': toggleMixer(); return true;
        case ',': stepBars(-1); return true;
        case '.': stepBars(1); return true;
        default: break;
        }
    }
    if (code == juce::KeyPress::escapeKey && plain)
    {
        // Esc clears the clip selection, then the range.
        if (!view.selectedClipIds.empty())
        {
            view.selectedClipIds.clear();
            view.changed();
        }
        else if (mirror.model().timelineSelection.has_value())
        {
            actions.setTimelineSelection(std::nullopt, false);
        }
        return true;
    }
    if ((code == juce::KeyPress::upKey || code == juce::KeyPress::downKey) && plain && !mods.isShiftDown())
    {
        selectAdjacentTrack(code == juce::KeyPress::upKey ? -1 : 1);
        return true;
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
