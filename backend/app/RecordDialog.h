#pragma once

#include "AppPreferences.h"
#include "EngineClient.h"
#include "LiveState.h"
#include "ProjectModel.h"
#include "WaveformStore.h"
#include "TimelineActions.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace stemcel::native
{

// Record Audio (the renderer's RecordAudioDialog over the engine's recording session,
// protocol 1): input device and channels, gain, where the take lands (playhead, project
// start, selection), count-in, click, monitoring, backing tracks and noise cleanup; then
// record / stop, review and commit the take to the timeline or the library. Latency
// calibration measures the round trip and applies it to later takes.
class RecordDialog final : public juce::Component, private juce::Timer, private juce::ChangeListener
{
    friend class SmokeTest; // shows a take's review without a real input
  public:
    RecordDialog(EngineClient& engine, ProjectMirror& mirror, TimelineActions& actions, TimelineViewState& view,
                 AppPreferences& prefs);
    ~RecordDialog() override;

    static void show(EngineClient& engine, ProjectMirror& mirror, TimelineActions& actions, TimelineViewState& view,
                     AppPreferences& prefs);

    void paint(juce::Graphics&) override;
    void resized() override;

  private:
    void timerCallback() override;
    void control(const juce::String& action, std::initializer_list<std::pair<const char*, juce::var>> props = {});
    void sessionState(const juce::var& state);
    void recordingReady(const juce::var& ready);
    void commit(const juce::String& destination);
    void calibrationState(const juce::var& state);
    juce::String calibrationKey() const;
    void pushCalibration();
    void updateEnablement();
    void close();

    EngineClient& engine;
    ProjectMirror& mirror;
    TimelineActions& actions;
    TimelineViewState& view;
    AppPreferences& prefs;
    Subscriptions subscriptions;
    juce::String sessionId, status, inputDevice, inputType;
    juce::var ready;
    float peakL = 0.0f, peakR = 0.0f;
    double recordedMs = 0.0;
    double sampleRate = 0.0;

    juce::Label inputLabel{{}, "Input"}, channelLabel{{}, "Channels"}, gainLabel{{}, "Input gain"}, whereLabel{{}, "Record at"},
        statusLabel, calibrationLabel;
    juce::ComboBox input, channels, where, countIn, mode;
    // Mono records one input channel, stereo an adjacent pair; the channel list follows it.
    juce::ComboBox format;
    juce::StringArray channelNames;
    void fillChannels(int firstChannel, int channelCount);
    juce::Slider gain;
    juce::ToggleButton click{"Click"}, monitor{"Hear input"}, cleanup{"Reduce noise"};
    juce::TextButton recordButton{"Record"}, calibrateButton{"Calibrate latency..."}, closeButton{"Close"};
    // Review.
    juce::TextEditor takeName;
    juce::TextButton toTimeline{"Add to timeline"}, toLibrary{"Save to library"}, discard{"Discard"};
    // RecordAudioReview.vue: a mono take saved as stereo (rewrites the take), or a stereo
    // take's two channels split into two items (each optionally as stereo).
    juce::ToggleButton saveAsStereo{"Save as Stereo"}, splitChannels{"Split Channels"}, eachAsStereo{"Each as Stereo"};
    // Review audition (RecordAudioReview.vue): the take's waveform with Play and click to
    // seek, optionally rolling the arrangement from where the take starts, at its own
    // backing level, which goes back to unity when the review ends.
    juce::TextButton playTake{"Play"};
    juce::ToggleButton withArrangement{"Play With the Arrangement"};
    juce::Label summaryLabel, noticeLabel, backingLabel{{}, "Backing volume"};
    juce::Slider backingVolume;
    juce::Rectangle<int> takeWaveArea;
    std::unique_ptr<WaveformStore> takeWaves;
    bool auditionLoaded = false, auditionPlaying = false, auditionWaiting = false, arrangementRolling = false;
    double auditionPositionMs = 0.0;
    juce::String auditionFile;
    double takeStartMs() const;
    void toggleAudition();
    void startArrangement(double fromTakeMs);
    void stopArrangement();
    void releaseAudition();
    void mouseDown(const juce::MouseEvent&) override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override { repaint(takeWaveArea); }
    juce::Rectangle<int> meterArea;
    juce::StringArray inputTypes, inputDevices;
    bool calibrating = false;
    bool canSplit() const;
};

} // namespace stemcel::native
