#pragma once

#include "EngineClient.h"
#include "LiveState.h"
#include "ProjectModel.h"
#include "TimelineActions.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace stemcel::native
{

// The mixer, docked in the workspace in place of the bottom panel (View > Mixer, F9): a
// channel strip per track (inserts, pan, mute and solo, fader, meter) and the master strip
// (its own plugin inserts, the master fader and meter). Plugins go on any strip's insert
// slots: click a slot for its menu, or "+" to add one from the scanned plugins.
class MixerPanel final : public juce::Component, private juce::ChangeListener, private juce::Timer
{
  public:
    MixerPanel(EngineClient& engine, ProjectMirror& mirror, TimelineActions& actions, TimelineViewState& view, LiveState& live,
               std::function<void(const juce::String&, bool)> notify);
    ~MixerPanel() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    class Strip;

  private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void rebuild();

    EngineClient& engine;
    ProjectMirror& mirror;
    TimelineActions& actions;
    TimelineViewState& view;
    LiveState& live;
    std::function<void(const juce::String&, bool)> notify;
    juce::Viewport viewport;
    juce::Component content;
    juce::OwnedArray<Strip> strips;
    std::unique_ptr<Strip> master;
    juce::StringArray shownTrackIds;
};

} // namespace stemcel::native
