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

// Stem separation models, on the Electron app's storage (`~/Stemcel/Models/<id>/`, or a
// folder the user located, from the shared preferences), with the same pinned manifests:
// the htdemucs backup (4 files), the Mel-Band RoFormer vocal pack and the BS-RoFormer
// rhythm pack. A model is installed when every file has its expected size; downloads are
// SHA-256 verified and stamped with the `.installed` revision sentinel.
namespace stems
{
struct ModelFile
{
    const char* fileName;
    const char* sha256;
    juce::int64 sizeBytes;
};

struct Model
{
    const char* id;
    const char* displayName;
    const char* repo;
    const char* prefsKey; // `paths.<key>` override in preferences
    std::vector<ModelFile> files;
    juce::int64 totalBytes() const;
};

const Model& backupModel();
const Model& vocalPack();
const Model& rhythmPack();

juce::File modelDir(const Model& model, AppPreferences& prefs);
bool isInstalled(const Model& model, AppPreferences& prefs);

// Downloads a model in the background. Progress (0..1) and completion arrive on the
// message thread. One download at a time per model.
class Downloader final : private juce::Thread
{
  public:
    Downloader(const Model& model, juce::File dir);
    ~Downloader() override;

    std::function<void(double progress, const juce::String& file)> onProgress;
    std::function<void(bool ok, const juce::String& error)> onDone;
    void start() { startThread(); }
    void cancel() { signalThreadShouldExit(); }

  private:
    void run() override;
    const Model& model;
    juce::File dir;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
};
// One model's status, with Download (and progress) when it is missing.
class ModelRow final : public juce::Component
{
  public:
    ModelRow(const Model& model, AppPreferences& prefs);
    void refresh();
    void resized() override;

  private:
    void startDownload();
    const Model& model;
    AppPreferences& prefs;
    juce::Label name, state;
    juce::TextButton download{"Download"};
    double progress = 0.0;
    juce::ProgressBar bar{progress};
    std::unique_ptr<Downloader> downloader;
};
} // namespace stems

// Runs separations and turns their results into library stems (and, for a timeline clip,
// one new track per stem aligned under it), like the renderer's createStemTracks.
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

    void start(const juce::String& sourceItemId, const juce::String& clipId, const juce::StringArray& stemNames,
               const juce::String& quality, const juce::String& dereverb);
    // The same, with UVR's models through audio-separator: a recipe (uvr::recipes()), the
    // model file for the "custom" one, and which of its stems to keep (empty = all).
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
