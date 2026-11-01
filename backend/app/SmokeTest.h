#pragma once

#include "AppPreferences.h"
#include "AutoMix.h"
#include "AudioImport.h"
#include "MidiControllerActions.h"
#include "RecordDialog.h"
#include "Stems.h"
#include "EngineClient.h"
#include "ProjectModel.h"
#include "TimelineActions.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <juce_events/juce_events.h>

#include <functional>
#include <atomic>
#include <memory>
#include <vector>

namespace stemcel::native
{

// `StemcelNative --smoke-test <dir>`: drives the real app (window, engine, audio device)
// through a scripted journey and quits with exit code 0 on success, 1 on failure or timeout.
// CI runs it on every platform. A step either waits for an engine broadcast (as the
// renderer's e2e journeys do) or, for in-process commands that apply synchronously, checks
// the project model straight after acting. Results also go to `<dir>/smoke-result.txt`.
class SmokeTest final : private juce::Timer
{
  public:
    struct Step
    {
        juce::String name;
        std::function<void()> act;
        // Broadcast that completes the step; empty = check the project model right after act.
        juce::String awaitType;
        std::function<bool(const juce::var&)> accept;
        // Wait this long before acting (e.g. for waveforms to load before a snapshot).
        double delayMs = 0.0;
        // Re-check `accept` on every tick (after act) until it holds, for async work that
        // has no broadcast of its own (imports).
        bool poll = false;
        // A longer limit than the default, for slow work (a model download, a separation).
        double timeoutMs = 0.0;
    };

    // Opens a project through the UI (as File > Open does), so the snapshot shows the workspace.
    std::function<void(const juce::File&)> openInUi;

    SmokeTest(EngineClient& engine, juce::File workDir);
    ~SmokeTest() override;

    static bool isRequested(const juce::String& commandLine, juce::File& workDirOut);

    void start();

  private:
    void addSteps();
    void addTempoJourney();
    void runStep();
    void pass();
    void finish(bool ok, const juce::String& message);
    void timerCallback() override;
    const ProjectModel& model();

    EngineClient& engine;
    juce::File workDir;
    ProjectMirror mirror;
    TimelineActions actions;
    AudioImporter importer;
    AppPreferences prefs; // read-only here: stems need its model paths
    StemService stems;
    std::vector<Step> steps;
    size_t current = 0;
    int subscription = 0;
    int failureSubscription = 0;
    double stepStartedMs = 0.0;
    juce::StringArray report;
    bool finished = false;
    bool delayDone = false;

    juce::StringArray tempoItemIds;
    TimelineViewState midiView;
    std::unique_ptr<LiveState> midiLive;
    std::unique_ptr<MidiControllerActions> midiActions;
    std::unique_ptr<RecordDialog> reviewDialog;
    juce::StringArray tempoClipIds;
    juce::String tempoLateClipId;
    juce::String alignClipId;
    int tempoSeed = 0; // which of the two tempo-journey tracks seeded the project
    juce::String savedClipItemId;
    bool uvrSkipped = false;
    juce::String uvrItemId;
    int uvrTracksBefore = 0;
    juce::StringArray uvrLibraryBefore; // item ids before the best-of-both run
    juce::String lastTempoLine;
    juce::String renameKeyBefore, renameFileBefore;
    juce::String trackId;
    juce::String itemId;
    juce::String clipId;
    juce::String rightClipId;
    juce::String importedId;
    juce::String rekordboxTrackId;
    bool rekordboxWritesOk = false;
    juce::Time seamlessSavedBefore;
    juce::String automationTrackId;
    int automationStage = 0;
    juce::String nearOutClipId, nearInClipId;
    juce::String sectionItemId, sectionClipId;
    juce::uint32 shortcutStartMs = 0;
    int shortcutClipsBefore = 0, shortcutJoins = 0;
    double shortcutOriginMs = 0.0, shortcutJoinEndMs = 0.0, panScrollBefore = 0.0, panScrollAfter = 0.0;
    juce::String shortcutCopyId, snapBefore;
    int dropTracksBefore = 0;
    bool dropPromptSeen = false;
    bool sectionRevealed = false;
    std::unique_ptr<class SectionAnalyser> sectionAnalyser;
    juce::File rekordboxFixtureDb;
    juce::String rekordboxFixturePassphrase;
    juce::String rekordboxItemId;
    bool polling = false;
    double expectedMixSeconds = 0.0;
    int tracksBefore = 0;
    std::vector<juce::String> pastedIds;
    juce::String droppedClipId;
    std::unique_ptr<class WaveformStore> editorWaveforms;
    std::unique_ptr<class WaveformStore> mixWaveforms;
    juce::String mixTransitionId;
    juce::String stutterTrackId;
    bool pluginScanSkipped = false;
    int pluginAutoStage = 0;
    juce::uint32 pluginAutoPlayedAt = 0;
    juce::String pluginAutoTrackId;
    struct PluginTest
    {
        juce::String identifier, name, plainName, slotId;
        juce::StringArray names;
        std::vector<float> targets;
    };
    std::vector<PluginTest> pluginTests;
    juce::String everyParamLine;
    double fitEndX = 0.0, fitFloor = 0.0, scrolledTo = 0.0;
    int fitWidth = 0;
    juce::uint32 followStartedAt = 0;
    int followStage = 0;
    juce::String resizeLine;
    juce::String gainClipId, gainCopyId;
    int txStage = 0;
    juce::uint32 txStartedAt = 0;
    double txOldStart = 0.0, txOldEnd = 0.0, txExpectEnd = 0.0, txExpectStart = 0.0;
    juce::String txFollowLine;
    int driftStage = 0;
    std::vector<juce::String> wheelClips;
    automix::Camelot wheelTarget;
    juce::String wheelLine, wheelItemId, wheelItemKey;
    std::vector<double> driftBeats;
    juce::String driftItemId, driftClipId, driftLine;
    std::unique_ptr<class TempoDriftAnalyser> driftAnalyser;
    bool resizeOk = false;
    bool everyParamOk = false;
    std::shared_ptr<std::atomic<bool>> pluginDirScanDone;
    int dragClipsBefore = 0, dragTracksBefore = 0;
    juce::String dragItemId;

    std::unique_ptr<class MashupBuilder> mashupBuilder;
    int mashupTracksBefore = 0;
    int tabsTracksBefore = 0;
    juce::String tabsPastedItem;
    std::unique_ptr<juce::Component> editorView;
};

} // namespace stemcel::native
