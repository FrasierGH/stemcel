#pragma once

#include "AppController.h"
#include "AppPreferences.h"
#include "AudioImport.h"
#include "EngineClient.h"
#include "LibraryPanel.h"
#include "LiveState.h"
#include "MidiControllerActions.h"
#include "MixerPanel.h"
#include "Stems.h"
#include "ProjectModel.h"
#include "TimelineActions.h"
#include "SectionAnalyser.h"
#include "TimelineView.h"
#include "TrackHeaders.h"
#include "TransportBar.h"
#include "Mashup.h"
#include "WaveformStore.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <map>
#include <memory>

namespace stemcel::native
{

// The editing workspace shown once a project is open: transport, track headers and the
// timeline, with the library panel below (milestone 3).
class Workspace final : public juce::Component, public juce::DragAndDropContainer
{
  public:
    Workspace(EngineClient& engine, AppController& controller, AppPreferences& prefs,
              std::function<void(const juce::String&, bool)> notify);
    ~Workspace() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;

    // Edit and view operations, shared with the menus.
    void splitAtPlayhead();
    void duplicateSelection();
    void deleteSelection();
    void copySelection();
    void cutSelection();
    void pasteAtPlayhead();
    void addMarkerAtPlayhead();
    void toggleLockSelection();
    void addTrack();
    void togglePlay();
    void zoom(double factor);
    void zoomToFit();
    bool hasClipSelection() const { return !view.selectedClipIds.empty(); }
    void importWithDialog();
    void showLibraryTab(int index);
    // The mixer in place of the bottom panel, or the bottom panel back.
    void toggleMixer();
    // Shortcuts' operations (also in the menus).
    void selectAllClips();
    void joinSelection();
    void toggleAutomationOnSelectedTrack();
    void toggleLoop();
    void toggleMetronome();
    void toggleSnap();
    void openClipEditorOnSelection();
    void stepBars(int bars);
    void selectAdjacentTrack(int direction);
    // After a tempo is detected or seeded: line placed clips up with the beat grid (preference).
    void alignAfterAnalysis(const juce::var& analysis);
    void alignAfterTempoSeed();
    bool isMixerShown() const { return mixer != nullptr && mixer->isVisible(); }
    void saveViewState();
    // Imports files/rekordbox tracks and lays them end to end on `trackId` (a new track
    // when empty) from `positionMs`, as one undo step. Two or more ask first whether to auto
    // mix them instead.
    void importAndPlace(std::vector<ImportRequest> requests, const juce::String& trackId, double positionMs);
    // Imports the tracks, waits for their tempo analysis (30 s at most), then auto mixes them
    // after the arrangement: harmonically ordered, or in the order given.
    void importAndMix(std::vector<ImportRequest> requests, bool harmonic);
    // The library's Auto Mix: builds the mix and shows it.
    void runAutoMix(std::vector<juce::String> itemIds, bool reorder, const juce::String& style, bool matchKeys);

    ProjectMirror& projectMirror() { return mirror; }
    TimelineActions& timelineActions() { return actions; }
    TimelineViewState& viewState() { return view; }
    LiveState& liveState() { return live; }
    WaveformStore& waveformStore() { return waveforms; }

    // Set by later milestones: the panel shown beneath the timeline.
    void setBottomPanel(std::unique_ptr<juce::Component> panel, int height);

  private:
    void placeImported(std::vector<ImportRequest> requests, const juce::String& trackId, double positionMs);
    void mixWhenAnalysed(std::vector<juce::String> itemIds, bool harmonic, juce::uint32 deadlineMs);
    EngineClient& engine;
    juce::String lastSnapGrid{"quarter"}; // what N turns snapping back on to
    AppController& controller;
    AppPreferences& prefs;

    ProjectMirror mirror;
    LiveState live;
    TimelineViewState view;
    TimelineActions actions;
    WaveformStore waveforms;
    Subscriptions subscriptions;
    AudioImporter importer;
    Audition audition;
    StemService stems;
    SectionAnalyser sectionAnalyser;
    std::unique_ptr<MashupBuilder> mashup;
    struct PrefsWatcher final : juce::ChangeListener
    {
        std::function<void()> fn;
        void changeListenerCallback(juce::ChangeBroadcaster*) override { if (fn) fn(); }
    } prefsWatcher;

    TransportBar transport;
    TrackHeaders headers;
    TimelineView timeline;
    std::unique_ptr<juce::Component> bottomPanel;
    LibraryPanel* libraryPanel = nullptr;
    int bottomPanelHeight = 0;
    std::unique_ptr<MixerPanel> mixer;
    std::unique_ptr<MidiControllerActions> midi;
    std::map<juce::String, juce::uint32> gridAlignPending; // item id -> when it was analysed
    std::function<void(const juce::String&, bool)> toast;
};

} // namespace stemcel::native
