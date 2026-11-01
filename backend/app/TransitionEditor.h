#pragma once

#include "MixTransitions.h"
#include "ProjectModel.h"
#include "TimelineActions.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace stemcel::native
{

// Edits one join's transition as its two halves (mix::Join): for the Out and the In, which
// move it is, the span of the window it covers, its curve, the fader, a filter sweep, what
// each EQ band does (swap on a point or sweep), how deep the kills go, and an Out's effect.
// A graph shows the automation it writes over the window's bars and beats. Each option has a
// + that hands its curve to the graph to draw by hand (the option greys out): click to add a
// point, drag one, double-click to delete it, right-click a segment for its shape or
// right-drag it to bend. Changes apply as they are made (each burst of changes one undo
// step); opening it on a plain preset starts from that preset's halves.
class TransitionEditor final : public juce::Component, private juce::Timer
{
  public:
    TransitionEditor(ProjectMirror& projectMirror, TimelineActions& timelineActions, juce::String outId, juce::String inId);
    ~TransitionEditor() override;

    static void show(ProjectMirror& mirror, TimelineActions& actions, const juce::String& outClipId, const juce::String& inClipId);

    void paint(juce::Graphics&) override;
    void resized() override;
    int graphHeight() const;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;

    class HalfEditor;

    // Smoke-test support: hands a half's parameter to the graph (as its + does) and draws it.
    void drawByHandForTest(bool outgoing, const juce::String& param, const std::vector<automation::Point>& points);

  private:
    void timerCallback() override;
    void changed();
    double windowBars() const;
    juce::Rectangle<int> graphArea() const;
    // The graph's band for each half: the outgoing one on top, the incoming one below.
    juce::Rectangle<float> band(bool outgoing) const;
    juce::Point<float> toGraph(bool outgoing, const juce::String& param, double at, double value) const;
    HalfEditor& editorFor(bool outgoing) const;
    // The curve drawn by hand being edited, and its points.
    std::vector<automation::Point> activePoints() const;
    void setActivePoints(std::vector<automation::Point> points);
    void activate(bool outgoing, const juce::String& param);
    int pointAt(bool outgoing, const juce::String& param, juce::Point<float> p) const;

    ProjectMirror& mirror;
    TimelineActions& actions;
    juce::String outClipId, inClipId;
    mix::Join join;
    juce::Label heading, sub, hint;
    std::unique_ptr<HalfEditor> outEditor, inEditor;
    juce::ToggleButton rampTempo{"Ramp the tempo across the window, from the outgoing track's to the incoming one's"};
    juce::TextButton remove{"Remove"}, close{"Close"};

    bool activeOut = true;
    juce::String activeParam;
    int dragIndex = -1;
    int bendSegment = -1;
    bool bending = false;
    float bendStartY = 0.0f;
};

} // namespace stemcel::native
