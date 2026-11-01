#pragma once

#include "AutoMix.h"
#include "ProjectModel.h"
#include "TimelineActions.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>

namespace stemcel::native
{

// The key wheel for mashups: the Camelot wheel (majors outside, minors inside, each with its
// key name, so it reads as the circle of fifths too), every chosen clip's key marked in its
// colour, the keys that mix with them lit, and, for several clips, the key they can all reach
// with the least pitch shift lit brightest. Hovering a key shows what each clip would shift;
// clicking it shifts them (one undo step). From the library it shows the keys only.
class KeyWheel final : public juce::Component, private juce::ChangeListener
{
  public:
    KeyWheel(ProjectMirror& mirror, TimelineActions& actions, std::vector<juce::String> clipIds, std::vector<juce::String> itemIds);
    ~KeyWheel() override;
    static void show(ProjectMirror& mirror, TimelineActions& actions, std::vector<juce::String> clipIds,
                     std::vector<juce::String> itemIds);

    // The smallest shift (semitones from its source key) within `maxShift` that puts `sourceKey`
    // on `target` (a pitch shift keeps major or minor, so only keys of its mode are reached).
    static std::optional<int> shiftTo(const juce::String& sourceKey, automix::Camelot target, int maxShift);
    // For several keys, the one they can all reach with the least total shift (each within 3
    // semitones); invalid when there is none.
    static automix::Camelot recommended(const std::vector<juce::String>& sourceKeys);

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;

    // For tests: shifts the clips to `target` as a click on it would.
    void applyForTest(automix::Camelot target) { apply(target); }

  private:
    struct Subject
    {
        juce::String clipId, name, sourceKey, currentKey;
        juce::Colour colour;
    };
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void rebuild();
    automix::Camelot segmentAt(juce::Point<float> p) const;
    juce::Path segmentPath(automix::Camelot key, float inner, float outer) const;
    void apply(automix::Camelot target);
    juce::String planFor(automix::Camelot target) const;

    ProjectMirror& mirror;
    TimelineActions& actions;
    std::vector<juce::String> clipIds, itemIds;
    std::vector<Subject> subjects;
    automix::Camelot best, hover;
    juce::Label info;
    juce::Point<float> centre;
    float radius = 0.0f;
};

} // namespace stemcel::native
