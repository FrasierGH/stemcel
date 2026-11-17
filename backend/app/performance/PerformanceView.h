#pragma once

// Live performance mode: two decks, the DJ mixer, pads and a browser on screen, driven by a
// DJ controller (the DDJ-400, as in rekordbox) or the mouse. Opening it opens the engine's
// performance section (PERF_OPEN) and hands the controller over to it; what is played is
// recorded and written into the timeline when the take ends (PerformanceTake).

#include "JogPlatter.h"
#include "StackedWaveforms.h"
#include "TimelineStrip.h"
#include "AppPreferences.h"
#include "AudioImport.h"
#include "EngineClient.h"
#include "PerfBrowser.h"
#include "PerfControls.h"
#include "PerfState.h"
#include "DeckDisplay.h"
#include "PerformanceTake.h"
#include "TrackDriver.h"
#include "ProjectModel.h"
#include "TimelineActions.h"
#include "WaveformStore.h"
#include "Stems.h"
#include "LevelMeter.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <optional>
#include <memory>

namespace stemcel::native::perf
{

class PerformanceView final : public juce::Component, private juce::Timer, private juce::ChangeListener
{
  public:
    struct Context
    {
        EngineClient& engine;
        ProjectMirror& mirror;
        TimelineActions& actions;
        WaveformStore& waveforms;
        AudioImporter& importer;
        AppPreferences& prefs;
        std::function<const RekordboxSnapshot*()> rekordbox;
        std::function<double()> playheadMs; // where a take lands in the timeline
        std::function<void(double)> seekTimeline; // after a take: the playhead to its end
        std::function<void(const juce::String&, bool)> toast;
        std::function<void()> exit;
        LiveState& live;
        StemService& stems; // separations for Stem Mix
    };

    explicit PerformanceView(Context context);
    ~PerformanceView() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;

    // Ends the take (if any) and writes it into the timeline; the view closes after.
    void finishTake(std::function<void()> then = {});
    // Debug > Rescan Controllers: finds and opens the controllers afresh.
    void rescanControllers();
    // For the dancer: the beats of the deck leading the mix (the master, else one playing),
    // while one plays.
    std::optional<double> danceBeats() const;
    // A separation's progress (StemService's), for the STEMS buttons: percent < 0 when idle.
    void stemProgress(double percent, const juce::String& stage);
    // Redo from here (PerformanceRedo.cpp): on, and off (the pass written).
    void startRedo();
    void stopRedo();
    bool redoActive() const { return redo.active; }
    // For tests.
    const State& stateForTest() const { return state; }
    // For the smoke test: REC and REDO as their buttons, a deck taken over, KEEP.
    void recForTest(bool on) { if (recButton.getToggleState() != on) recButton.setToggleState(on, juce::sendNotification); }
    void redoForTest(bool on) { redoButton.setToggleState(on, juce::sendNotification); }
    bool takeOverForTest(int deck) { return redoTakesOver("tempo", deck); }
    bool keepForTest() { return driver.keepPass(); }
    bool redoRunningForTest() const { return redo.running; }
    bool heldPassForTest() const { return driver.hasHeldPass(); }
    const DeckTrackInfo& trackForTest(int deck) const { return infos[static_cast<size_t>(deck - 1)]; }
    PerfBrowser& browserForTest() { return browser; }
    TimelineStrip& stripForTest() { return *timelineStrip; }
    JogPlatter* platterForTest(int deck) { return platters[static_cast<size_t>(deck - 1)].get(); }
    StackedWaveforms* stackedForTest() { return stacked.get(); }
    void setStackedForTest(bool on) { stackButton.setToggleState(on, juce::sendNotification); }
    void setFourDecksForTest(bool on) { fourButton.setToggleState(on, juce::sendNotification); }
    void loadForTest(const juce::String& itemId, int deck) { loadItem(itemId, deck); }
    const PerformanceTake& takeForTest() const { return take; }
    void bindForTest(int deck, const juce::String& trackId)
    {
        int id = 1;
        for (size_t i = 0; i < targetIds.size(); ++i)
            if (targetIds[i] == trackId) id = static_cast<int>(i) + 2;
        targetBoxes[static_cast<size_t>(deck - 1)]->setSelectedId(id, juce::sendNotificationSync); // as the picker does
    }
    void loadRegionForTest(const juce::String& trackId, double fromMs, double toMs, int deck)
    {
        BrowserRow row;
        row.isRegion = true;
        row.regionTrackId = trackId;
        row.regionStartMs = fromMs;
        row.regionEndMs = toMs;
        row.title = "region";
        renderRegion(row, deck);
    }
    bool hasRegionForTest(int deck) const { return deckRegions[static_cast<size_t>(deck - 1)].has_value(); }

  private:
    struct DeckControls
    {
        std::unique_ptr<DeckDisplay> display;
        std::unique_ptr<HoldButton> play, cue, sync, keyLock, quantize, loopIn, loopOut, reloop, halve, dbl;
        std::unique_ptr<ControlSlider> tempo, trim, eqHi, eqMid, eqLow, filter, fader;
        std::unique_ptr<HoldButton> hpCue;
        std::array<std::unique_ptr<HoldButton>, 8> pads, modes;
        std::unique_ptr<HoldButton> loadButton, stemsButton;
        double posAt = 0.0;   // the state's position, and when it came
        double receivedAt = 0.0;
        // The position the screens show: it runs on at the deck's rate every frame and is eased
        // toward the engine's, so the waveforms scroll steadily rather than stepping with each
        // state message.
        double shownPos = 0.0, shownAt = 0.0;
        double lastLoadPress = 0.0;
    };

    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void build(int deck);
    void onState(const juce::var&);
    void onLoaded(const juce::var&);
    void onLoadComplete(const juce::var&);
    void onItemAttached(const juce::var&);
    // A track off the disk at once (path and hints), while it joins the project's library.
    void loadFile(const juce::File& file, int deck, const juce::DynamicObject::Ptr& hints, ImportRequest request);
    // Reading ahead the track the browser cursor rests on, so LOAD is instant.
    void preloadSelected();
    juce::String preloadCandidate, preloaded;
    double preloadSince = 0.0;
    void onUi(const juce::var&);
    void onCues(const juce::var&);
    void loadRow(const BrowserRow& row, int deck);
    void loadItem(const juce::String& itemId, int deck);
    void loadButton(int deck);
    void loadSampleSlot(int slot);
    void renderRegion(const BrowserRow& row, int deck);
    void regionRendered(const juce::var& payload);
    void refreshTargets();
    void clearRegion(const juce::String& trackId, double fromMs, double toMs);
    void buildFx();
    // The Beat FX display, clicked: every effect in a menu, as rekordbox's FX panel lists them.
    juce::Rectangle<int> fxDisplayArea() const;
    void showFxMenu();
    void enableControllers(const juce::var& devices);
    void restoreControllers();
    void openControllers();
    juce::String padLabel(int deck, int pad) const;
    void layoutDeck(int deck, juce::Rectangle<int> area, bool compact = false);

    // Stem Mix (PerformanceStems.cpp).
    struct DeckStems
    {
        int serial = 0;          // the deck's track these are for
        bool loading = false;    // PERF_STEMS sent, not answered
        bool separating = false; // a separation for it is running
        bool failed = false, asked = false;
        bool twoStems = false; // vocals and instrumental only (no bass or drums)
    };
    std::array<DeckStems, kDecks> deckStems;
    std::array<juce::Rectangle<int>, kDecks> legendAreas;
    double stemPercent = -1.0;
    juce::String stemStage;
    std::array<juce::String, kStems> stemItemsFor(const juce::String& itemId) const;
    void stemsButtonPressed(int deck);
    void prepareStems(int deck, bool separateIfMissing);
    void prepareStems(int deck, bool separateIfMissing, bool twoStems);
    void onStemsReady(const juce::var&);
    void stemTick();
    juce::String stemsButtonText(int deck) const;
    void paintStemLegend(juce::Graphics& g, int deck) const;
    void layoutMixer(juce::Rectangle<int> area);

    // Redo from here (PerformanceRedo.cpp).
    struct Takeover
    {
        bool on = false, loading = false, keyLock = false;
        int serialBefore = 0; // the deck's track before the takeover's load
        double atMs = 0.0, clipOffsetMs = 0.0, clipInMs = 0.0, ratio = 1.0;
        juce::String trackId, itemId;
    };
    struct Join
    {
        double originalMs = 0.0, stopMs = 0.0; // where the take plays the deck's moment; the stop
        juce::String itemId;
        double sourceMs = 0.0, ratio = 1.0;
    };
    struct Redo
    {
        bool active = false, running = false, write = false;
        std::array<juce::String, kDecks> tracks;              // the take's deck tracks
        std::array<std::vector<ClipModel>, kDecks> original;  // their clips as the pass began
        std::array<Takeover, kDecks> takeovers;
        double passStartMs = 0.0, passStartClock = 0.0;
    };
    Redo redo;
    juce::TextButton redoButton{"REDO"}, keepButton{"KEEP"};
    juce::Label joinLight;
    void setUpRedo();
    std::array<juce::String, kDecks> takeTracksAt(double ms) const;
    void beginPass();
    void endPass();
    void takeOver(int deck);
    void redoTick();
    void rebindAfterPass();
    std::optional<Join> joinFor(int deck) const;
    bool redoTakesOver(const juce::String& control, int deck);

    Context ctx;
    Subscriptions subscriptions;
    State state;
    std::array<DeckTrackInfo, kDecks> infos{};
    std::array<DeckControls, kDecks> decks;
    std::unique_ptr<ControlSlider> crossfader;
    // The Beat FX section, Sound Color FX and Release FX, and the sampler.
    std::unique_ptr<HoldButton> fxPrev, fxNext, beatPrev, beatNext, fxOnButton, releaseButton;
    std::array<std::unique_ptr<HoldButton>, 5> fxChannelButtons; // CH SELECT codes 0..4 (1, 2, M, 3, 4)
    std::unique_ptr<ControlSlider> fxLevelKnob, samplerVolumeKnob;
    juce::ComboBox colorFxBox, releaseFxBox, samplerBankBox;
    juce::Rectangle<int> fxArea;
    std::array<juce::String, 64> sampleNames;
    PerfBrowser browser;
    juce::TextButton exitButton{"Exit"}, recButton{"REC"}, autoGainButton{"AUTO GAIN"}, plattersButton{"DISCS"},
        stackButton{"STACKED"};
    // Four decks: 4 DECKS shows decks 3 and 4 (and their channels), and CTRL 3 / CTRL 4 put a
    // two-deck controller's left or right side on them.
    juce::TextButton fourButton{"4 DECKS"};
    std::array<juce::TextButton, 2> layerButtons;
    bool fourDecks() const { return fourButton.getToggleState(); }
    void setDeckShown(int deck, bool shown);
    std::array<bool, kDecks> meterOnRight{true, false, true, false};
    std::unique_ptr<StackedWaveforms> stacked; // the close-ups one above the other (STACKED)
    std::unique_ptr<TimelineStrip> timelineStrip;
    std::array<std::unique_ptr<JogPlatter>, kDecks> platters;
    // The take being recorded, as the timeline strip shows it: each deck's audible stretches.
    std::vector<TimelineStrip::Pending> pendingTake;
    std::array<int, kDecks> openStretch{-1, -1, -1, -1};
    bool trackingTake = false;
    double takeStartMs = 0.0, takeStartClock = 0.0;
    void trackTake();
    void applyAutoGain(int deck);
    // Each channel's meter: its L and R levels as shown (linear, falling back slowly), and their
    // peak holds.
    std::array<std::array<float, 2>, kDecks> meterLevels{};
    std::array<std::array<meter::PeakHold, 2>, kDecks> meterHolds{};
    double meterAt = 0.0;
    juce::Rectangle<float> meterArea(int deck) const;
    juce::Label status, midiLine;
    juce::Rectangle<int> mixerArea, topBar;
    PerformanceTake take;
    TrackDriver driver;
    std::array<std::unique_ptr<juce::ComboBox>, kDecks> targetBoxes;
    std::vector<juce::String> targetIds; // combo id - 2 → track id
    // A deck playing a stretch of the timeline: a take with it replaces that stretch.
    struct Region
    {
        juce::String trackId; // "" = the whole mix
        double fromMs = 0.0, toMs = 0.0;
        juce::String itemId;
    };
    std::array<std::optional<Region>, kDecks> deckRegions;
    struct PendingRegion
    {
        juce::String path;
        int deck = 1;
        Region region;
    };
    std::vector<PendingRegion> pendingRegions;
    bool shiftHeld = false;
    bool controllersEnabled = false;
    int takeNumber = 0;
    juce::StringArray controllerIds, controllerInputs;
    juce::String controllerNames;
    int openAttempts = 0;
    bool controllerOpened = false;
};

} // namespace stemcel::native::perf
