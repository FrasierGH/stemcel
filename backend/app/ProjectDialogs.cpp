#include "ProjectDialogs.h"

#include "Theme.h"

namespace stemcel::native
{
namespace
{
juce::var payload(std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();
    for (const auto& [k, v] : props) o->setProperty(k, v);
    return juce::var(o);
}

juce::String formatLength(double ms)
{
    if (ms <= 0.0) return {};
    const int total = juce::roundToInt(ms / 1000.0);
    return juce::String(total / 60) + ":" + juce::String(total % 60).paddedLeft('0', 2);
}

// "m:ss", "h:mm:ss" or plain seconds; < 0 when unparseable, 0 when empty.
double parseLength(const juce::String& text)
{
    const auto t = text.trim();
    if (t.isEmpty()) return 0.0;
    double seconds = 0.0;
    for (const auto& part : juce::StringArray::fromTokens(t, ":", ""))
    {
        if (!part.trim().containsOnly("0123456789.")) return -1.0;
        seconds = seconds * 60.0 + part.getDoubleValue();
    }
    return seconds * 1000.0;
}

void launch(juce::Component* content, const juce::String& title)
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(content);
    options.dialogTitle = title;
    options.dialogBackgroundColour = colours::background;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}
} // namespace

// ── Project properties ─────────────────────────────────────────────────────
ProjectPropertiesDialog::ProjectPropertiesDialog(EngineClient& client, AppPreferences& preferences, const ProjectModel& project)
    : engine(client), prefs(preferences), original(project.raw)
{
    name.setText(project.name, false);
    bpm.setText(juce::String(project.bpm, 2), false);
    bpm.setInputRestrictions(7, "0123456789.");
    length.setText(formatLength(static_cast<double>(project.raw.getProperty("projectLengthMs", 0.0))), false);
    barStart.setText(juce::String(static_cast<int>(project.raw.getProperty("barCounterStart", 1))), false);
    barStart.setInputRestrictions(4, "-0123456789");
    rate.addItem("44 100 Hz", 44100);
    rate.addItem("48 000 Hz", 48000);
    const int defaultRate = static_cast<int>(preferences.get("ui", "defaultProjectSampleRate", 44100)) == 48000 ? 48000 : 44100;
    rate.setSelectedId(static_cast<int>(project.raw.getProperty("targetSampleRate", defaultRate)) == 48000 ? 48000 : 44100,
                       juce::dontSendNotification);

    // Devices from the engine's last list; "App default" clears the project's choice.
    output.addItem("App default", 1);
    outputTypes.add({});
    outputDevices.add({});
    const auto currentType = project.raw.getProperty("audioOutputTypeName", {}).toString();
    const auto currentDevice = project.raw.getProperty("audioOutputDeviceName", {}).toString();
    int selected = 1;
    if (const auto* types = engine.latest("AUDIO_DEVICES_LIST").getProperty("types", {}).getArray())
        for (const auto& t : *types)
            if (const auto* devices = t.getProperty("outputDevices", t.getProperty("devices", {})).getArray())
                for (const auto& d : *devices)
                {
                    const auto typeName = t.getProperty("name", t.getProperty("typeName", {})).toString();
                    const auto deviceName = d.isString() ? d.toString() : d.getProperty("name", {}).toString();
                    outputTypes.add(typeName);
                    outputDevices.add(deviceName);
                    output.addItem(typeName + ": " + deviceName, outputTypes.size());
                    if (typeName == currentType && deviceName == currentDevice) selected = outputTypes.size();
                }
    output.setSelectedId(selected, juce::dontSendNotification);

    for (auto* c : std::initializer_list<juce::Component*>{&name, &bpm, &length, &barStart, &rate, &output, &applyButton, &cancelButton})
        addAndMakeVisible(c);
    for (auto* l : {&nameLabel, &bpmLabel, &lengthLabel, &rateLabel, &barLabel, &outputLabel})
    {
        l->setColour(juce::Label::textColourId, colours::textDim);
        l->setFont(uiFont(13.0f));
        addAndMakeVisible(l);
    }
    applyButton.setColour(juce::TextButton::buttonColourId, colours::sky600);
    applyButton.onClick = [this] { apply(); };
    cancelButton.onClick = [this] { close(); };
    setSize(520, 330);
}

void ProjectPropertiesDialog::show(EngineClient& engine, AppPreferences& prefs, const ProjectModel& project)
{
    launch(new ProjectPropertiesDialog(engine, prefs, project), "Project Properties");
}

void ProjectPropertiesDialog::apply()
{
    const double nextBpm = bpm.getText().getDoubleValue();
    const double lengthMs = parseLength(length.getText());
    const int barCounter = barStart.getText().getIntValue();
    if (nextBpm < 20.0 || nextBpm > 300.0 || lengthMs < 0.0 || barCounter < -64 || barCounter > 1)
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Project Properties",
                                               "Tempo must be 20-300 BPM, the length m:ss, and the bar counter between -64 and 1.");
        return;
    }
    engine.inUndoGroup("Project properties",
                       [&]
                       {
                           const auto nextName = name.getText().trim();
                           if (nextName.isNotEmpty() && nextName != original.getProperty("name", {}).toString())
                               engine.send("PROJECT_RENAME", payload({{"name", nextName}}));
                           if (std::abs(nextBpm - static_cast<double>(original.getProperty("bpm", 120.0))) > 1e-6)
                               engine.send("PROJECT_SET_BPM", payload({{"bpm", nextBpm},
                                                                       {"autoWarp", prefs.get("ui", "matchProjectTempoOnDrop", true)}}));
                           if (std::abs(lengthMs - static_cast<double>(original.getProperty("projectLengthMs", 0.0))) > 0.5)
                               engine.send("PROJECT_SET_LENGTH", payload({{"lengthMs", lengthMs}}));
                           if (rate.getSelectedId() != static_cast<int>(original.getProperty(
                                   "targetSampleRate", static_cast<int>(prefs.get("ui", "defaultProjectSampleRate", 44100)) == 48000 ? 48000 : 44100)))
                               engine.send("PROJECT_SET_TARGET_SAMPLE_RATE", payload({{"sampleRate", rate.getSelectedId()}}));
                           if (barCounter != static_cast<int>(original.getProperty("barCounterStart", 1)))
                               engine.send("PROJECT_SET_BAR_COUNTER_START", payload({{"barCounterStart", barCounter}}));
                           const int index = output.getSelectedId() - 1;
                           const auto typeName = outputTypes[index];
                           const auto deviceName = outputDevices[index];
                           if (typeName != original.getProperty("audioOutputTypeName", {}).toString()
                               || deviceName != original.getProperty("audioOutputDeviceName", {}).toString())
                               engine.send("PROJECT_SET_AUDIO_OUTPUT",
                                           payload({{"typeName", typeName.isNotEmpty() ? juce::var(typeName) : juce::var()},
                                                    {"deviceName", deviceName.isNotEmpty() ? juce::var(deviceName) : juce::var()}}));
                       });
    close();
}

void ProjectPropertiesDialog::close()
{
    if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState(0);
}

void ProjectPropertiesDialog::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
}

void ProjectPropertiesDialog::resized()
{
    auto area = getLocalBounds().reduced(16);
    auto row = [&area](juce::Label& l, juce::Component& c)
    {
        auto r = area.removeFromTop(28);
        l.setBounds(r.removeFromLeft(190));
        c.setBounds(r.reduced(0, 2));
        area.removeFromTop(6);
    };
    row(nameLabel, name);
    row(bpmLabel, bpm);
    row(lengthLabel, length);
    row(rateLabel, rate);
    row(barLabel, barStart);
    row(outputLabel, output);
    auto buttons = area.removeFromBottom(30);
    applyButton.setBounds(buttons.removeFromRight(100));
    buttons.removeFromRight(6);
    cancelButton.setBounds(buttons.removeFromRight(90));
}

// ── Relink ─────────────────────────────────────────────────────────────────
RelinkDialog::RelinkDialog(EngineClient& client, ProjectMirror& projectMirror) : engine(client), mirror(projectMirror)
{
    heading.setFont(uiFont(14.0f, true));
    heading.setColour(juce::Label::textColourId, colours::text);
    addAndMakeVisible(heading);
    list.setModel(this);
    list.setRowHeight(40);
    list.setColour(juce::ListBox::backgroundColourId, colours::zinc950);
    addAndMakeVisible(list);
    locateButton.onClick = [this] { locate(); };
    searchButton.onClick = [this] { searchFolder(); };
    closeButton.onClick = [this] { close(); };
    for (auto* b : {&locateButton, &searchButton, &closeButton}) addAndMakeVisible(b);
    mirror.addChangeListener(this);
    refresh();
    list.selectRow(0);
    setSize(620, 380);
}

RelinkDialog::~RelinkDialog()
{
    mirror.removeChangeListener(this);
}

void RelinkDialog::showIfNeeded(EngineClient& engine, ProjectMirror& mirror)
{
    for (const auto& item : mirror.model().library)
        if (item.unresolved && item.kind != "clip")
        {
            launch(new RelinkDialog(engine, mirror), "Missing Audio");
            return;
        }
}

void RelinkDialog::refresh()
{
    missing.clear();
    for (const auto& item : mirror.model().library)
        if (item.unresolved && item.kind != "clip") missing.emplace_back(item.id, item.filePath);
    heading.setText(missing.empty() ? juce::String("All audio found.")
                                    : juce::String(static_cast<int>(missing.size())) + " audio file(s) can't be found. Locate them to play this project.",
                    juce::dontSendNotification);
    list.updateContent();
    list.repaint();
}

void RelinkDialog::changeListenerCallback(juce::ChangeBroadcaster*)
{
    refresh();
    if (missing.empty()) close();
}

int RelinkDialog::getNumRows()
{
    return static_cast<int>(missing.size());
}

void RelinkDialog::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= static_cast<int>(missing.size())) return;
    if (selected) g.fillAll(colours::sky600.withAlpha(0.35f));
    const juce::File file(missing[static_cast<size_t>(row)].second);
    g.setColour(colours::text);
    g.setFont(uiFont(13.0f, true));
    g.drawText(file.getFileName(), 8, 2, width - 16, 18, juce::Justification::centredLeft, true);
    g.setColour(colours::textDim);
    g.setFont(uiFont(11.0f));
    g.drawText(file.getParentDirectory().getFullPathName(), 8, 20, width - 16, 16, juce::Justification::centredLeft, true);
    juce::ignoreUnused(height);
}

void RelinkDialog::locate()
{
    const int row = list.getSelectedRow();
    if (row < 0 || row >= static_cast<int>(missing.size())) return;
    const auto itemId = missing[static_cast<size_t>(row)].first;
    const auto path = missing[static_cast<size_t>(row)].second;
    const juce::File old(path);
    chooser = std::make_unique<juce::FileChooser>("Locate " + old.getFileName(), old.getParentDirectory(), "*" + old.getFileExtension());
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                         [this, itemId](const juce::FileChooser& fc)
                         {
                             if (fc.getResult().existsAsFile())
                                 engine.send("LIBRARY_ITEM_RELINK", payload({{"itemId", itemId}, {"filePath", fc.getResult().getFullPathName()}}));
                         });
}

void RelinkDialog::searchFolder()
{
    chooser = std::make_unique<juce::FileChooser>("Search a folder for the missing audio");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                         [this](const juce::FileChooser& fc)
                         {
                             const auto dir = fc.getResult();
                             if (!dir.isDirectory()) return;
                             // Match by file name, first hit wins.
                             std::map<juce::String, juce::String> wanted;
                             for (const auto& [id, path] : missing) wanted[juce::File(path).getFileName().toLowerCase()] = id;
                             int found = 0;
                             engine.inUndoGroup("Relink audio",
                                                [&]
                                                {
                                                    for (const auto& entry : juce::RangedDirectoryIterator(dir, true, "*", juce::File::findFiles))
                                                    {
                                                        const auto it = wanted.find(entry.getFile().getFileName().toLowerCase());
                                                        if (it == wanted.end()) continue;
                                                        engine.send("LIBRARY_ITEM_RELINK",
                                                                    payload({{"itemId", it->second}, {"filePath", entry.getFile().getFullPathName()}}));
                                                        wanted.erase(it);
                                                        ++found;
                                                        if (wanted.empty()) break;
                                                    }
                                                });
                             heading.setText("Found " + juce::String(found) + " file(s).", juce::dontSendNotification);
                         });
}

void RelinkDialog::close()
{
    if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState(0);
}

void RelinkDialog::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
}

void RelinkDialog::resized()
{
    auto area = getLocalBounds().reduced(14);
    heading.setBounds(area.removeFromTop(28));
    area.removeFromTop(6);
    auto buttons = area.removeFromBottom(30);
    closeButton.setBounds(buttons.removeFromRight(90));
    buttons.removeFromRight(6);
    searchButton.setBounds(buttons.removeFromRight(130));
    buttons.removeFromRight(6);
    locateButton.setBounds(buttons.removeFromRight(100));
    area.removeFromBottom(8);
    list.setBounds(area);
}

// ── Import from project ────────────────────────────────────────────────────
ProjectImportDialog::ProjectImportDialog(EngineClient& client, const juce::File& sourceFile,
                                         std::function<void(const juce::String&, bool)> notifyFn)
    : engine(client), source(sourceFile), notify(std::move(notifyFn)), subscriptions(client)
{
    heading.setText("Reading " + source.getFileName() + "...", juce::dontSendNotification);
    heading.setFont(uiFont(14.0f, true));
    addAndMakeVisible(heading);
    list.setModel(this);
    list.setRowHeight(26);
    list.setColour(juce::ListBox::backgroundColourId, colours::zinc950);
    addAndMakeVisible(list);
    importButton.setColour(juce::TextButton::buttonColourId, colours::sky600);
    importButton.setEnabled(false);
    importButton.onClick = [this] { import(); };
    cancelButton.onClick = [this] { close(); };
    addAndMakeVisible(importButton);
    addAndMakeVisible(cancelButton);

    subscriptions.on("PROJECT_IMPORT_SOURCE_MANIFEST",
                     [this](const juce::var& p)
                     {
                         if (p.getProperty("sourceProjectPath", {}).toString() != source.getFullPathName()) return;
                         entries.clear();
                         for (const auto& [key, label] : std::vector<std::pair<const char*, const char*>>{{"stems", "Stem"}, {"samples", "Sample"}, {"recordings", "Recording"}})
                             if (const auto* items = p.getProperty(key, {}).getArray())
                                 for (const auto& e : *items)
                                     entries.push_back({e.getProperty("id", {}).toString(), e.getProperty("name", {}).toString(), label, true});
                         const auto name = p.getProperty("name", {}).toString();
                         heading.setText(entries.empty() ? "\"" + name + "\" has no stems, samples or recordings to import."
                                                         : "Choose what to import from \"" + name + "\"",
                                         juce::dontSendNotification);
                         importButton.setEnabled(!entries.empty());
                         list.updateContent();
                     });
    subscriptions.on("PROJECT_IMPORT_SOURCE_FAILED",
                     [this](const juce::var& p) { heading.setText("Could not read that project: " + p.getProperty("error", {}).toString(), juce::dontSendNotification); });
    subscriptions.on("PROJECT_IMPORT_COMPLETED",
                     [this](const juce::var& p)
                     {
                         const bool ok = static_cast<bool>(p.getProperty("ok", false));
                         if (notify) notify(ok ? "Imported from " + source.getFileNameWithoutExtension()
                                               : "Import failed: " + p.getProperty("error", {}).toString(),
                                            !ok);
                         close();
                     });
    engine.send("PROJECT_IMPORT_SOURCE_INSPECT", payload({{"sourceProjectPath", source.getFullPathName()}}));
    setSize(520, 420);
}

void ProjectImportDialog::chooseAndShow(EngineClient& engine, std::function<void(const juce::String&, bool)> notify)
{
    auto chooser = std::make_shared<juce::FileChooser>("Import from Project", juce::File(), "*.stemcel;*.legacydaw");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                         [&engine, notify, chooser](const juce::FileChooser& fc)
                         {
                             if (fc.getResult().existsAsFile())
                                 launch(new ProjectImportDialog(engine, fc.getResult(), notify), "Import from Project");
                         });
}

int ProjectImportDialog::getNumRows()
{
    return static_cast<int>(entries.size());
}

void ProjectImportDialog::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool)
{
    if (row < 0 || row >= static_cast<int>(entries.size())) return;
    const auto& e = entries[static_cast<size_t>(row)];
    const auto box = juce::Rectangle<float>(8.0f, height / 2.0f - 7.0f, 14.0f, 14.0f);
    g.setColour(colours::zinc500);
    g.drawRoundedRectangle(box, 2.0f, 1.2f);
    if (e.chosen)
    {
        g.setColour(colours::sky400);
        g.fillRoundedRectangle(box.reduced(3.0f), 1.5f);
    }
    g.setColour(colours::text);
    g.setFont(uiFont(13.0f));
    g.drawText(e.name, 30, 0, width - 140, height, juce::Justification::centredLeft, true);
    g.setColour(colours::textDim);
    g.drawText(e.group, width - 110, 0, 100, height, juce::Justification::centredRight);
}

void ProjectImportDialog::listBoxItemClicked(int row, const juce::MouseEvent&)
{
    if (row < 0 || row >= static_cast<int>(entries.size())) return;
    entries[static_cast<size_t>(row)].chosen = !entries[static_cast<size_t>(row)].chosen;
    list.repaintRow(row);
}

void ProjectImportDialog::import()
{
    juce::Array<juce::var> ids;
    for (const auto& e : entries)
        if (e.chosen) ids.add(e.id);
    if (ids.isEmpty()) return;
    importButton.setEnabled(false);
    heading.setText("Importing...", juce::dontSendNotification);
    engine.send("PROJECT_IMPORT_ASSETS", payload({{"sourceProjectPath", source.getFullPathName()}, {"libraryItemIds", ids}}));
}

void ProjectImportDialog::close()
{
    if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState(0);
}

void ProjectImportDialog::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
}

void ProjectImportDialog::resized()
{
    auto area = getLocalBounds().reduced(14);
    heading.setBounds(area.removeFromTop(28));
    area.removeFromTop(6);
    auto buttons = area.removeFromBottom(30);
    importButton.setBounds(buttons.removeFromRight(100));
    buttons.removeFromRight(6);
    cancelButton.setBounds(buttons.removeFromRight(90));
    area.removeFromBottom(8);
    list.setBounds(area);
}

} // namespace stemcel::native
