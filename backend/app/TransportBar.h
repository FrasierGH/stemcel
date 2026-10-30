#pragma once

#include "AppPreferences.h"
#include "LiveState.h"
#include "ProjectModel.h"
#include "TimelineActions.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace stemcel::native
{

class TimelineView;

// The tempo box: click to type a BPM, or drag up and down or turn the wheel to change it
// (Shift for fine steps). A drag or a run of wheel steps is one change, sent when it ends.
class BpmField final : public juce::Label, private juce::Timer
{
  public:
    BpmField();
    std::function<double()> current;          // the tempo shown while nothing is being changed
    std::function<void(double bpm)> onCommit; // a dragged or scrolled value

    // Being typed, dragged or scrolled: the shown value is the user's, not the project's.
    bool busy() const { return dragging || timerRunning || isBeingEdited(); }

    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

  private:
    void timerCallback() override;
    void show(double bpm);

    double dragStart = 0.0;
    double pending = 0.0;
    bool dragging = false;
    bool timerRunning = false;
};

// Transport and project controls above the timeline (the renderer's TransportBar): play,
// stop, position, tempo, snap grid, loop, follow, zoom, master volume and meter.
class TransportBar final : public juce::Component, private juce::ChangeListener, private juce::Timer
{
  public:
    static constexpr int kHeight = 44;

    TransportBar(ProjectMirror& mirror, TimelineViewState& view, LiveState& live, TimelineActions& actions,
                 AppPreferences& prefs);
    ~TransportBar() override;

    void setTimeline(TimelineView* timelineView) { timeline = timelineView; }
    // The Mixer button, which the workspace answers.
    std::function<void()> onToggleMixer;
    void setMixerShown(bool shown);
    void togglePlay();

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;

  private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void sync();
    juce::String positionText() const;
    // The tempo box: the project tempo, or the tempo map's at the playhead.
    void showTempo();

    ProjectMirror& mirror;
    TimelineViewState& view;
    LiveState& live;
    TimelineActions& actions;
    AppPreferences& prefs;
    TimelineView* timeline = nullptr;

    juce::TextButton playButton{"Play"};
    juce::TextButton stopButton{"Stop"};
    juce::Label position;
    juce::Label bpmLabel{{}, "BPM"};
    BpmField bpmValue;
    void applyTempo(double typed);
    juce::ComboBox snapBox;
    juce::TextButton loopButton{"Loop"};
    juce::TextButton followButton{"Follow"};
    juce::TextButton volumeButton{"Vol"};
    juce::TextButton mixerButton{"Mixer"};
    juce::TextButton skipBack{"|<"}, skipForward{">|"};
    void skip(bool forward);
    juce::TextButton zoomOut{"-"};
    juce::TextButton zoomIn{"+"};
    juce::TextButton zoomFit{"Fit"};
    juce::Slider zoomSlider{juce::Slider::LinearHorizontal, juce::Slider::NoTextBox};
    juce::Label masterLabel{{}, "MASTER"};
    juce::Slider master{juce::Slider::LinearHorizontal, juce::Slider::NoTextBox};
    juce::Rectangle<int> meterArea;
    // The meter's peak hold per channel (falls back after a moment) and clip light.
    float holdL = 0.0f, holdR = 0.0f;
    juce::uint32 holdTimeL = 0, holdTimeR = 0;
    bool clipped = false;
};

} // namespace stemcel::native
