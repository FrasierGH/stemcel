#pragma once

// Mashup mode (docs/native-app.md, "Mashups and the sample library"): the vocals of one track
// over the instrumental of another. The stems come from the library when they are already
// there, or are separated first with UVR's models (the vocals by Mel-Band RoFormer, then
// drums, bass, guitar, piano and other by BS-Roformer SW), one job at a time; then the
// stems go after the arrangement on their own tracks, warped to the project
// tempo, on the bar grid, with the vocals shifted into the instrumental's key (up to three
// semitones either way). The placing is one undo step.

#include "EngineClient.h"
#include "ProjectModel.h"

#include <juce_events/juce_events.h>

#include <functional>

namespace stemcel::native
{
class StemService;
class TimelineActions;

class MashupBuilder final : private juce::Timer
{
  public:
    MashupBuilder(EngineClient& engine, ProjectMirror& mirror, TimelineActions& actions, StemService& stems);

    std::function<void(const juce::String& message, bool isError)> onNotify;
    // Called with the time range and first track of what it placed.
    std::function<void(double fromMs, double toMs, const juce::String& trackId)> onPlaced;

    void start(const juce::String& vocalItemId, const juce::String& beatItemId);
    bool busy() const { return vocalItem.isNotEmpty(); }

    // A stem of `itemId` already in the library ("Vocals", "Drums", "Bass", "Other"), or empty.
    static juce::String findStem(const ProjectModel& model, const juce::String& itemId, const juce::String& label);

  private:
    void timerCallback() override;
    void place();
    void finish(const juce::String& message, bool isError);

    EngineClient& engine;
    ProjectMirror& mirror;
    TimelineActions& actions;
    StemService& stems;
    juce::String vocalItem, beatItem;
    bool vocalRequested = false, beatRequested = false;
};

} // namespace stemcel::native
