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
// A graph shows the automation it writes. Changes apply as they are made (each burst of
// changes one undo step); opening it on a plain preset starts from that preset's halves.
class TransitionEditor final : public juce::Component, private juce::Timer
{
  public:
    TransitionEditor(ProjectMirror& projectMirror, TimelineActions& timelineActions, juce::String outId, juce::String inId);
    ~TransitionEditor() override;

    static void show(ProjectMirror& mirror, TimelineActions& actions, const juce::String& outClipId, const juce::String& inClipId);

    void paint(juce::Graphics&) override;
    void resized() override;

    class HalfEditor;

  private:
    void timerCallback() override;
    void changed();
    double windowBars() const;
    juce::Rectangle<int> graphArea() const;

    ProjectMirror& mirror;
    TimelineActions& actions;
    juce::String outClipId, inClipId;
    mix::Join join;
    juce::Label heading, sub;
    std::unique_ptr<HalfEditor> outEditor, inEditor;
    juce::ToggleButton rampTempo{"Ramp the tempo across the window, from the outgoing track's to the incoming one's"};
    juce::TextButton remove{"Remove"}, close{"Close"};
};

} // namespace stemcel::native
