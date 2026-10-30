#include "ExportDialog.h"

#include "Theme.h"

namespace stemcel::native
{
namespace
{
// useExportMixdownForm.ts: the persisted blob's version, choices and loudness presets.
constexpr int kExportSettingsVersion = 1;
constexpr int kLabelW = 110; // the label column
const juce::StringArray kFormats{"wav", "mp3", "flac", "aiff"};
const juce::StringArray kFormatLabels{"WAV", "MP3", "FLAC", "AIFF"};
const juce::Array<int> kRates{44100, 48000};
const juce::Array<int> kBitrates{128, 192, 320};
const juce::StringArray kLengthModes{"fixed-duration", "trim-to-last-clip", "selection"};
const juce::StringArray kLoudnessPresets{"off", "streaming-14", "apple-16", "broadcast-23", "analyze", "custom"};
const juce::StringArray kLoudnessLabels{"None - export as-is",
                                        "Streaming -14 LUFS (Spotify, YouTube, SoundCloud, Tidal, Amazon)",
                                        "Apple Music -16 LUFS",
                                        "EBU R128 / AES Broadcast -23 LUFS",
                                        "Analyse only (measure, no gain change)",
                                        "Custom target..."};

juce::Array<int> depthsFor(const juce::String& format)
{
    if (format == "wav") return {16, 24, 32};
    if (format == "flac" || format == "aiff") return {16, 24};
    return {};
}

// musicTime.ts formatTime / parseTime: `mm:ss` or `h:mm:ss`, and `ss`, `mm:ss`, `h:mm:ss` back.
juce::String formatTime(double ms)
{
    const auto total = static_cast<juce::int64>(juce::jmax(0.0, std::floor(ms / 1000.0)));
    const auto h = total / 3600, m = (total % 3600) / 60, s = total % 60;
    const auto two = [](juce::int64 v) { return juce::String(v).paddedLeft('0', 2); };
    return h > 0 ? juce::String(h) + ":" + two(m) + ":" + two(s) : two(m) + ":" + two(s);
}

std::optional<double> parseTime(const juce::String& text)
{
    const auto trimmed = text.trim();
    if (trimmed.isEmpty()) return std::nullopt;
    const auto parts = juce::StringArray::fromTokens(trimmed, ":", "");
    if (parts.size() > 3) return std::nullopt;
    std::vector<double> values;
    for (const auto& p : parts)
    {
        const auto t = p.trim();
        if (t.isEmpty() || !t.containsOnly("0123456789.")) return std::nullopt;
        values.push_back(t.getDoubleValue());
    }
    double h = 0.0, m = 0.0, s = 0.0;
    if (values.size() == 1) s = values[0];
    else if (values.size() == 2) m = values[0], s = values[1];
    else h = values[0], m = values[1], s = values[2];
    return std::round((h * 3600.0 + m * 60.0 + s) * 1000.0);
}

std::optional<double> number(const juce::String& text)
{
    const auto t = text.trim();
    if (t.isEmpty() || !t.containsOnly("-+0123456789.")) return std::nullopt;
    return t.getDoubleValue();
}

void styleLabel(juce::Label& l)
{
    l.setColour(juce::Label::textColourId, colours::textDim);
    l.setFont(uiFont(13.0f));
}
} // namespace

ExportDialog::ExportDialog(EngineClient& client, AppPreferences& preferences, const ProjectModel& project,
                           std::function<void(const juce::String&, bool)> notifyFn)
    : engine(client), prefs(preferences), projectName(project.name), projectFile(project.filePath), bpm(project.bpm),
      barCounterStart(static_cast<int>(project.raw.getProperty("barCounterStart", 1))), lastClipEndMs(project.contentEndMs()),
      selection(project.timelineSelection), notify(std::move(notifyFn)), subscriptions(client)
{
    for (int i = 0; i < kFormats.size(); ++i) formatBox.addItem(kFormatLabels[i], i + 1);
    for (int i = 0; i < kRates.size(); ++i) rate.addItem(juce::String(kRates[i]) + " Hz", i + 1);
    for (int i = 0; i < kBitrates.size(); ++i) bitrate.addItem(juce::String(kBitrates[i]) + " kbps", i + 1);
    length.addItem("Clip at duration", 1);
    length.addItem("Trim to end of last clip", 2);
    length.addItem("Timeline selection", 3);
    length.setItemEnabled(3, selection.has_value());
    for (int i = 0; i < kLoudnessLabels.size(); ++i) loudness.addItem(kLoudnessLabels[i], i + 1);

    // Base defaults (reseedOnOpen), then the project's persisted settings over them.
    const int projectRate = static_cast<int>(project.raw.getProperty("targetSampleRate", 0));
    const int fallbackRate = static_cast<int>(prefs.get("ui", "defaultProjectSampleRate", 44100)) == 48000 ? 48000 : 44100;
    formatBox.setSelectedItemIndex(0, juce::dontSendNotification);
    rate.setSelectedItemIndex(kRates.indexOf(projectRate == 48000 || projectRate == 44100 ? projectRate : fallbackRate),
                              juce::dontSendNotification);
    bitrate.setSelectedItemIndex(1, juce::dontSendNotification);
    dither.setToggleState(true, juce::dontSendNotification);
    tail.setText("0", false);
    loudness.setSelectedItemIndex(0, juce::dontSendNotification);
    target.setText("-14", false);
    ceiling.setText("-1", false);
    length.setSelectedItemIndex(0, juce::dontSendNotification);
    duration.setText(formatTime(project.projectLengthMs > 0.0 ? project.projectLengthMs : lastClipEndMs), false);
    startBar.setText(juce::String(static_cast<int>(project.raw.getProperty("mixdownStartBar", 1))), false);
    title.setText(projectName.trim(), false);
    applyPersisted(project.raw.getProperty("exportSettingsJson", {}).toString());
    if (output.getText().trim().isEmpty())
    {
        // mixdownHandlers.ts resolveDefaultPath: <project folder>/mixdown/<name>.<ext>.
        const auto base = projectFile.isNotEmpty() ? juce::File(projectFile).getParentDirectory() : prefs.defaultProjectDirectory();
        const auto safe = (projectName.trim().isNotEmpty() ? projectName.trim() : juce::String("Untitled"))
                              .replaceCharacters("\\/:*?\"<>|", "_________");
        output.setText(base.getChildFile("mixdown").getChildFile(safe + "." + format()).getFullPathName(), false);
    }
    // The saved depth, or the highest the format allows when it does not allow that one.
    const auto allowedDepths = depthsFor(format());
    for (const int d : allowedDepths) depth.addItem(juce::String(d) + "-bit" + (d == 32 ? " float" : ""), d);
    if (!allowedDepths.isEmpty())
        depth.setSelectedId(allowedDepths.contains(restoredDepth) ? restoredDepth : allowedDepths.getLast(), juce::dontSendNotification);

    formatBox.onChange = [this]
    {
        // Keep the path's extension and the depth choices in step with the format.
        const auto path = output.getText().trim();
        if (path.isNotEmpty()) output.setText(juce::File(path).withFileExtension(format()).getFullPathName(), false);
        const int previous = depth.getSelectedId();
        depth.clear(juce::dontSendNotification);
        const auto allowed = depthsFor(format());
        for (const int d : allowed) depth.addItem(juce::String(d) + "-bit" + (d == 32 ? " float" : ""), d);
        if (!allowed.isEmpty()) depth.setSelectedId(allowed.contains(previous) ? previous : allowed.getLast(), juce::dontSendNotification);
        updateEnablement();
    };
    for (auto* c : {&rate, &depth, &bitrate, &length, &loudness}) c->onChange = [this] { updateEnablement(); };
    for (auto* e : {&output, &startBar, &tail, &duration, &target, &ceiling}) e->onTextChange = [this] { updateEnablement(); };
    for (auto* c : std::initializer_list<juce::Component*>{&formatBox, &rate, &depth, &bitrate, &length, &loudness, &dither, &browseButton})
        addAndMakeVisible(c);
    for (auto* e : {&output, &startBar, &tail, &duration, &target, &ceiling, &title, &artist, &album, &year, &genre, &comment})
        addAndMakeVisible(e);
    for (auto* l : {&outputLabel, &formatLabel, &rateLabel, &depthLabel, &bitrateLabel, &startLabel, &startHint, &tailLabel,
                    &lengthLabel, &loudnessLabel, &targetLabel, &ceilingLabel, &titleLabel, &artistLabel, &albumLabel,
                    &yearLabel, &genreLabel, &commentLabel})
    {
        styleLabel(*l);
        addAndMakeVisible(l);
    }
    startHint.setFont(uiFont(11.0f));
    browseButton.onClick = [this] { browse(); };

    exportButton.setColour(juce::TextButton::buttonColourId, colours::sky600);
    exportButton.onClick = [this] { submit(); };
    cancelButton.onClick = [this]
    {
        if (rendering) engine.send("MIXDOWN_CANCEL");
        else close();
    };
    addAndMakeVisible(exportButton);
    addAndMakeVisible(cancelButton);
    progressBar.setColour(juce::ProgressBar::foregroundColourId, colours::sky500);
    addChildComponent(progressBar);

    subscriptions.on("MIXDOWN_PROGRESS",
                     [this](const juce::var& p)
                     {
                         progress = static_cast<double>(p.getProperty("percent", 0.0)) / 100.0;
                         stage = p.getProperty("stage", {}).toString();
                         progressBar.setTextToDisplay(stage.isNotEmpty() ? stage : juce::String());
                     });
    subscriptions.on("MIXDOWN_DONE",
                     [this](const juce::var& p)
                     {
                         juce::String message = "Exported " + juce::File(p.getProperty("filePath", {}).toString()).getFileName();
                         if (const auto l = p.getProperty("loudness", {}); l.isObject() && !l.getProperty("integratedLufs", {}).isVoid())
                             message << " (" << juce::String(static_cast<double>(l.getProperty("integratedLufs", 0.0)), 1) << " LUFS)";
                         if (const auto warning = p.getProperty("warning", {}).toString(); warning.isNotEmpty())
                             message << " - " << warning;
                         finished(true, message, juce::File(p.getProperty("filePath", {}).toString()));
                     });
    subscriptions.on("MIXDOWN_FAILED",
                     [this](const juce::var& p)
                     {
                         const auto code = p.getProperty("code", {}).toString();
                         finished(false, code == "cancelled" ? juce::String("Export cancelled")
                                                             : "Export failed: " + p.getProperty("error", {}).toString(),
                                  {});
                     });
    updateEnablement();
    setSize(620, 560);
}

ExportDialog::~ExportDialog()
{
    if (rendering) engine.send("MIXDOWN_CANCEL");
}

void ExportDialog::show(EngineClient& engine, AppPreferences& prefs, const ProjectModel& project,
                        std::function<void(const juce::String&, bool)> notify)
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(new ExportDialog(engine, prefs, project, std::move(notify)));
    options.dialogTitle = "Export Mixdown";
    options.dialogBackgroundColour = colours::background;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}

// The project's persisted blob, whitelisted field by field so a malformed or newer one
// cannot break the dialog (applyPersistedExportSettings).
void ExportDialog::applyPersisted(const juce::String& json)
{
    if (json.isEmpty()) return;
    const auto o = juce::JSON::parse(json);
    if (!o.isObject() || static_cast<int>(o.getProperty("version", 0)) != kExportSettingsVersion) return;
    const auto str = [&o](const char* key) -> std::optional<juce::String>
    {
        const auto v = o.getProperty(key, {});
        return v.isString() ? std::optional<juce::String>(v.toString()) : std::nullopt;
    };
    if (const auto f = str("format"); f && kFormats.contains(*f)) formatBox.setSelectedItemIndex(kFormats.indexOf(*f), juce::dontSendNotification);
    if (const int r = static_cast<int>(o.getProperty("sampleRate", 0)); kRates.contains(r))
        rate.setSelectedItemIndex(kRates.indexOf(r), juce::dontSendNotification);
    if (const int d = static_cast<int>(o.getProperty("bitDepth", 0)); d == 16 || d == 24 || d == 32) restoredDepth = d;
    if (o.getProperty("dither", {}).isBool()) dither.setToggleState(static_cast<bool>(o.getProperty("dither", true)), juce::dontSendNotification);
    if (const auto t = str("tailSecondsText")) tail.setText(*t, false);
    if (const int b = static_cast<int>(o.getProperty("bitrate", 0)); kBitrates.contains(b))
        bitrate.setSelectedItemIndex(kBitrates.indexOf(b), juce::dontSendNotification);
    if (const auto l = str("loudnessPreset"); l && kLoudnessPresets.contains(*l))
        loudness.setSelectedItemIndex(kLoudnessPresets.indexOf(*l), juce::dontSendNotification);
    if (const auto t = str("customTargetText")) target.setText(*t, false);
    if (const auto t = str("customCeilingText")) ceiling.setText(*t, false);
    if (const auto m = str("lengthMode"); m && (*m == "trim-to-last-clip" || *m == "fixed-duration"))
        length.setSelectedItemIndex(kLengthModes.indexOf(*m), juce::dontSendNotification);
    if (const auto t = str("durationText")) duration.setText(*t, false);
    if (const auto t = str("title")) title.setText(*t, false);
    if (const auto t = str("artist")) artist.setText(*t, false);
    if (const auto t = str("album")) album.setText(*t, false);
    if (const auto t = str("year")) year.setText(*t, false);
    if (const auto t = str("genre")) genre.setText(*t, false);
    if (const auto t = str("comment")) comment.setText(*t, false);
    if (const auto t = str("outputPath"); t && t->isNotEmpty()) output.setText(*t, false);
}

juce::var ExportDialog::snapshot() const
{
    auto* o = new juce::DynamicObject();
    const auto mode = kLengthModes[juce::jmax(0, length.getSelectedItemIndex())];
    o->setProperty("version", kExportSettingsVersion);
    o->setProperty("outputPath", output.getText());
    o->setProperty("format", format());
    o->setProperty("sampleRate", kRates[juce::jmax(0, rate.getSelectedItemIndex())]);
    o->setProperty("bitDepth", bitDepth() > 0 ? bitDepth() : 16);
    o->setProperty("dither", dither.getToggleState());
    o->setProperty("tailSecondsText", tail.getText());
    o->setProperty("bitrate", kBitrates[juce::jmax(0, bitrate.getSelectedItemIndex())]);
    o->setProperty("loudnessPreset", kLoudnessPresets[juce::jmax(0, loudness.getSelectedItemIndex())]);
    o->setProperty("customTargetText", target.getText());
    o->setProperty("customCeilingText", ceiling.getText());
    // The selection is per session; the project remembers the renderer's two modes.
    o->setProperty("lengthMode", mode == "selection" ? juce::String("fixed-duration") : mode);
    o->setProperty("durationText", duration.getText());
    o->setProperty("title", title.getText());
    o->setProperty("artist", artist.getText());
    o->setProperty("album", album.getText());
    o->setProperty("year", year.getText());
    o->setProperty("genre", genre.getText());
    o->setProperty("comment", comment.getText());
    return juce::var(o);
}

juce::String ExportDialog::format() const
{
    return kFormats[juce::jmax(0, formatBox.getSelectedItemIndex())];
}

int ExportDialog::bitDepth() const
{
    return depth.getNumItems() > 0 ? depth.getSelectedId() : 0;
}

double ExportDialog::startBarMs() const
{
    // The ruler labels internal bar i as `i + barCounterStart`; the inverse is the bar to skip to.
    const int bar = juce::jlimit(-64, 4096, juce::roundToInt(startBar.getText().getDoubleValue()));
    const double msPerBar = 60000.0 / juce::jmax(1.0, bpm) * 4.0;
    return juce::jmax(0, bar - barCounterStart) * msPerBar;
}

std::optional<double> ExportDialog::durationMs() const
{
    const auto mode = kLengthModes[juce::jmax(0, length.getSelectedItemIndex())];
    if (mode == "trim-to-last-clip") return lastClipEndMs > 0.0 ? std::optional<double>(lastClipEndMs) : std::nullopt;
    if (mode == "selection")
        return selection.has_value() ? std::optional<double>(selection->second) : std::nullopt;
    const auto parsed = parseTime(duration.getText());
    return parsed.has_value() && *parsed > 0.0 ? parsed : std::nullopt;
}

std::optional<double> ExportDialog::tailSeconds() const
{
    if (tail.getText().trim().isEmpty()) return 0.0;
    const auto n = number(tail.getText());
    return n.has_value() && *n >= 0.0 && *n <= 60.0 ? n : std::nullopt;
}

void ExportDialog::updateEnablement()
{
    const auto fmt = format();
    depth.setEnabled(fmt != "mp3");
    bitrate.setEnabled(fmt == "mp3");
    // TPDF dither only matters for 16-bit integer output.
    dither.setEnabled(fmt != "mp3" && bitDepth() == 16);
    const bool custom = kLoudnessPresets[juce::jmax(0, loudness.getSelectedItemIndex())] == "custom";
    for (juce::Component* c : {static_cast<juce::Component*>(&target), static_cast<juce::Component*>(&ceiling),
                               static_cast<juce::Component*>(&targetLabel), static_cast<juce::Component*>(&ceilingLabel)})
        c->setVisible(custom);
    duration.setEnabled(length.getSelectedItemIndex() == 0);
    const double skip = startBarMs();
    startHint.setText(skip > 0.0 ? "Skips the first " + formatTime(skip) + "." : juce::String("Renders from the project start."),
                      juce::dontSendNotification);

    const auto t = number(target.getText());
    const auto c = number(ceiling.getText());
    const bool loudnessOk = !custom || (t && *t >= -30.0 && *t <= -6.0 && c && *c >= -9.0 && *c <= 0.0);
    tail.setColour(juce::TextEditor::outlineColourId, tailSeconds().has_value() ? colours::border : colours::red500);
    exportButton.setEnabled(!rendering && output.getText().trim().isNotEmpty() && durationMs().has_value()
                            && tailSeconds().has_value() && loudnessOk);
}

void ExportDialog::browse()
{
    const auto current = output.getText().trim();
    const auto suggested = current.isNotEmpty() ? juce::File(current)
                                                : prefs.defaultProjectDirectory().getChildFile("mixdown").getChildFile("Mixdown." + format());
    chooser = std::make_unique<juce::FileChooser>("Export Mixdown", suggested, "*." + format());
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                         [this](const juce::FileChooser& fc)
                         {
                             const auto file = fc.getResult();
                             if (file != juce::File()) output.setText(file.withFileExtension(format()).getFullPathName(), true);
                         });
}

void ExportDialog::submit()
{
    if (!exportButton.isEnabled()) return;
    // Normalise a typed extension to the selected format, then confirm an overwrite.
    auto file = juce::File(output.getText().trim());
    if (!file.hasFileExtension(format())) file = file.withFileExtension(format());
    output.setText(file.getFullPathName(), false);
    if (!file.existsAsFile())
    {
        start(file);
        return;
    }
    juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                     .withIconType(juce::MessageBoxIconType::QuestionIcon)
                                     .withTitle("Export Mixdown")
                                     .withMessage(file.getFileName() + " already exists. Replace it?")
                                     .withButton("Replace")
                                     .withButton("Cancel"),
                                 [safe = juce::Component::SafePointer<ExportDialog>(this), file](int result)
                                 {
                                     if (safe != nullptr && result == 1) safe->start(file);
                                 });
}

void ExportDialog::start(const juce::File& file)
{
    const auto fmt = format();
    const auto mode = kLengthModes[juce::jmax(0, length.getSelectedItemIndex())];
    const auto lengthMs = durationMs();
    if (!lengthMs.has_value()) return;

    auto* p = new juce::DynamicObject();
    p->setProperty("outputPath", file.getFullPathName());
    p->setProperty("sampleRate", kRates[juce::jmax(0, rate.getSelectedItemIndex())]);
    p->setProperty("format", fmt);
    p->setProperty("tailSeconds", tailSeconds().value_or(0.0));
    // A selection renders as a fixed span: from its start to its end (lengthMs is project time).
    p->setProperty("lengthMode", mode == "selection" ? juce::String("fixed-duration") : mode);
    p->setProperty("lengthMs", *lengthMs);
    const double startMs = mode == "selection" && selection.has_value() ? selection->first : startBarMs();
    if (startMs > 0.0) p->setProperty("startMs", startMs);
    if (fmt == "mp3")
        p->setProperty("bitrateKbps", kBitrates[juce::jmax(0, bitrate.getSelectedItemIndex())]);
    else
    {
        p->setProperty("bitDepth", bitDepth());
        p->setProperty("dither", dither.getToggleState());
    }
    auto* md = new juce::DynamicObject();
    for (const auto& [key, editor] : std::initializer_list<std::pair<const char*, const juce::TextEditor*>>{
             {"title", &title}, {"artist", &artist}, {"album", &album}, {"year", &year}, {"genre", &genre}, {"comment", &comment}})
        if (editor->getText().trim().isNotEmpty()) md->setProperty(key, editor->getText().trim());
    if (!md->getProperties().isEmpty()) p->setProperty("metadata", juce::var(md));
    else delete md;
    const auto preset = kLoudnessPresets[juce::jmax(0, loudness.getSelectedItemIndex())];
    if (preset != "off")
    {
        auto* l = new juce::DynamicObject();
        if (preset == "analyze") l->setProperty("mode", "analyze");
        else
        {
            l->setProperty("mode", "normalize");
            const double t = preset == "streaming-14" ? -14.0 : preset == "apple-16" ? -16.0 : preset == "broadcast-23" ? -23.0
                                                                                                                       : target.getText().getDoubleValue();
            const double c = preset == "broadcast-23" ? -2.0 : preset == "custom" ? ceiling.getText().getDoubleValue() : -1.0;
            l->setProperty("targetLufs", t);
            l->setProperty("ceilingDbtp", c);
        }
        p->setProperty("loudness", juce::var(l));
    }

    // Persist with the project just before rendering, so a cancelled export keeps them.
    engine.send("PROJECT_SET_EXPORT_SETTINGS", [this] {
        auto* o = new juce::DynamicObject();
        o->setProperty("json", juce::JSON::toString(snapshot(), true));
        return juce::var(o);
    }());
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("mixdownStartBar", juce::jlimit(-64, 4096, juce::roundToInt(startBar.getText().getDoubleValue())));
        engine.send("PROJECT_SET_MIXDOWN_START_BAR", juce::var(o));
    }

    rendering = true;
    progress = 0.0;
    progressBar.setTextToDisplay("Preparing");
    progressBar.setVisible(true);
    exportButton.setEnabled(false);
    for (auto* c : getChildren())
        if (c != &progressBar && c != &cancelButton && c != &exportButton) c->setEnabled(false);
    resized();
    startTimerHz(10);
    engine.send("MIXDOWN_START", juce::var(p));
}

void ExportDialog::timerCallback()
{
    progressBar.repaint();
}

void ExportDialog::finished(bool ok, const juce::String& message, const juce::File& file)
{
    if (!rendering) return;
    rendering = false;
    stopTimer();
    if (notify) notify(message, !ok);
    if (ok && file.existsAsFile())
    {
        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                         .withIconType(juce::MessageBoxIconType::InfoIcon)
                                         .withTitle("Export Mixdown")
                                         .withMessage(message)
                                         .withButton("Show file")
                                         .withButton("OK"),
                                     [file](int result)
                                     {
                                         if (result == 1) file.revealToUser();
                                     });
    }
    close();
}

void ExportDialog::close()
{
    if (auto* window = findParentComponentOfClass<juce::DialogWindow>()) window->exitModalState(0);
}

void ExportDialog::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
}

void ExportDialog::resized()
{
    auto area = getLocalBounds().reduced(16);
    auto row = [&area](juce::Label& label) -> juce::Rectangle<int>
    {
        auto r = area.removeFromTop(28);
        label.setBounds(r.removeFromLeft(kLabelW));
        area.removeFromTop(4);
        return r.reduced(0, 2);
    };
    {
        auto r = row(outputLabel);
        browseButton.setBounds(r.removeFromRight(90));
        r.removeFromRight(6);
        output.setBounds(r);
    }
    {
        auto r = row(formatLabel);
        formatBox.setBounds(r.removeFromLeft(130));
        r.removeFromLeft(16);
        rateLabel.setBounds(r.removeFromLeft(90));
        rate.setBounds(r.removeFromLeft(130));
    }
    {
        auto r = row(depthLabel);
        depth.setBounds(r.removeFromLeft(130));
        r.removeFromLeft(16);
        bitrateLabel.setBounds(r.removeFromLeft(90));
        bitrate.setBounds(r.removeFromLeft(130));
        r.removeFromLeft(16);
        dither.setBounds(r);
    }
    {
        auto r = row(startLabel);
        startBar.setBounds(r.removeFromLeft(70));
        r.removeFromLeft(10);
        startHint.setBounds(r.removeFromLeft(220));
        r.removeFromLeft(10);
        tailLabel.setBounds(r.removeFromLeft(60));
        tail.setBounds(r.removeFromLeft(60));
    }
    {
        auto r = row(lengthLabel);
        length.setBounds(r.removeFromLeft(220));
        r.removeFromLeft(10);
        duration.setBounds(r.removeFromLeft(90));
    }
    loudness.setBounds(row(loudnessLabel));
    {
        auto r = area.removeFromTop(28).reduced(0, 2);
        r.removeFromLeft(kLabelW);
        targetLabel.setBounds(r.removeFromLeft(90));
        target.setBounds(r.removeFromLeft(70));
        r.removeFromLeft(16);
        ceilingLabel.setBounds(r.removeFromLeft(90));
        ceiling.setBounds(r.removeFromLeft(70));
        area.removeFromTop(8);
    }
    title.setBounds(row(titleLabel));
    artist.setBounds(row(artistLabel));
    album.setBounds(row(albumLabel));
    {
        auto r = row(yearLabel);
        year.setBounds(r.removeFromLeft(90));
        r.removeFromLeft(16);
        genreLabel.setBounds(r.removeFromLeft(60));
        genre.setBounds(r);
    }
    comment.setBounds(row(commentLabel));
    auto buttons = area.removeFromBottom(32);
    exportButton.setBounds(buttons.removeFromRight(110));
    buttons.removeFromRight(8);
    cancelButton.setBounds(buttons.removeFromRight(90));
    if (rendering) progressBar.setBounds(buttons.reduced(0, 4));
}

} // namespace stemcel::native
