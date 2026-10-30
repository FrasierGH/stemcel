#include "Stems.h"

#include "AppPaths.h"
#include "Log.h"
#include "ProjectSession.h"
#include "Theme.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>

namespace stemcel::native
{
namespace stems
{
namespace
{
constexpr const char* kRevision = "main";
constexpr const char* kSentinel = ".installed";

juce::String urlFor(const Model& model, const ModelFile& file)
{
    return juce::String("https://huggingface.co/") + model.repo + "/resolve/" + kRevision + "/" + file.fileName;
}
} // namespace

juce::int64 Model::totalBytes() const
{
    juce::int64 total = 0;
    for (const auto& f : files) total += f.sizeBytes;
    return total;
}

const Model& backupModel()
{
    static const Model m{"htdemucs-ft", "Backup separation model (htdemucs, 4-stem)", "legacydaw/htdemucs-ft-onnx", "stemModelDir",
                         {{"htdemucs_ft_vocals.onnx", "8c5d5e2da1f27050240bb80236673307ee3b40d4b064066d9350f4d64bfd544d", 316446953},
                          {"htdemucs_ft_drums.onnx", "f76b68af36066e38885b369299b5032a861038f9b49da5aa6cf1c31cfa69cf27", 316446953},
                          {"htdemucs_ft_bass.onnx", "2a74d9283fc2336fcc58d50f87a7080aff57aea372f65cfe3f0211ea1ff16182", 316446953},
                          {"htdemucs_ft_other.onnx", "90e11806c1bb558ca9d9c7e909d28a2854f7f217982e90482dbed6442513daad", 316446953}}};
    return m;
}

const Model& vocalPack()
{
    static const Model m{"mel-band-roformer-vocals", "Vocal Quality Pack (Mel-Band RoFormer)",
                         "legacydaw/mel-band-roformer-vocals-onnx", "vocalPackDir",
                         {{"syhft_core_folded_fp16_webgpu.onnx", "dde2bfe8f85d2c12efa24ce4d45cc13e8709b8a72e277a93f130d496d948e918", 5308300},
                          {"syhft_core_folded_fp16_webgpu.onnx.data", "b08cfc80905e3560a4dd5d30f641299a47dd96d309ebbe9524d9d6c9d2a0356f", 741190540}}};
    return m;
}

const Model& rhythmPack()
{
    static const Model m{"bs-roformer-rhythm", "Rhythm Quality Pack (BS-RoFormer)", "legacydaw/bs-roformer-rhythm-onnx",
                         "rhythmPackDir",
                         {{"bs_roformer_4stem_rhythm_fp16.onnx", "4bac38e42c085b5d69964e2b7ff161318a1315122c10b025c718b20e94d85d6b", 269457840}}};
    return m;
}

juce::File modelDir(const Model& model, AppPreferences& prefs)
{
    const auto located = prefs.get("paths", model.prefsKey, {}).toString();
    if (located.isNotEmpty() && juce::File::isAbsolutePath(located) && juce::File(located).isDirectory()) return juce::File(located);
    return juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(appFolderName()).getChildFile("Models").getChildFile(model.id);
}

bool isInstalled(const Model& model, AppPreferences& prefs)
{
    const auto dir = modelDir(model, prefs);
    for (const auto& f : model.files)
        if (dir.getChildFile(f.fileName).getSize() != f.sizeBytes) return false;
    // Present at the right sizes: stamp the sentinel as the Electron store's reconcile does.
    if (dir.getChildFile(kSentinel).loadFileAsString() != kRevision) dir.getChildFile(kSentinel).replaceWithText(kRevision);
    return true;
}

Downloader::Downloader(const Model& m, juce::File d) : juce::Thread("stem model download"), model(m), dir(std::move(d)) {}

Downloader::~Downloader()
{
    alive->store(false);
    stopThread(10000);
}

void Downloader::run()
{
    auto fail = [this](const juce::String& error)
    {
        juce::MessageManager::callAsync([alive = alive, cb = onDone, error] { if (alive->load() && cb) cb(false, error); });
    };
    if (!dir.createDirectory())
    {
        fail("Can't create " + dir.getFullPathName());
        return;
    }
    const double total = static_cast<double>(model.totalBytes());
    juce::int64 done = 0;
    for (const auto& f : model.files)
    {
        const auto target = dir.getChildFile(f.fileName);
        if (target.getSize() == f.sizeBytes)
        {
            done += f.sizeBytes;
            continue;
        }
        const auto part = dir.getChildFile(juce::String(f.fileName) + ".part");
        part.deleteFile();
        int status = 0;
        auto stream = juce::URL(urlFor(model, f))
                          .createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                                                 .withConnectionTimeoutMs(30000)
                                                 .withStatusCode(&status)
                                                 .withNumRedirectsToFollow(10));
        if (stream == nullptr || (status != 0 && status >= 400))
        {
            fail("Download failed for " + juce::String(f.fileName) + " (HTTP " + juce::String(status) + ")");
            return;
        }
        {
            juce::FileOutputStream out(part);
            if (!out.openedOk())
            {
                fail("Can't write " + part.getFullPathName());
                return;
            }
            juce::HeapBlock<char> buffer(1 << 20);
            juce::int64 fileDone = 0;
            double lastReport = 0.0;
            while (!stream->isExhausted())
            {
                if (threadShouldExit())
                {
                    out.flush();
                    part.deleteFile();
                    fail("Download cancelled");
                    return;
                }
                const int n = stream->read(buffer, 1 << 20);
                if (n <= 0) break;
                out.write(buffer, static_cast<size_t>(n));
                fileDone += n;
                const double progress = (static_cast<double>(done + fileDone)) / total;
                if (progress - lastReport > 0.002)
                {
                    lastReport = progress;
                    juce::MessageManager::callAsync([alive = alive, cb = onProgress, progress, name = juce::String(f.fileName)]
                                                    { if (alive->load() && cb) cb(progress, name); });
                }
            }
            out.flush();
        }
        if (part.getSize() != f.sizeBytes)
        {
            part.deleteFile();
            fail(juce::String(f.fileName) + " is incomplete (" + juce::String(part.getSize()) + " bytes)");
            return;
        }
        // Integrity: the pinned SHA-256.
        juce::FileInputStream in(part);
        const auto digest = juce::SHA256(in).toHexString();
        if (!digest.equalsIgnoreCase(f.sha256))
        {
            part.deleteFile();
            fail(juce::String(f.fileName) + " failed its integrity check");
            return;
        }
        target.deleteFile();
        if (!part.moveFileTo(target))
        {
            fail("Can't finish " + target.getFullPathName());
            return;
        }
        done += f.sizeBytes;
    }
    dir.getChildFile(kSentinel).replaceWithText(kRevision);
    juce::MessageManager::callAsync([alive = alive, cb = onDone] { if (alive->load() && cb) cb(true, {}); });
}

ModelRow::ModelRow(const Model& m, AppPreferences& preferences) : model(m), prefs(preferences)
{
    name.setText(model.displayName, juce::dontSendNotification);
    name.setFont(uiFont(12.0f, true));
    addAndMakeVisible(name);
    state.setFont(uiFont(11.0f));
    state.setColour(juce::Label::textColourId, colours::textDim);
    addAndMakeVisible(state);
    download.onClick = [this] { startDownload(); };
    addAndMakeVisible(download);
    addChildComponent(bar);
    refresh();
}

void ModelRow::refresh()
{
    const bool installed = isInstalled(model, prefs);
    state.setText(installed ? "Installed in " + modelDir(model, prefs).getFullPathName()
                            : "Not installed (" + juce::File::descriptionOfSizeInBytes(model.totalBytes()) + ")",
                  juce::dontSendNotification);
    download.setVisible(!installed && downloader == nullptr);
}

void ModelRow::resized()
{
    auto area = getLocalBounds();
    download.setBounds(area.removeFromRight(100).reduced(0, 6));
    name.setBounds(area.removeFromTop(18));
    state.setBounds(area);
    bar.setBounds(area.reduced(0, 2));
}

void ModelRow::startDownload()
{
    downloader = std::make_unique<Downloader>(model, modelDir(model, prefs));
    downloader->onProgress = [this](double p, const juce::String&) { progress = p; };
    downloader->onDone = [this](bool ok, const juce::String& error)
    {
        bar.setVisible(false);
        state.setVisible(true);
        downloader.reset();
        refresh();
        if (!ok) state.setText(error, juce::dontSendNotification);
    };
    download.setVisible(false);
    state.setVisible(false);
    bar.setVisible(true);
    downloader->start();
}
} // namespace stems

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

const juce::StringArray kStemNames{"vocals", "drums", "bass", "other"};
const juce::StringArray kStemLabels{"Vocals", "Drums", "Bass", "Other"};

class StemDialog final : public juce::Component
{
  public:
    StemDialog(StemService& svc, AppPreferences& preferences, juce::String sourceItem, juce::String clip, juce::String name)
        : service(svc), prefs(preferences), sourceItemId(std::move(sourceItem)), clipId(std::move(clip))
    {
        heading.setText("Separate \"" + name + "\" into stems", juce::dontSendNotification);
        heading.setFont(uiFont(15.0f, true));
        addAndMakeVisible(heading);
        for (int i = 0; i < kStemNames.size(); ++i)
        {
            auto* t = toggles.add(new juce::ToggleButton(kStemLabels[i]));
            t->setToggleState(true, juce::dontSendNotification);
            addAndMakeVisible(t);
        }
        quality.addItem("Fast", 1);
        quality.addItem("Balanced", 2);
        quality.addItem("Best", 3);
        const auto q = prefs.get("stems", "quality", "balanced").toString();
        quality.setSelectedId(q == "fast" ? 1 : q == "best" ? 3 : 2, juce::dontSendNotification);
        addAndMakeVisible(quality);
        dereverb.addItem("No vocal dereverb", 1);
        dereverb.addItem("Dereverb vocals: light", 2);
        dereverb.addItem("Dereverb vocals: medium", 3);
        dereverb.addItem("Dereverb vocals: strong", 4);
        dereverb.setSelectedId(1, juce::dontSendNotification);
        addAndMakeVisible(dereverb);
        gpu.setButtonText("Use GPU acceleration");
        gpu.setToggleState(static_cast<bool>(prefs.get("stems", "useGpu", false)), juce::dontSendNotification);
        addAndMakeVisible(gpu);

        for (const auto* model : {&stems::backupModel(), &stems::vocalPack(), &stems::rhythmPack()})
        {
            auto* row = modelRows.add(new stems::ModelRow(*model, prefs));
            addAndMakeVisible(row);
        }
        // UVR's models through audio-separator.
        engineChoice.addItem("Built-in models", 1);
        engineChoice.addItem("UVR models (audio-separator)", 2);
        engineChoice.setSelectedId(prefs.get("stems", "engine", "builtin").toString() == "uvr" ? 2 : 1, juce::dontSendNotification);
        engineChoice.onChange = [this] { layoutForEngine(); };
        engineChoice.setComponentID("engine");
        addAndMakeVisible(engineChoice);
        for (const auto& r : uvr::recipes()) recipe.addItem(r.label, recipe.getNumItems() + 1);
        recipe.setSelectedItemIndex(juce::jmax(0, indexOfRecipe(prefs.get("stems", "uvrRecipe", "best").toString())), juce::dontSendNotification);
        recipe.onChange = [this] { layoutForEngine(); };
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
        for (auto* c : std::initializer_list<juce::Component*>{&recipe, &recipeHelp, &customModel, &toolStatus, &locate, &copyInstall})
            addChildComponent(c);
        layoutForEngine();

        separate.setColour(juce::TextButton::buttonColourId, colours::sky600);
        separate.onClick = [this] { go(); };
        cancel.onClick = [this] { close(); };
        addAndMakeVisible(separate);
        addAndMakeVisible(cancel);
        setSize(560, 460);
    }

    static int indexOfRecipe(const juce::String& id)
    {
        for (size_t i = 0; i < uvr::recipes().size(); ++i)
            if (uvr::recipes()[i].id == id) return static_cast<int>(i);
        return 0;
    }

    const uvr::Recipe& chosenRecipe() const { return uvr::recipes()[static_cast<size_t>(juce::jmax(0, recipe.getSelectedItemIndex()))]; }
    bool usingUvr() const { return engineChoice.getSelectedId() == 2; }

    void layoutForEngine()
    {
        const bool u = usingUvr();
        for (juce::Component* c : std::initializer_list<juce::Component*>{&quality, &dereverb, &gpu}) c->setVisible(!u);
        for (auto* r : modelRows) r->setVisible(!u);
        for (juce::Component* c : std::initializer_list<juce::Component*>{&recipe, &recipeHelp, &toolStatus}) c->setVisible(u);
        const auto& r = chosenRecipe();
        customModel.setVisible(u && r.id == "custom");
        recipeHelp.setText(r.help, juce::dontSendNotification);
        // The stem picker follows what the engine or recipe produces.
        uvrToggles.clear();
        if (u)
            for (const auto& key : r.stems)
            {
                auto* t = uvrToggles.add(new juce::ToggleButton(uvr::stemLabel(key)));
                t->setComponentID(key);
                t->setToggleState(true, juce::dontSendNotification);
                addAndMakeVisible(t);
            }
        for (auto* t : toggles) t->setVisible(!u);
        const auto exe = uvr::findExecutable(prefs);
        if (u && exe.existsAsFile() && !accelerationAsked)
        {
            // Whether the models will run on the GPU: pip's default PyTorch on Windows is CPU-only,
            // which takes minutes per model and looks stuck.
            accelerationAsked = true;
            uvr::checkAcceleration(exe, [safe = juce::Component::SafePointer<StemDialog>(this)](uvr::Acceleration a)
                                   {
                                       if (safe == nullptr) return;
                                       safe->acceleration = a;
                                       safe->layoutForEngine();
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
        locate.setVisible(u);
        copyInstall.setButtonText(exe.existsAsFile() ? "Copy GPU command" : "Copy install command");
        copyInstall.setVisible(u && (!exe.existsAsFile() || (slow && gpuCommand.isNotEmpty())));
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
                                 layoutForEngine();
                             });
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(14);
        heading.setBounds(area.removeFromTop(26));
        area.removeFromTop(6);
        engineChoice.setBounds(area.removeFromTop(26).removeFromLeft(300));
        area.removeFromTop(6);
        if (usingUvr())
        {
            recipe.setBounds(area.removeFromTop(26));
            area.removeFromTop(4);
            recipeHelp.setBounds(area.removeFromTop(30));
            if (customModel.isVisible())
            {
                customModel.setBounds(area.removeFromTop(26));
                area.removeFromTop(4);
            }
            auto row = area.removeFromTop(26);
            for (auto* t : uvrToggles) t->setBounds(row.removeFromLeft(juce::jmax(80, row.getWidth() / juce::jmax(1, uvrToggles.size()))));
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
            return;
        }
        auto stemsRow = area.removeFromTop(26);
        for (auto* t : toggles) t->setBounds(stemsRow.removeFromLeft(110));
        area.removeFromTop(6);
        quality.setBounds(area.removeFromTop(26).removeFromLeft(240));
        area.removeFromTop(4);
        dereverb.setBounds(area.removeFromTop(26).removeFromLeft(240));
        area.removeFromTop(4);
        gpu.setBounds(area.removeFromTop(24));
        area.removeFromTop(8);
        for (auto* r : modelRows)
        {
            r->setBounds(area.removeFromTop(40));
            area.removeFromTop(4);
        }
        auto buttons = area.removeFromBottom(30);
        separate.setBounds(buttons.removeFromRight(110));
        buttons.removeFromRight(6);
        cancel.setBounds(buttons.removeFromRight(90));
    }

    void paint(juce::Graphics& g) override { g.fillAll(colours::background); }

  private:

    void go()
    {
        prefs.set("stems", "engine", usingUvr() ? "uvr" : "builtin");
        if (usingUvr())
        {
            const auto& r = chosenRecipe();
            juce::StringArray keep;
            for (auto* t : uvrToggles)
                if (t->getToggleState()) keep.add(t->getComponentID());
            if (!r.stems.isEmpty() && keep.isEmpty()) return;
            if (!uvr::findExecutable(prefs).existsAsFile())
            {
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Install audio-separator",
                                                       "UVR's models run through audio-separator. Install it with:\n\n" + uvr::installCommand()
                                                           + "\n\nor use Locate... if it is installed somewhere Stemcel does not look.");
                return;
            }
            prefs.set("stems", "uvrRecipe", r.id);
            prefs.set("stems", "uvrCustomModel", customModel.getText().trim());
            prefs.save();
            service.startUvr(sourceItemId, clipId, r.id, customModel.getText(), keep);
            close();
            return;
        }
        juce::StringArray chosen;
        for (int i = 0; i < toggles.size(); ++i)
            if (toggles[i]->getToggleState()) chosen.add(kStemNames[i]);
        if (chosen.isEmpty()) return;
        if (!stems::isInstalled(stems::backupModel(), prefs) && !stems::isInstalled(stems::vocalPack(), prefs)
            && !stems::isInstalled(stems::rhythmPack(), prefs))
        {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Download a model first",
                                                   "Stem separation needs at least one model. Download the backup model or the quality packs above.");
            return;
        }
        const auto q = quality.getSelectedId() == 1 ? "fast" : quality.getSelectedId() == 3 ? "best" : "balanced";
        prefs.set("stems", "quality", q);
        prefs.set("stems", "useGpu", gpu.getToggleState());
        prefs.save();
        const juce::StringArray strengths{"", "light", "medium", "strong"};
        service.start(sourceItemId, clipId, chosen, q, strengths[juce::jmax(0, dereverb.getSelectedId() - 1)]);
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
    juce::OwnedArray<juce::ToggleButton> toggles;
    juce::ComboBox quality, dereverb;
    juce::ToggleButton gpu;
    juce::OwnedArray<stems::ModelRow> modelRows;
    juce::TextButton separate{"Separate"}, cancel{"Cancel"};
    juce::ComboBox engineChoice, recipe;
    juce::Label recipeHelp, toolStatus;
    juce::TextEditor customModel;
    juce::OwnedArray<juce::ToggleButton> uvrToggles;
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
    subscriptions.on("STEM_PROGRESS",
                     [this](const juce::var& p)
                     {
                         if (p.getProperty("jobId", {}).toString() != activeJob) return;
                         if (onProgress) onProgress(static_cast<double>(p.getProperty("percent", 0.0)), p.getProperty("stage", {}).toString());
                     });
    auto stemFile = [this](const juce::var& p)
    {
        const auto it = jobs.find(p.getProperty("jobId", {}).toString());
        if (it == jobs.end()) return;
        importStem(it->second, p.getProperty("stem", {}).toString(), juce::File(p.getProperty("filePath", {}).toString()));
    };
    subscriptions.on("STEM_PARTIAL", stemFile);
    subscriptions.on("STEM_READY",
                     [this](const juce::var& p)
                     {
                         const auto jobId = p.getProperty("jobId", {}).toString();
                         const auto it = jobs.find(jobId);
                         if (it == jobs.end()) return;
                         if (const auto* list = p.getProperty("stems", {}).getArray())
                             for (const auto& s : *list)
                                 importStem(it->second, s.getProperty("stem", {}).toString(), juce::File(s.getProperty("filePath", {}).toString()));
                         const int count = static_cast<int>(it->second.placed.size());
                         if (onNotify)
                             onNotify(count > 0 ? (it->second.clipId.isNotEmpty() ? "Added " : "Extracted ") + juce::String(count) + (count == 1 ? " stem from " : " stems from ")
                                                      + it->second.sourceName
                                                : "Could not create stems from " + it->second.sourceName,
                                      count == 0);
                         jobs.erase(it);
                         if (activeJob == jobId) activeJob.clear();
                         if (onProgress) onProgress(-1.0, {});
                     });
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
    subscriptions.on("STEM_FAILED",
                     [this](const juce::var& p)
                     {
                         const auto jobId = p.getProperty("jobId", {}).toString();
                         jobs.erase(jobId);
                         if (activeJob == jobId) activeJob.clear();
                         if (onProgress) onProgress(-1.0, {});
                         if (onNotify) onNotify("Stem separation failed: " + p.getProperty("error", "unknown error").toString(), true);
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

void StemService::start(const juce::String& sourceItemId, const juce::String& clipId, const juce::StringArray& stemNames,
                        const juce::String& quality, const juce::String& dereverbStrength)
{
    const auto* item = mirror.model().findLibraryItem(sourceItemId);
    if (item == nullptr) return;
    Job job;
    job.sourceItemId = sourceItemId;
    job.clipId = clipId;
    job.sourceName = item->displayName();
    job.mediaId = item->mediaId;
    if (const auto* c = mirror.model().findClip(clipId))
    {
        // The stems are of the clip's window, placed where the clip starts.
        job.startMs = c->offsetMs;
        job.sourceInMs = c->inMs;
    }
    const auto jobId = juce::Uuid().toDashedString();
    jobs[jobId] = job;
    activeJob = jobId;

    const bool useBackup = static_cast<bool>(prefs.get("stems", "useBackupModel", false));
    const auto vocalPath = !useBackup && stems::isInstalled(stems::vocalPack(), prefs)
                               ? stems::modelDir(stems::vocalPack(), prefs).getChildFile(stems::vocalPack().files[0].fileName).getFullPathName()
                               : juce::String();
    const auto rhythmPath = !useBackup && stems::isInstalled(stems::rhythmPack(), prefs)
                                ? stems::modelDir(stems::rhythmPack(), prefs).getChildFile(stems::rhythmPack().files[0].fileName).getFullPathName()
                                : juce::String();
    juce::Array<juce::var> names;
    for (const auto& s : stemNames) names.add(s);
    auto p = payload({{"jobId", jobId},
                      {"sourceItemId", sourceItemId},
                      {"clipId", clipId.isNotEmpty() ? juce::var(clipId) : juce::var()},
                      {"modelDir", stems::modelDir(stems::backupModel(), prefs).getFullPathName()},
                      {"roformerModelPath", vocalPath.isNotEmpty() ? juce::var(vocalPath) : juce::var()},
                      {"rhythmModelPath", rhythmPath.isNotEmpty() ? juce::var(rhythmPath) : juce::var()},
                      {"sourceName", job.sourceName},
                      {"stems", names},
                      {"quality", quality},
                      {"useGpu", static_cast<bool>(prefs.get("stems", "useGpu", false))},
                      {"enhanceVocals", prefs.get("stems", "enhanceVocals", false)},
                      {"vocalEnhanceStrength", prefs.get("stems", "vocalEnhanceStrength", "medium")},
                      {"enhanceDrums", prefs.get("stems", "enhanceDrums", false)},
                      {"drumEnhanceStrength", prefs.get("stems", "drumEnhanceStrength", "medium")},
                      {"enhanceBass", prefs.get("stems", "enhanceBass", false)},
                      {"bassEnhanceStrength", prefs.get("stems", "bassEnhanceStrength", "medium")},
                      {"enhanceOther", prefs.get("stems", "enhanceOther", false)},
                      {"otherEnhanceStrength", prefs.get("stems", "otherEnhanceStrength", "medium")}});
    if (dereverbStrength.isNotEmpty() && stemNames.contains("vocals"))
    {
        p.getDynamicObject()->setProperty("dereverb", true);
        p.getDynamicObject()->setProperty("dereverbStrength", dereverbStrength);
    }
    log::info("stems", "dispatch STEM_SEPARATE jobId=" + jobId + " source=" + sourceItemId + " stems=" + stemNames.joinIntoString(","));
    if (onProgress) onProgress(0.0, "prepare");
    engine.send("STEM_SEPARATE", p);
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
    const auto exe = uvr::findExecutable(prefs);
    if (!exe.existsAsFile())
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
        // just that window (the built-in engine separates the same span).
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
    // Beside the built-in engine's stems, so the library's file clean-up covers these too.
    auto folder = juce::File::createLegalFileName(juce::File(item->filePath).getFileNameWithoutExtension()).trim();
    if (folder.isEmpty()) folder = "stems";
    const auto base = projectArtifactsBaseDir(mirror.model().filePath, "stems");
    auto outputDir = base.getChildFile(folder + "-stems");
    for (int n = 2; outputDir.exists(); ++n) outputDir = base.getChildFile(folder + "-stems-" + juce::String(n));

    const auto jobId = juce::Uuid().toDashedString();
    jobs[jobId] = job;
    activeJob = jobId;
    uvrJob = std::make_unique<uvr::Job>(exe, input, outputDir, run);
    uvrJob->onProgress = [this](double percent, const juce::String& stage)
    { if (onProgress) onProgress(percent, stage == "download" ? juce::String("download model") : juce::String("separate (UVR)")); };
    // Best of both: the built-in vocal model takes the vocals (it outscored every UVR model
    // measured), so SW's are dropped and the built-in engine runs on the same window after.
    const bool builtinVocals = run.builtinVocals && (keep.isEmpty() || keep.contains("vocals"))
                               && stems::isInstalled(stems::backupModel(), prefs) && stems::isInstalled(stems::vocalPack(), prefs);
    auto uvrKeep = keep;
    if (builtinVocals)
    {
        if (uvrKeep.isEmpty()) uvrKeep = run.stems;
        uvrKeep.removeString("vocals");
    }
    uvrJob->onDone = [this, jobId, uvrKeep, tempInput, builtinVocals](bool ok, const juce::String& error, std::map<juce::String, juce::File> stems)
    {
        const auto job = jobs.count(jobId) != 0 ? jobs[jobId] : Job{};
        finishUvr(jobId, ok, error, std::move(stems), uvrKeep, tempInput);
        if (ok && builtinVocals && mirror.model().findLibraryItem(job.sourceItemId) != nullptr
            && (job.clipId.isEmpty() || mirror.model().findClip(job.clipId) != nullptr))
            start(job.sourceItemId, job.clipId, {"vocals"}, "best", {});
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
    if (activeJob.isEmpty()) return;
    engine.send("STEM_SEPARATE_CANCEL", payload({{"jobId", activeJob}}));
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
