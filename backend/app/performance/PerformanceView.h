#pragma once

// Live performance mode: two decks, the DJ mixer, pads and a browser on screen, driven by a
// DJ controller (the DDJ-400, as in rekordbox) or the mouse. Opening it opens the engine's
// performance section (PERF_OPEN) and hands the controller over to it; what is played is
// recorded and written into the timeline when the take ends (PerformanceTake).

#include "AppPreferences.h"
#include "AudioImport.h"
#include "EngineClient.h"
#include "PerfBrowser.h"
#include "PerfControls.h"
#include "PerfState.h"
#include "DeckDisplay.h"
#include "PerformanceTake.h"
#include "ProjectModel.h"
#include "TimelineActions.h"
#include "WaveformStore.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
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
    };

    explicit PerformanceView(Context context);
    ~PerformanceView() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;

    // Ends the take (if any) and writes it into the timeline; the view closes after.
    void finishTake(std::function<void()> then = {});
    // For tests.
    const State& stateForTest() const { return state; }
    const DeckTrackInfo& trackForTest(int deck) const { return infos[static_cast<size_t>(deck - 1)]; }
    PerfBrowser& browserForTest() { return browser; }
    void loadForTest(const juce::String& itemId, int deck) { loadItem(itemId, deck); }
    const PerformanceTake& takeForTest() const { return take; }

  private:
    struct DeckControls
    {
        std::unique_ptr<DeckDisplay> display;
        std::unique_ptr<HoldButton> play, cue, sync, keyLock, quantize, loopIn, loopOut, reloop, halve, dbl;
        std::unique_ptr<ControlSlider> tempo, trim, eqHi, eqMid, eqLow, filter, fader;
        std::unique_ptr<HoldButton> hpCue;
        std::array<std::unique_ptr<HoldButton>, 8> pads, modes;
        std::unique_ptr<HoldButton> loadButton;
        double posAt = 0.0;   // the state's position, and when it came
        double receivedAt = 0.0;
        double lastLoadPress = 0.0;
    };

    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void build(int deck);
    void onState(const juce::var&);
    void onLoaded(const juce::var&);
    void onUi(const juce::var&);
    void onCues(const juce::var&);
    void loadRow(const BrowserRow& row, int deck);
    void loadItem(const juce::String& itemId, int deck);
    void loadButton(int deck);
    void enableControllers(const juce::var& devices);
    void restoreControllers();
    juce::String padLabel(int deck, int pad) const;
    void layoutDeck(int deck, juce::Rectangle<int> area);
    void layoutMixer(juce::Rectangle<int> area);

    Context ctx;
    Subscriptions subscriptions;
    State state;
    std::array<DeckTrackInfo, kDecks> infos{};
    std::array<DeckControls, kDecks> decks;
    std::unique_ptr<ControlSlider> crossfader;
    PerfBrowser browser;
    juce::TextButton exitButton{"Exit"}, recButton{"REC"};
    juce::Label status, midiLine;
    juce::Rectangle<int> mixerArea, topBar;
    PerformanceTake take;
    bool shiftHeld = false;
    bool controllersEnabled = false;
    int takeNumber = 0;
    juce::StringArray controllerIds;
};

} // namespace stemcel::native::perf
