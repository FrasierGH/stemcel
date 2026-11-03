#pragma once

// Mashups (docs/native-app.md, "Mashups and the sample library"): chosen parts of several
// tracks played together. Quick Mashup asks which parts of each track to use (all of it, or
// its vocals, drums, bass, other, guitar, piano) and which track leads; the stems come from
// the library when they are already there, or are separated first with UVR's models (vocals
// alone by Mel-Band RoFormer, anything else by BS-Roformer SW), one job at a time. Then only
// the chosen parts go after the arrangement on their own tracks, on the bar grid, every other
// track shifted into the leader's key, and the key wheel opens on them with that leader so the
// key can be changed. The placing is one undo step.

#include "EngineClient.h"
#include "ProjectModel.h"

#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <map>

namespace stemcel::native
{
class StemService;
class TimelineActions;

class MashupBuilder final : private juce::Timer
{
  public:
    MashupBuilder(EngineClient& engine, ProjectMirror& mirror, TimelineActions& actions, StemService& stems);

    // One track of a mashup: the library item and its parts ("All", or stem labels).
    struct Request
    {
        juce::String itemId;
        juce::StringArray parts;
    };
    static const juce::StringArray& partNames(); // All, Vocals, Drums, Bass, Other, Guitar, Piano

    std::function<void(const juce::String& message, bool isError)> onNotify;
    // Called with the time range and first track of what it placed.
    std::function<void(double fromMs, double toMs, const juce::String& trackId)> onPlaced;
    // Called with a clip per track (the leader's first) and, per clip, the other parts of its
    // track, which follow its key shifts: for the key wheel.
    std::function<void(std::vector<juce::String> clipIds, std::map<juce::String, std::vector<juce::String>> companions)> onKeys;

    // `wheel`: open the key wheel on what was placed.
    void start(std::vector<Request> requests, int leader, bool wheel = true);
    // The vocals of one track over the instrumental of another, the instrumental leading.
    void start(const juce::String& vocalItemId, const juce::String& beatItemId);
    bool busy() const { return !requests.empty(); }

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
    std::vector<Request> requests;
    std::vector<bool> requested;
    int leader = 0;
    bool openWheel = false;
};

// The Quick Mashup dialog: a row per track with its parts and a Leader choice.
namespace quickmashup
{
void show(ProjectMirror& mirror, std::vector<juce::String> itemIds,
          std::function<void(std::vector<MashupBuilder::Request>, int leader)> build);
}

} // namespace stemcel::native
