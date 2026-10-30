#pragma once

#include "EngineClient.h"
#include "ProjectModel.h"
#include "WaveformStore.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <set>

namespace stemcel::native
{

// The Scratch Editor (the renderer's ScratchEditorDialog over scratch protocol 1): the
// clip (or library item) on a virtual deck. Drag the record to scratch, ride the
// crossfader, play / pause, and record a take; a recorded take can be saved as a library
// sample (baked audio that keeps its notation), kept as a pattern, or applied to the clip
// so it plays scratched on the timeline. MIDI controllers drive the same session.
class ScratchEditor final : public juce::Component, private juce::Timer
{
  public:
    ScratchEditor(EngineClient& engine, ProjectMirror& mirror, WaveformStore& waveforms, const juce::String& clipId,
                  const juce::String& libraryItemId, std::function<void(const juce::String&, bool)> notify);
    ~ScratchEditor() override;

    static void show(EngineClient& engine, ProjectMirror& mirror, WaveformStore& waveforms, const juce::String& clipId,
                     const juce::String& libraryItemId, std::function<void(const juce::String&, bool)> notify);
    // MIDI Cue while scratching: prepare the backing track, as the button does. False when
    // no scratch editor is open (Cue then steps to the previous marker).
    static bool cueBuildBacking();

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;

  private:
    class Deck;
    class Notation;
    void timerCallback() override;
    void control(const juce::String& action, std::initializer_list<std::pair<const char*, juce::var>> props = {});
    void state(const juce::var& s);
    void saveAsSample();
    void savePattern(bool applyToClip);
    // The project's saved patterns (scratchPersistenceController.ts): load one as the take,
    // rename or delete it. Saving after a load updates that pattern rather than adding one.
    juce::Array<juce::var> savedPatterns() const;
    void refreshSaved();
    void loadSaved();
    void renameSaved();
    void deleteSaved();
    void close();
    juce::String sourceItemId() const;

    EngineClient& engine;
    ProjectMirror& mirror;
    WaveformStore& waveforms;
    juce::String clipId, libraryItemId;
    std::function<void(const juce::String&, bool)> notify;
    Subscriptions subscriptions;

    juce::String sessionId, status, error;
    double progress = 0.0;
    juce::int64 positionUs = 0, durationUs = 0;
    double platterTurns = 0.0;
    double playbackRate = 0.0;
    bool armed = false, touched = false;
    juce::var pattern; // the last recorded take (crop edits apply to it)
    juce::String loadedPatternId; // the saved pattern the take came from, if any
    juce::String savedSignature;  // what the saved list was built from
    bool replaying = false;
    double replayPosition = 0.0; // 0..1 of the take
    juce::String backingStatus;
    juce::int64 backingPositionUs = 0, backingDurationUs = 0;

    std::unique_ptr<Deck> deck;
    std::unique_ptr<Notation> notation;
    juce::TextButton replayButton{"Play take"};
    juce::TextButton backingButton{"Build"}, backingClear{"Clear"};
    // ScratchBackingPanel.vue: which tracks make the bed, where it starts, how long it runs.
    juce::TextButton backingTracksButton{"Tracks"};
    juce::ComboBox backingFrom, backingLength;
    juce::Label backingHeading{{}, "Backing"};
    mutable std::set<juce::String> backingTrackIds; // the chosen tracks; muted ones never join
    mutable bool backingTracksSeeded = false;
    std::vector<juce::String> audibleBackingTracks() const;
    void showBackingTracksMenu();
    juce::ToggleButton backingLoop{"Loop"};
    juce::Slider backingGain, scratchGain;
    juce::Label backingLabel, gainLabel{{}, "Scratch level"};
    juce::Label title, statusLabel;
    juce::TextButton playButton{"Play"}, startButton{"|<"}, armButton{"Arm"}, recordButton{"Record"};
    juce::Slider crossfader;
    juce::ComboBox realism;
    juce::TextEditor takeName;
    juce::TextButton saveSample{"Save as sample"}, savePatternButton{"Keep pattern"}, applyButton{"Apply to clip"},
        closeButton{"Close"};
    juce::Label savedLabel{{}, "Saved"};
    juce::ComboBox savedList;
    juce::TextButton loadSavedButton{"Load"}, renameSavedButton{"Rename..."}, deleteSavedButton{"Delete"};
    juce::Rectangle<int> waveArea;
};

} // namespace stemcel::native
