#include "Stems.h"

#include "AppPaths.h"
#include "Log.h"
#include "ProjectSession.h"
#include "Theme.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace stemcel::native
{
// ── Picker dialog ──────────────────────────────────────────────────────────
namespace
{
juce::var payload(std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();
    for (const auto& [k, v] : props)
        if (!v.isVoid()) o->setProperty(k, v);
    return juce::var(o);
}

class StemDialog final : public juce::Component
{
  public:
    StemDialog(StemService& svc, AppPreferences& preferences, juce::String sourceItem, juce::String clip, juce::String name)
        : service(svc), prefs(preferences), sourceItemId(std::move(sourceItem)), clipId(std::move(clip))
    {
        heading.setText("Separate \"" + name + "\" into stems", juce::dontSendNotification);
        heading.setFont(uiFont(15.0f, true));
        addAndMakeVisible(heading);
        for (const auto& r : uvr::recipes()) recipe.addItem(r.label, recipe.getNumItems() + 1);
        recipe.setSelectedItemIndex(juce::jmax(0, indexOfRecipe(prefs.get("stems", "uvrRecipe", "best").toString())), juce::dontSendNotification);
        recipe.onChange = [this] { layoutForRecipe(); };
        recipe.setComponentID("recipe");
        customModel.setTextToShowWhenEmpty("Model file, e.g. mel_band_roformer_vocals_becruily.ckpt", colours::textFaint);
        customModel.setText(prefs.get("stems", "uvrCustomModel", "").toString(), false);
        for (auto* l : {&recipeHelp, &toolStatus})
        {
            l->setFont(uiFont(11.0f));
            l->setColour(juce::Label::textColourId, colours::textDim);
            l->setJustificationType(juce::Justification::topLeft);
            l->setMinimumHorizontalScale(1.0f); // wrap, never squash
        }
        locate.onClick = [this] { locateTool(); };
        copyInstall.onClick = [this]
        {
            juce::SystemClipboard::copyTextToClipboard(uvr::findExecutable(prefs).existsAsFile() ? uvr::gpuInstallCommand()
                                                                                                 : uvr::installCommand());
        };
        for (auto* c : std::initializer_list<juce::Component*>{&recipe, &recipeHelp, &toolStatus, &locate})
            addAndMakeVisible(c);
        for (auto* c : std::initializer_list<juce::Component*>{&customModel, &copyInstall}) addChildComponent(c);
        layoutForRecipe();

        separate.setColour(juce::TextButton::buttonColourId, colours::sky600);
        separate.onClick = [this] { go(); };
        cancel.onClick = [this] { close(); };
        addAndMakeVisible(separate);
        addAndMakeVisible(cancel);
        setSize(560, 340);
    }

    static int indexOfRecipe(const juce::String& id)
    {
        for (size_t i = 0; i < uvr::recipes().size(); ++i)
            if (uvr::recipes()[i].id == id) return static_cast<int>(i);
        return 0;
    }

    const uvr::Recipe& chosenRecipe() const { return uvr::recipes()[static_cast<size_t>(juce::jmax(0, recipe.getSelectedItemIndex()))]; }

    void layoutForRecipe()
    {
        const auto& r = chosenRecipe();
        customModel.setVisible(r.id == "custom");
        recipeHelp.setText(r.help, juce::dontSendNotification);
        // The stem picker follows what the recipe produces.
        stemToggles.clear();
        for (const auto& key : r.stems)
        {
            auto* t = stemToggles.add(new juce::ToggleButton(uvr::stemLabel(key)));
            t->setComponentID(key);
            t->setToggleState(true, juce::dontSendNotification);
            addAndMakeVisible(t);
        }
        const auto exe = uvr::findExecutable(prefs);
        if (exe.existsAsFile() && !accelerationAsked)
        {
            // Whether the models will run on the GPU: pip's default PyTorch on Windows is CPU-only,
            // which takes minutes per model and looks stuck.
            accelerationAsked = true;
            uvr::checkAcceleration(exe, [safe = juce::Component::SafePointer<StemDialog>(this)](uvr::Acceleration a)
                                   {
                                       if (safe == nullptr) return;
                                       safe->acceleration = a;
                                       safe->layoutForRecipe();
                                   });
        }
        const bool slow = acceleration == uvr::Acceleration::cpu;
        const auto gpuCommand = uvr::gpuInstallCommand();
        const juce::String device =
            acceleration == uvr::Acceleration::gpu   ? juce::String("Runs on the GPU.")
            : acceleration == uvr::Acceleration::cpu ? (gpuCommand.isNotEmpty()
                                                            ? "Runs on the CPU (slow: minutes per model). For your NVIDIA GPU, run this, then reopen the dialog:\n" + gpuCommand
                                                            : juce::String("Runs on the CPU (slow: minutes per model)."))
            : accelerationAsked ? juce::String("Checking whether it can use the GPU...") : juce::String();
        toolStatus.setText(exe.existsAsFile() ? "audio-separator: " + exe.getFullPathName() + "\n" + device
                                              : "audio-separator is not installed. Install it (needs Python 3.10+), then reopen this dialog:\n"
                                                    + uvr::installCommand(),
                           juce::dontSendNotification);
        toolStatus.setColour(juce::Label::textColourId, slow ? colours::amber400 : colours::textDim);
        copyInstall.setButtonText(exe.existsAsFile() ? "Copy GPU command" : "Copy install command");
        copyInstall.setVisible(!exe.existsAsFile() || (slow && gpuCommand.isNotEmpty()));
        resized();
    }

    void locateTool()
    {
        chooser = std::make_unique<juce::FileChooser>("Locate audio-separator", juce::File::getSpecialLocation(juce::File::userHomeDirectory));
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                             [this](const juce::FileChooser& fc)
                             {
                                 if (!fc.getResult().existsAsFile()) return;
                                 prefs.set("stems", "audioSeparatorPath", fc.getResult().getFullPathName());
                                 prefs.save();
                                 layoutForRecipe();
                             });
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(14);
        heading.setBounds(area.removeFromTop(26));
        area.removeFromTop(6);
        recipe.setBounds(area.removeFromTop(26));
        area.removeFromTop(4);
        recipeHelp.setBounds(area.removeFromTop(30));
        if (customModel.isVisible())
        {
            customModel.setBounds(area.removeFromTop(26));
            area.removeFromTop(4);
        }
        auto row = area.removeFromTop(26);
        for (auto* t : stemToggles) t->setBounds(row.removeFromLeft(juce::jmax(80, row.getWidth() / juce::jmax(1, stemToggles.size()))));
        area.removeFromTop(8);
        toolStatus.setBounds(area.removeFromTop(64));
        auto tools = area.removeFromTop(26);
        locate.setBounds(tools.removeFromLeft(110));
        tools.removeFromLeft(6);
        copyInstall.setBounds(tools.removeFromLeft(180));
        auto buttons = area.removeFromBottom(30);
        separate.setBounds(buttons.removeFromRight(110));
        buttons.removeFromRight(6);
        cancel.setBounds(buttons.removeFromRight(90));
    }

    void paint(juce::Graphics& g) override { g.fillAll(colours::background); }

  private:
    void go()
    {
        const auto& r = chosenRecipe();
        juce::StringArray keep;
        for (auto* t : stemToggles)
            if (t->getToggleState()) keep.add(t->getComponentID());
        if (!r.stems.isEmpty() && keep.isEmpty()) return;
        if (!uvr::findExecutable(prefs).existsAsFile())
        {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Install audio-separator",
                                                   "Stem separation runs UVR's models through audio-separator. Install it with:\n\n"
                                                       + uvr::installCommand()
                                                       + "\n\nor use Locate... if it is installed somewhere Stemcel does not look.");
            return;
        }
        prefs.set("stems", "uvrRecipe", r.id);
        prefs.set("stems", "uvrCustomModel", customModel.getText().trim());
        prefs.save();
        service.startUvr(sourceItemId, clipId, r.id, customModel.getText(), keep);
        close();
    }

    void close()
    {
        if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState(0);
    }

    StemService& service;
    AppPreferences& prefs;
    juce::String sourceItemId, clipId;
    juce::Label heading;
    juce::TextButton separate{"Separate"}, cancel{"Cancel"};
    juce::ComboBox recipe;
    juce::Label recipeHelp, toolStatus;
    juce::TextEditor customModel;
    juce::OwnedArray<juce::ToggleButton> stemToggles;
    juce::TextButton locate{"Locate..."}, copyInstall{"Copy install command"};
    uvr::Acceleration acceleration = uvr::Acceleration::unknown;
    bool accelerationAsked = false;
    std::unique_ptr<juce::FileChooser> chooser;
};
} // namespace

// ── Service ────────────────────────────────────────────────────────────────
StemService::StemService(EngineClient& client, ProjectMirror& projectMirror, TimelineActions& timelineActions,
                         AppPreferences& preferences)
    : engine(client), mirror(projectMirror), actions(timelineActions), prefs(preferences), subscriptions(client)
{
    subscriptions.on("CHANNEL_SPLIT_READY",
                     [this](const juce::var& p)
                     {
                         const auto it = jobs.find(p.getProperty("jobId", {}).toString());
                         if (it == jobs.end()) return;
                         if (const auto* list = p.getProperty("channels", {}).getArray())
                             for (const auto& ch : *list)
                                 importStem(it->second, ch.getProperty("channel", {}).toString(), juce::File(ch.getProperty("filePath", {}).toString()));
                         jobs.erase(it);
                     });
    subscriptions.on("CHANNEL_SPLIT_FAILED",
                     [this](const juce::var& p)
                     {
                         jobs.erase(p.getProperty("jobId", {}).toString());
                         if (onNotify) onNotify("Could not split channels: " + p.getProperty("error", {}).toString(), true);
                     });
}

StemService::~StemService() = default;

std::unique_ptr<juce::Component> StemService::makeDialog(const juce::String& sourceItemId, const juce::String& clipId)
{
    const auto* item = mirror.model().findLibraryItem(sourceItemId);
    if (item == nullptr) return {};
    return std::make_unique<StemDialog>(*this, prefs, sourceItemId, clipId, item->displayName());
}

void StemService::openDialog(const juce::String& sourceItemId, const juce::String& clipId)
{
    if (busy())
    {
        if (onNotify) onNotify("A stem separation is already running.", true);
        return;
    }
    auto dialog = makeDialog(sourceItemId, clipId);
    if (dialog == nullptr) return;
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(dialog.release());
    options.dialogTitle = "Separate Stems";
    options.dialogBackgroundColour = colours::background;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}

void StemService::startUvr(const juce::String& sourceItemId, const juce::String& clipId, const juce::String& recipeId,
                           const juce::String& customModel, const juce::StringArray& keep)
{
    const auto* item = mirror.model().findLibraryItem(sourceItemId);
    const auto* recipe = uvr::findRecipe(recipeId);
    if (item == nullptr || recipe == nullptr)
    {
        log::warn("stems", "UVR: no library item " + sourceItemId + " or recipe " + recipeId);
        return;
    }
    if (!uvr::findExecutable(prefs).existsAsFile())
    {
        if (onNotify) onNotify("audio-separator is not installed. Install it with: " + uvr::installCommand(), true);
        return;
    }
    auto run = *recipe;
    if (run.id == "custom")
    {
        if (customModel.trim().isEmpty()) return;
        run.model = customModel.trim();
    }
    Job job;
    job.sourceItemId = sourceItemId;
    job.clipId = clipId;
    job.sourceName = item->displayName();
    job.mediaId = item->mediaId;
    juce::File input(item->audioPath());
    juce::File tempInput;
    if (const auto* c = mirror.model().findClip(clipId))
    {
        // The stems are of the clip's window, placed where the clip starts: hand the tool
        // just that window.
        job.startMs = c->offsetMs;
        job.sourceInMs = c->inMs;
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        if (std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(input)); reader != nullptr)
        {
            tempInput = systemTempDirectory().getChildFile(appFolderName()).getChildFile("uvr-" + juce::Uuid().toDashedString() + ".wav");
            tempInput.getParentDirectory().createDirectory();
            const auto start = static_cast<juce::int64>(c->inMs / 1000.0 * reader->sampleRate);
            const auto length = juce::jmin(reader->lengthInSamples - start, static_cast<juce::int64>(c->durationMs / 1000.0 * reader->sampleRate));
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(tempInput);
            const auto options = juce::AudioFormatWriterOptions{}.withSampleRate(reader->sampleRate)
                                     .withNumChannels(static_cast<int>(reader->numChannels)).withBitsPerSample(24);
            std::unique_ptr<juce::AudioFormatWriter> writer(juce::WavAudioFormat().createWriterFor(stream, options));
            if (writer == nullptr || length <= 0 || !writer->writeFromAudioReader(*reader, start, length))
            {
                if (onNotify) onNotify("Could not prepare the clip's audio for separation", true);
                tempInput.deleteFile();
                return;
            }
            writer.reset();
            input = tempInput;
        }
    }
    if (!input.existsAsFile())
    {
        if (onNotify) onNotify("Source audio not found for " + job.sourceName, true);
        return;
    }
    // In the project's stems folder, so the library's file clean-up covers them.
    auto folder = juce::File::createLegalFileName(juce::File(item->filePath).getFileNameWithoutExtension()).trim();
    if (folder.isEmpty()) folder = "stems";
    const auto base = projectArtifactsBaseDir(mirror.model().filePath, "stems");
    auto outputDir = base.getChildFile(folder + "-stems");
    for (int n = 2; outputDir.exists(); ++n) outputDir = base.getChildFile(folder + "-stems-" + juce::String(n));

    // Best of both: a vocal model takes the vocals (it scores higher on them), so the first
    // model's are dropped and the vocal model runs on the same audio after it.
    const bool vocalPass = run.vocalModel.isNotEmpty() && (keep.isEmpty() || keep.contains("vocals"));
    auto firstKeep = keep;
    if (vocalPass)
    {
        if (firstKeep.isEmpty()) firstKeep = run.stems;
        firstKeep.removeString("vocals");
    }
    runUvr(job, input, tempInput, outputDir, run, firstKeep, vocalPass);
}

void StemService::runUvr(const Job& job, const juce::File& input, const juce::File& tempInput, const juce::File& outputDir,
                         const uvr::Recipe& run, const juce::StringArray& keep, bool vocalPassAfter)
{
    const auto jobId = juce::Uuid().toDashedString();
    jobs[jobId] = job;
    activeJob = jobId;
    uvrJob = std::make_unique<uvr::Job>(uvr::findExecutable(prefs), input, outputDir, run);
    uvrJob->onProgress = [this](double percent, const juce::String& stage)
    { if (onProgress) onProgress(percent, stage == "download" ? juce::String("download model") : juce::String("separate (UVR)")); };
    uvrJob->onDone = [this, jobId, job, keep, input, tempInput, outputDir, run, vocalPassAfter](
                         bool ok, const juce::String& error, std::map<juce::String, juce::File> stems)
    {
        // The window written for a clip stays until the vocal pass has read it too.
        finishUvr(jobId, ok, error, std::move(stems), keep, vocalPassAfter ? juce::File() : tempInput);
        const bool sourceStill = mirror.model().findLibraryItem(job.sourceItemId) != nullptr
                                 && (job.clipId.isEmpty() || mirror.model().findClip(job.clipId) != nullptr);
        if (!vocalPassAfter) return;
        if (!ok || !sourceStill)
        {
            tempInput.deleteFile();
            return;
        }
        uvr::Recipe vocals;
        vocals.id = "vocalPass";
        vocals.label = "Vocals";
        vocals.model = run.vocalModel;
        vocals.stems = {"vocals", "other"};
        // Its own folder: the vocal model also writes an "other", which must not land on the
        // first model's.
        const auto vocalDir = outputDir.getSiblingFile(outputDir.getFileName() + "-vocals");
        juce::MessageManager::callAsync([this, job, input, tempInput, vocalDir, vocals]
                                        { runUvr(job, input, tempInput, vocalDir, vocals, {"vocals"}, false); });
    };
    log::info("stems", "UVR job " + jobId + " recipe=" + run.id + " model=" + (run.preset.isNotEmpty() ? run.preset : run.model));
    if (onProgress) onProgress(0.0, "prepare");
    uvrJob->start();
}

void StemService::finishUvr(const juce::String& jobId, bool ok, const juce::String& error, std::map<juce::String, juce::File> stems,
                            const juce::StringArray& keep, const juce::File& tempInput)
{
    tempInput.deleteFile();
    const auto it = jobs.find(jobId);
    if (activeJob == jobId) activeJob.clear();
    if (onProgress) onProgress(-1.0, {});
    if (it == jobs.end()) return;
    if (!ok)
    {
        jobs.erase(it);
        if (onNotify) onNotify(error == "cancelled" ? juce::String("Stem separation cancelled") : "UVR separation failed: " + error, error != "cancelled");
        juce::MessageManager::callAsync([this] { uvrJob.reset(); });
        return;
    }
    for (const auto& [key, file] : stems)
    {
        if (!keep.isEmpty() && !keep.contains(key))
        {
            file.deleteFile(); // not wanted: no stray files in the project folder
            continue;
        }
        importStem(it->second, key, file);
    }
    const int count = static_cast<int>(it->second.placed.size());
    if (onNotify)
        onNotify(count > 0 ? (it->second.clipId.isNotEmpty() ? "Added " : "Extracted ") + juce::String(count) + (count == 1 ? " stem from " : " stems from ") + it->second.sourceName
                           : "Could not create stems from " + it->second.sourceName,
                 count == 0);
    jobs.erase(it);
    juce::MessageManager::callAsync([this] { uvrJob.reset(); });
}

void StemService::cancel()
{
    if (uvrJob != nullptr) uvrJob->cancel();
}

void StemService::splitChannels(const juce::String& clipId, const juce::StringArray& channels)
{
    const auto* c = mirror.model().findClip(clipId);
    const auto* item = c != nullptr ? mirror.model().findLibraryItem(c->libraryItemId) : nullptr;
    if (item == nullptr || channels.isEmpty()) return;
    Job job;
    job.sourceItemId = item->id;
    job.clipId = clipId;
    job.sourceName = c->name.isNotEmpty() ? c->name : item->displayName();
    job.mediaId = item->mediaId;
    job.startMs = c->offsetMs;
    job.sourceInMs = c->inMs;
    const auto jobId = juce::Uuid().toDashedString();
    jobs[jobId] = job;
    juce::Array<juce::var> list;
    for (const auto& ch : channels) list.add(ch);
    engine.send("CLIP_SPLIT_CHANNELS", payload({{"jobId", jobId}, {"clipId", clipId}, {"sourceName", job.sourceName}, {"channels", list}}));
    if (onNotify) onNotify("Splitting channels of " + job.sourceName, false);
}

void StemService::importStem(Job& job, const juce::String& stem, const juce::File& file)
{
    if (stem.isEmpty() || job.placed.count(stem) != 0 || !file.existsAsFile()) return;
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr) return;
    job.placed.insert(stem);
    const auto label = stem == "left" ? juce::String("Left") : stem == "right" ? juce::String("Right") : uvr::stemLabel(stem);
    const auto name = label + juce::String(juce::CharPointer_UTF8(" \xe2\x80\x94 ")) + job.sourceName;
    int next = 1;
    for (const auto& item : mirror.model().library)
        if (item.id.startsWithChar('l') && item.id.substring(1).containsOnly("0123456789"))
            next = juce::jmax(next, item.id.substring(1).getIntValue() + 1);
    const auto itemId = "l" + juce::String(next);
    engine.inUndoGroup("Add " + label + " stem",
                       [&]
                       {
                           engine.send("LIBRARY_ADD", payload({{"itemId", itemId},
                                                               {"filePath", file.getFullPathName()},
                                                               {"fileName", file.getFileName()},
                                                               {"kind", "stem"},
                                                               {"name", name},
                                                               {"durationMs", static_cast<double>(reader->lengthInSamples) * 1000.0 / reader->sampleRate},
                                                               {"sampleRate", static_cast<int>(reader->sampleRate)},
                                                               {"channelCount", static_cast<int>(reader->numChannels)},
                                                               {"sourceItemId", job.sourceItemId},
                                                               {"sourceClipId", job.clipId.isNotEmpty() ? juce::var(job.clipId) : juce::var()},
                                                               {"sourceInMs", job.sourceInMs},
                                                               {"sourceDurationMs", 0.0},
                                                               {"mediaId", job.mediaId.isNotEmpty() ? juce::var(job.mediaId) : juce::var()}}));
                           mirror.refreshNow();
                           if (job.clipId.isEmpty()) return;
                           const auto trackId = actions.addTrack();
                           actions.renameTrack(trackId, name);
                           mirror.refreshNow();
                           if (const auto* item = mirror.model().findLibraryItem(itemId)) actions.addClip(trackId, *item, job.startMs);
                       });
}

} // namespace stemcel::native
