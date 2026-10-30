#pragma once

#include "AppPreferences.h"
#include "EngineClient.h"
#include "ProjectModel.h"
#include "TimelineActions.h"
#include "UvrSeparator.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace stemcel::native
{

// Runs separations with UVR's models (uvr::Job, through audio-separator) and turns their
// results into library stems (and, for a timeline clip, one new track per stem aligned
// under it), like the renderer's createStemTracks.
class StemService final
{
  public:
    StemService(EngineClient& engine, ProjectMirror& mirror, TimelineActions& actions, AppPreferences& prefs);
    ~StemService();

    // Opens the picker for a timeline clip (or a library item when clipId is empty).
    void openDialog(const juce::String& sourceItemId, const juce::String& clipId);
    std::unique_ptr<juce::Component> makeDialog(const juce::String& sourceItemId, const juce::String& clipId);
    bool busy() const { return activeJob.isNotEmpty(); }
    void cancel();

    std::function<void(const juce::String&, bool)> onNotify;
    // Progress for a status line: percent < 0 when idle.
    std::function<void(double percent, const juce::String& stage)> onProgress;

    // Splits a clip's stereo channels into mono stems on their own tracks.
    void splitChannels(const juce::String& clipId, const juce::StringArray& channels);

    // A recipe (uvr::recipes()), the model file for the "custom" one, and which of its stems
    // to keep (empty = all).
    void startUvr(const juce::String& sourceItemId, const juce::String& clipId, const juce::String& recipeId,
                  const juce::String& customModel, const juce::StringArray& keep);

  private:
    struct Job
    {
        juce::String sourceItemId, clipId, sourceName, mediaId;
        double sourceInMs = 0.0;
        double startMs = 0.0;
        std::set<juce::String> placed;
    };
    void importStem(Job& job, const juce::String& stem, const juce::File& file);

    // One run of the tool; with `vocalPassAfter`, the recipe's vocal model runs after it on
    // the same audio and takes the vocals.
    void runUvr(const Job& job, const juce::File& input, const juce::File& tempInput, const juce::File& outputDir,
                const uvr::Recipe& run, const juce::StringArray& keep, bool vocalPassAfter);
    void finishUvr(const juce::String& jobId, bool ok, const juce::String& error, std::map<juce::String, juce::File> stems,
                   const juce::StringArray& keep, const juce::File& tempInput);

    EngineClient& engine;
    ProjectMirror& mirror;
    TimelineActions& actions;
    AppPreferences& prefs;
    Subscriptions subscriptions;
    std::map<juce::String, Job> jobs;
    juce::String activeJob;
    std::unique_ptr<uvr::Job> uvrJob;
};

} // namespace stemcel::native
