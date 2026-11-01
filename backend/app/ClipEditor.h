#pragma once

#include "EngineClient.h"
#include "ProjectModel.h"
#include "TimelineActions.h"
#include "WarpModeDefaults.h"
#include "WaveformStore.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>

namespace stemcel::native
{

// The Clip Editor (the renderer's ClipEditorDialog): the clip's source audio with its
// trim window and beat grid, warp (mode, follow project / fixed BPM), pitch, grid tempo and
// first beat, reverse / brake / backspin, slicing, and a looping preview. Everything is a
// draft until Save, which commits it as one undo step.
class ClipEditor final : public juce::Component, private juce::ChangeListener, private juce::Timer
{
  public:
    // On a timeline clip, or (empty clipId) on a library item: a saved clip saves back to the
    // library and every placement of it; any item can be saved as a new clip.
    ClipEditor(EngineClient& engine, ProjectMirror& mirror, TimelineActions& actions, WaveformStore& waveforms,
               const juce::String& clipId, const juce::String& libraryItemId = {});
    ~ClipEditor() override;

    static void show(EngineClient& engine, ProjectMirror& mirror, TimelineActions& actions, WaveformStore& waveforms,
                     const juce::String& clipId);
    static void showLibraryItem(EngineClient& engine, ProjectMirror& mirror, TimelineActions& actions, WaveformStore& waveforms,
                                const juce::String& itemId);

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;

  private:
    friend class SmokeTest; // drives the draft directly
    class WaveView;
    struct Draft
    {
        double inMs = 0.0;
        double durationMs = 0.0;
        bool tempoEnabled = false;
        bool followProject = true;
        double fixedBpm = 0.0; // the tempo a manually pinned clip plays at
        juce::String mode = kDefaultWarpMode;
        int semitones = 0;
        int cents = 0;
        double gridBpm = 0.0;
        double anchorSec = 0.0;
        bool reversed = false;
        bool brake = false;
        bool backspin = false;
        // Volume shape: timeline ms from the window start, gain 0..1.5 (the clip envelope).
        std::vector<EnvelopePoint> envelope;
    };

    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void readDraft();
    void draftToControls();
    void controlsChanged();
    void save();
    void close();
    void togglePlay();
    void stopPreview();
    void restartPreviewIfPlaying();
    // Push the draft's warp, pitch, reverse, brake, backspin and metronome to the playing
    // preview, so the audition tracks the draft live (previewStore setWarp/setBrake/...).
    void pushPreviewDraft();
    void seekPreview(double windowMs); // window-relative, as the preview voice counts
    double previewRatio() const;
    // A playback selection inside the window (useClipEditorViewport): Play starts there and
    // Loop wraps it; Crop narrows the window to it.
    bool hasSelection() const { return selDurMs > 0.0; }
    void clearSelection();
    void applyPreviewLoop();
    void crop();
    double envelopeDurationMs() const; // the window in timeline ms
    void ensureEnvelope();
    std::vector<double> gridBeatsMs() const; // source-time beat positions
    const ClipModel* clip() const;
    const LibraryItemModel* item() const;
    double sourceBpm() const;
    bool libraryMode() const { return clipId.isEmpty(); }
    const LibraryItemModel* sourceItem() const; // the file's own item (a saved clip's source)
    void saveLibraryClip();
    void saveAsNewClip();
    juce::var warpRatioForSave() const; // void = follow the project

    EngineClient& engine;
    ProjectMirror& mirror;
    TimelineActions& actions;
    WaveformStore& waveforms;
    Subscriptions subscriptions;
    juce::String clipId;
    juce::String libraryItemId;
    Draft draft;
    Draft original;
    bool previewPlaying = false;
    bool previewWaiting = false;
    double previewPositionMs = 0.0;
    bool previewLoaded = false; // loaded and paused keeps its position (useClipEditorTransport)

    std::unique_ptr<WaveView> wave;
    juce::Label title, info;
    juce::TextButton playButton{"Play"}, loopButton{"Loop"}, metronomeButton{"Click"};
    juce::TextButton skipStartButton{"|<"}, skipEndButton{">|"};
    juce::TextButton zoomInButton{"+"}, zoomOutButton{"-"}, fitButton{"Fit"};
    juce::ToggleButton tempoToggle{"Warp to tempo"};
    juce::ComboBox tempoMode, warpMode;
    juce::Label fixedBpmLabel{{}, "Clip BPM"}, semitonesLabel{{}, "Semitones"}, centsLabel{{}, "Cents"},
        gridBpmLabel{{}, "Grid BPM"}, anchorLabel{{}, "First beat (s)"}, gainLabel{{}, "Clip gain"};
    juce::Slider fixedBpm, semitones, cents, gridBpm, anchor;
    // The clip's own gain: applied as it moves (and undoable), not held for Save.
    juce::Slider gain;
    juce::TextButton halveButton{"/2"}, doubleButton{"x2"}, correctTempoButton{"Apply tempo to all clips"};
    juce::ToggleButton reverseToggle{"Reverse"}, brakeToggle{"Brake"}, backspinToggle{"Backspin"};
    juce::ComboBox sliceDivision;
    juce::TextButton gridMarkersButton{"Grid"}, sliceModeButton{"Slice mode"};
    std::vector<double> sliceMarkers; // source ms
    bool sliceMode = false;
    juce::TextButton sliceSamplesButton{"Slice to samples"}, sliceTimelineButton{"Slice on timeline"};
    juce::TextButton fullButton{"Whole file"}, resetButton{"Revert"}, saveButton{"Save"}, cancelButton{"Close"};
    bool loop = true;
    bool metronome = false;
    double selInMs = 0.0, selDurMs = 0.0; // source ms; no selection when the length is 0
    bool volumeMode = false;
    juce::TextButton volumeButton{"Volume"}, cropButton{"Crop"};
    juce::TextButton saveAsNewButton{"Save as New Clip"};
};

} // namespace stemcel::native
