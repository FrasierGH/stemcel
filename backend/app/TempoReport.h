#pragma once

#include "EngineClient.h"
#include "ProjectModel.h"
#include "TimelineActions.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <set>

namespace stemcel::native
{

// The BPM variability check (stemcel::analyseTempoDrift): a track's tempo bar by bar, stored
// on its library item, and what to do about a drifting one.
namespace tempodrift
{
struct Drift
{
    double mainBpm = 0.0;
    double driftPercent = 0.0;
    std::vector<TempoPoint> curve; // {ms into the file, bpm}, one per bar
    struct Section
    {
        double startSec = 0.0, endSec = 0.0, bpm = 0.0;
    };
    std::vector<Section> sections;
    bool valid() const { return mainBpm > 0.0 && curve.size() >= 2; }
};
// The analysis stored on the item (empty when it has not been run).
Drift of(const LibraryItemModel& item);

// Auto-warp: the clip follows the project tempo beat for beat along its source's measured
// tempo, so drifting stretches land back on the grid. One undo step.
bool autoWarp(TimelineActions& actions, const ProjectModel& model, const juce::String& clipId);
// The project tempo follows the clip instead: its measured tempo becomes the tempo map over
// the clip's span, and the clip plays as recorded (other warped clips follow the map). One
// undo step.
bool followInProjectTempo(TimelineActions& actions, const ProjectModel& model, const juce::String& clipId);
// Back to the clip's one detected tempo.
void clearWarp(TimelineActions& actions, const juce::String& clipId);
} // namespace tempodrift

// Runs the check on a worker and stores the result on the item (LIBRARY_ITEM_SET_TEMPO_DRIFT).
class TempoDriftAnalyser final
{
  public:
    TempoDriftAnalyser(EngineClient& engine, ProjectMirror& mirror);
    ~TempoDriftAnalyser();

    void analyse(const juce::String& itemId);
    bool isRunning(const juce::String& itemId) const { return running.count(itemId) != 0; }
    std::function<void(const juce::String& message, bool isError)> onNotify;

  private:
    EngineClient& engine;
    ProjectMirror& mirror;
    juce::ThreadPool pool{1};
    std::set<juce::String> running;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
};

// The report: the tempo over the track (its main tempo, the drifting stretches shaded), and for
// a clip, the three ways to deal with drift: auto-warp, follow in the project tempo, or edit
// the warp by hand on the graph (click to add a point, drag it, double-click to remove it).
class TempoReport final : public juce::Component, private juce::ChangeListener
{
  public:
    TempoReport(ProjectMirror& mirror, TimelineActions& actions, TempoDriftAnalyser& analyser, const juce::String& itemId,
                const juce::String& clipId);
    ~TempoReport() override;
    static void show(ProjectMirror& mirror, TimelineActions& actions, TempoDriftAnalyser& analyser, const juce::String& itemId,
                     const juce::String& clipId);

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;

  private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void refresh();
    juce::Rectangle<float> graph() const;
    juce::Point<float> toGraph(double ms, double bpm) const;
    std::pair<double, double> fromGraph(juce::Point<float> p) const;
    std::pair<double, double> bpmRange() const;
    int handleAt(juce::Point<float> p) const;
    void startEditing();

    ProjectMirror& mirror;
    TimelineActions& actions;
    TempoDriftAnalyser& analyser;
    juce::String itemId, clipId;
    tempodrift::Drift drift;
    double durationMs = 0.0;
    juce::Label summary;
    juce::TextButton autoWarpButton{"Auto-Warp to Main Tempo"}, followButton{"Follow in Project Tempo"},
        editButton{"Edit Warp by Hand"}, clearButton{"Clear Warp"}, againButton{"Analyse Again"}, closeButton{"Close"};
    bool editing = false;
    std::vector<TempoPoint> draft; // the clip's warp curve while it is edited
    int dragging = -1;
};

} // namespace stemcel::native
