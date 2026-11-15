#pragma once

// The library's track editor: the selected track's waveform with its beat grid and cues,
// played through the preview voice, and the controls DJs fix a track with before a gig —
// the grid (tempo, half or double, nudged earlier or later, the downbeat put where it
// is), hot cues A–H, memory cues and loops, its rating and colour.

#include "Collection.h"
#include "EngineClient.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace stemcel::native
{
class WaveformStore;
}

namespace stemcel::native::dj
{

class TrackEditor final : public juce::Component, private juce::ChangeListener, private juce::Timer
{
  public:
    TrackEditor(EngineClient& engine, WaveformStore& waveforms);
    ~TrackEditor() override;

    void showTrack(juce::int64 id); // 0 = none
    juce::int64 trackId() const { return id; }
    void togglePlay();
    void stopPreview();
    bool keyPressed(const juce::KeyPress&) override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    // Smoke-test support.
    double positionSec() const { return playhead; }
    void setPositionForTest(double sec) { seek(sec); }
    void setHotCueForTest(int slot) { hotCuePressed(slot); }
    void shiftGridForTest(double ms) { shiftGrid(ms); }
    void setDownbeatForTest() { downbeatHere(); }

  private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void refresh();
    const Track* track() const;
    void seek(double sec);
    double snapped(double sec) const;
    void hotCuePressed(int slot);
    void hotCueMenu(int slot);
    void addMemoryCue();
    void shiftGrid(double ms);
    void scaleTempo(double factor);
    void setTempo(double bpm);
    void downbeatHere();
    void tap();
    void cueMenuAt(double sec);
    juce::Rectangle<float> overviewArea() const;
    juce::Rectangle<float> detailArea() const;
    double secAtOverview(float x) const;
    double secAtDetail(float x) const;
    void paintGrid(juce::Graphics&, juce::Rectangle<float> area, double startSec, double lengthSec, bool numbers) const;
    void paintCues(juce::Graphics&, juce::Rectangle<float> area, double startSec, double lengthSec, bool labels) const;

    EngineClient& engine;
    WaveformStore& waveforms;
    Subscriptions subscriptions;
    juce::int64 id = 0;
    juce::String loadedPath;
    juce::String playablePath;
    bool previewLoaded = false;
    bool previewPlaying = false;
    bool playWhenLoaded = false;
    double playhead = 0.0;     // seconds
    double zoomSeconds = 8.0;  // the detail view's width
    bool draggingOverview = false;
    bool draggingDetail = false;
    float dragStartX = 0.0F;
    double dragStartPlayhead = 0.0;
    std::vector<double> taps;

    juce::Label title, info;
    juce::TextButton playButton{"Play"}, cueButton{"Cue"};
    juce::TextEditor bpmField;
    juce::TextButton halfButton{"/2"}, doubleButton{"x2"}, earlierButton{"<"}, laterButton{">"}, downbeatButton{"1 Here"},
        tapButton{"Tap"};
    juce::ToggleButton quantize{"Q"};
    std::array<juce::TextButton, 8> hotCueButtons;
    juce::TextButton memoryButton{"+ Memory"};
    juce::ComboBox ratingBox, colourBox;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    juce::ThreadPool pool{1};
};

// rekordbox's eight track colours (index 1–8; 0 is none).
juce::Colour trackColour(int index);
juce::String trackColourName(int index);
// The hot cue colours a CDJ shows when a cue has none of its own.
juce::Colour hotCueColour(int slot);

} // namespace stemcel::native::dj
