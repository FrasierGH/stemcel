#pragma once

#include "Automation.h"

#include "LiveState.h"
#include "ProjectModel.h"
#include "TimelineActions.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <map>
#include <memory>

namespace stemcel::native
{

// One track's header: colour, name, automation (A), mute/solo, gain, pan and meter (the
// renderer's TrackHeaderPanel row), and beside each automation lane its header: the
// parameter it shows (click to change), + for another lane and x to close it.
class TrackHeaderRow final : public juce::Component
{
  public:
    TrackHeaderRow(TimelineActions& actions, TimelineViewState& view, LiveState& live);

    void update(const TrackModel& track, int displayIndex, bool silenced, int clipHeight);
    // The master's row: its name and meter, and a lane menu of the master's parameters.
    bool isMaster() const { return trackId == kMasterTrackId; }
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void paintMeter(juce::Graphics&);

    juce::String trackId;

  private:
    void showMenu();
    void rename();

    TimelineActions& actions;
    TimelineViewState& view;
    LiveState& live;

    juce::Label name;
    juce::String storedName, shownName; // the track's name, and what the label shows for it
    juce::TextButton automationButton{"A"};
    juce::TextButton mute{"M"};
    juce::TextButton solo{"S"};
    juce::Slider volume{juce::Slider::LinearHorizontal, juce::Slider::NoTextBox};
    juce::Slider pan{juce::Slider::LinearHorizontal, juce::Slider::NoTextBox};
    // What each slider is, and its value while it is dragged.
    juce::Label gainCaption{{}, "Gain"}, panCaption{{}, "Pan"};
    int colourIndex = 0;
    int clipLaneHeight = 0;
    int displayIndex = 0;
    int trackCount = 0;
    bool resizing = false;
    int resizeStartHeight = 0;
    std::vector<automation::Lane> lanes;
    juce::var plugins; // the inserts, for their parameter lanes
    void toggleLane(const juce::String& paramId);
    juce::String laneCaption(const juce::String& paramId) const;
    void addPluginLaneMenus(juce::PopupMenu& menu);
    // The parameters a lane can show: those with points first, then the track's (or the
    // master's), then each plugin's. `chosen` gets the one picked.
    void addParamItems(juce::PopupMenu& menu, const juce::String& current, std::function<void(const juce::String&)> chosen);
    void showLaneParamMenu(const juce::String& laneParam, bool adding);
    juce::StringArray automated; // parameters with points
    struct LaneHit
    {
        juce::String paramId;
        juce::Rectangle<int> caption, add, close;
    };
    std::vector<LaneHit> laneHits;
    bool isSilenced = false;
    juce::Rectangle<int> meterArea;
    juce::Rectangle<int> swatchArea;
};

// The header column, scrolled in step with the timeline.
class TrackHeaders final : public juce::Component, private juce::ChangeListener, private juce::Timer
{
  public:
    static constexpr int kDefaultWidth = 193;

    TrackHeaders(ProjectMirror& mirror, TimelineViewState& view, LiveState& live, TimelineActions& actions);
    ~TrackHeaders() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

  private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void rebuild();
    void paintHeader(juce::Graphics& g);

    ProjectMirror& mirror;
    TimelineViewState& view;
    LiveState& live;
    TimelineActions& actions;
    juce::TextButton addTrackButton{"+ Add Track"};
    juce::TextButton tempoButton{"Tempo"}; // shows the tempo lane
    bool hadTempoMap = false;
    // Covers the ruler and tempo-lane height above the rows, so a scrolled row never shows there.
    struct HeaderCover final : juce::Component
    {
        std::function<void(juce::Graphics&)> painter;
        void paint(juce::Graphics& g) override { if (painter) painter(g); }
    } cover;
    std::map<juce::String, std::unique_ptr<TrackHeaderRow>> rowComponents;
};

// Linear gain <-> slider position, with unity near the top of the travel (renderer taper).
double gainToSlider(double gain);
double sliderToGain(double position);

} // namespace stemcel::native
