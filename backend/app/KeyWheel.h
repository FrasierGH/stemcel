#pragma once

#include "AutoMix.h"
#include "ProjectModel.h"
#include "TimelineActions.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>

namespace stemcel::native
{

// The key wheel for mashups: the Camelot wheel (majors outside, minors inside, each with its
// key name, so it reads as the circle of fifths too), every chosen clip's current key (source
// key plus its shift, however it was made) marked in its colour, and the keys that mix with
// them lit. Under it, a button per clip picks the one a click on the wheel shifts ("All"
// shifts every clip into the clicked key); for several clips, a leader (the dominant track,
// which stays put or shifts least) and the common keys they can all reach, best first.
// Shifts count from each clip's original key, or, ticked, from where it is now. Hovering a
// key shows what would shift; clicking it shifts (one undo step). From the library it shows
// the keys only.
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
    void applyForTest(automix::Camelot target) { apply(target, true); }
    // For tests: the common keys listed (best first), and choosing the leader.
    const std::vector<automix::Camelot>& commonKeysForTest() const { return common; }
    void setLeaderForTest(int index)
    {
        leader = index;
        rebuild();
    }

    // The keys every one of `keys` can reach exactly: those the others reach within 3 semitones
    // while the leader (`leader`, an index) stays as it is; failing any, those all reach within a
    // tritone, ranked by total shift with the leader's counted three times. At most `limit`.
    static std::vector<automix::Camelot> commonKeys(const std::vector<juce::String>& keys, int leader, int limit = 4);

  private:
    struct Subject
    {
        juce::String clipId, name, sourceKey, currentKey;
        juce::Colour colour;
        int shift = 0; // semitones from the source key now
    };
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void rebuild();
    automix::Camelot segmentAt(juce::Point<float> p) const;
    juce::Path segmentPath(automix::Camelot key, float inner, float outer) const;
    void apply(automix::Camelot target, bool all);
    juce::String planFor(automix::Camelot target) const;
    // The key a subject's shift is counted from: its original, or its current with the tickbox.
    juce::String baseKey(const Subject& s) const;
    // The shift from the source key that puts a subject on `target`, if any.
    std::optional<int> targetShift(const Subject& s, automix::Camelot target) const;
    void rebuildControls();
    bool applies(size_t index) const;

    ProjectMirror& mirror;
    TimelineActions& actions;
    std::vector<juce::String> clipIds, itemIds;
    std::vector<Subject> subjects;
    automix::Camelot best, hover;
    juce::Label info;
    juce::OwnedArray<juce::TextButton> clipButtons, commonButtons;
    juce::ComboBox leaderBox;
    juce::ToggleButton fromCurrent{"Shift from the current pitch"};
    juce::Label commonLabel;
    std::vector<automix::Camelot> common;
    int chosen = -1; // the subject a click shifts; -1 for all
    int leader = 0;
    size_t builtFor = 0;
    juce::Point<float> centre;
    float radius = 0.0f;
};

} // namespace stemcel::native
