#pragma once

// Sprig, the workspace's dancer: a little sapling (Stemcel's tree, sprouted) that dances in a
// window of its own on the desktop to whatever plays. It bounces on every beat, throws its leaf arms up in turn, steps,
// and spins or jumps into each new phrase, harder the louder the music is; with nothing
// playing it sways and blinks. Drawn from paths, so it scales with the window. Drag it anywhere
// on screen, out of Stemcel's window too; double-click for another dance; right-click to keep it
// on top or not, or to send it away (View > Dancer brings it back). It hides while Stemcel is
// minimised.

#include "Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace stemcel::native
{

class Dancer final : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
  public:
    struct Beat
    {
        bool playing = false;
        double beats = 0.0; // beats since a downbeat (fractional)
        float level = 0.0F; // the master's peak, 0..1
    };

    Dancer();
    ~Dancer() override;

    std::function<Beat()> beatSource;
    std::function<void()> onHide;     // right-click > Hide
    std::function<void(juce::Point<int>)> onMoved; // dragged: its new top-left (screen)
    std::function<void(bool)> onStayOnTop;         // right-click > Stay on Top changed
    std::function<bool()> ownerMinimised;          // Stemcel's window is minimised: hide meanwhile
    void setStayOnTop(bool on)
    {
        stayOnTop = on;
        setAlwaysOnTop(on);
    }

    int style() const { return dance; }
    void setStyle(int s) { dance = ((s % kStyles) + kStyles) % kStyles; }
    static constexpr int kStyles = 3;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;

    // For tests: the pose it would draw at a moment.
    struct Pose
    {
        float hop = 0.0F;       // up, in body heights
        float squash = 0.0F;    // 0 = round, + = flattened
        float lean = 0.0F;      // radians
        float armL = 0.0F, armR = 0.0F; // radians from hanging down
        float stepL = 0.0F, stepR = 0.0F; // foot lift, 0..1
        float spin = 1.0F;      // horizontal scale (-1..1) while it turns
        float sprout = 0.0F;    // the sprout's sway
        bool blink = false;
    };
    Pose poseAt(const Beat& beat, double seconds) const;
    // One frame: the beat read, the energy eased toward the level (the timer's work, for tests).
    void tick();

  private:
    void timerCallback() override;

    int dance = 0;
    double startedAt = 0.0;
    float energy = 0.0F; // the level, eased
    Beat last;
    juce::ComponentDragger dragger;
    bool dragged = false;
    bool stayOnTop = true;
    bool hiddenWithOwner = false;
};

} // namespace stemcel::native
