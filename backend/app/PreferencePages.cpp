#include "PreferencePages.h"
#include "AppPaths.h"

#include <tuple>

#include "Stems.h"
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

// Preset tables shared with the renderer's brake/backspin stores.
double brakeSeconds(const juce::String& d) { return d == "short" ? 0.4 : d == "long" ? 0.9 : 0.6; }
double brakeCurve(const juce::String& c) { return c == "linear" ? 1.0 : c == "steep" ? 3.0 : 2.0; }
double spinSeconds(const juce::String& d) { return d == "short" ? 0.4 : d == "medium" ? 0.6 : 0.9; }
double spinSpeed(const juce::String& i) { return i == "gentle" ? 4.0 : i == "wild" ? 8.0 : 6.0; }

void styleLabel(juce::Label& l)
{
    l.setColour(juce::Label::textColourId, colours::textDim);
    l.setFont(uiFont(13.0f));
}

void fillCombo(juce::ComboBox& box, const juce::StringArray& values, const juce::StringArray& labels, const juce::String& current)
{
    for (int i = 0; i < values.size(); ++i) box.addItem(labels[i], i + 1);
    box.setSelectedItemIndex(juce::jmax(0, values.indexOf(current)), juce::dontSendNotification);
}
} // namespace

void pushEnginePreferences(EngineClient& engine, AppPreferences& prefs)
{
    engine.send("BRAKE_SETTINGS_SET", payload({{"seconds", brakeSeconds(prefs.get("brake", "duration", "medium").toString())},
                                               {"curve", brakeCurve(prefs.get("brake", "curve", "curved").toString())}}));
    engine.send("BACKSPIN_SETTINGS_SET", payload({{"seconds", spinSeconds(prefs.get("backspin", "duration", "long").toString())},
                                                  {"speed", spinSpeed(prefs.get("backspin", "intensity", "medium").toString())},
                                                  {"curve", 3.0}}));
    juce::Array<juce::var> enabled;
    if (const auto* o = prefs.getSection("enabledMidiInputs").getDynamicObject())
        for (const auto& p : o->getProperties())
            if (static_cast<bool>(p.value)) enabled.add(p.name.toString());
    engine.send("MIDI_INPUTS_SET", payload({{"identifiers", enabled}}));
    if (const auto* o = prefs.getSection("midiDevicePreferences").getDynamicObject())
        for (const auto& p : o->getProperties())
            engine.send("MIDI_SCRATCH_SETTINGS_SET",
                        payload({{"deviceIdentifier", p.name.toString()},
                                 {"crossfaderDirection", p.value.getProperty("crossfaderDirection", "leftToRight")}}));
    if (const auto* o = prefs.getSection("midiDeckSelections").getDynamicObject())
        for (const auto& p : o->getProperties())
            engine.send("MIDI_DECK_SELECTION_SET", payload({{"deviceIdentifier", p.name.toString()},
                                                            {"deck1Enabled", p.value.getProperty("deck1Enabled", true)},
                                                            {"deck2Enabled", p.value.getProperty("deck2Enabled", false)}}));
}

// ── General ────────────────────────────────────────────────────────────────
GeneralPreferencesPage::GeneralPreferencesPage(AppPreferences& preferences) : prefs(preferences)
{
    styleLabel(waveLabel);
    fillCombo(waveMode, {"summary", "stereo"},
              {"Single waveform - One combined waveform per clip", "Left and right - Separate L / R for stereo clips"},
              prefs.get("ui", "waveformDisplayMode", "stereo").toString());
    waveMode.onChange = [this]
    {
        prefs.set("ui", "waveformDisplayMode", juce::StringArray{"summary", "stereo"}[waveMode.getSelectedItemIndex()]);
        prefs.save();
    };
    toasts.setToggleState(static_cast<bool>(prefs.get("toasts", "enabled", true)), juce::dontSendNotification);
    toasts.onClick = [this]
    {
        prefs.set("toasts", "enabled", toasts.getToggleState());
        prefs.save();
    };
    for (auto* c : std::initializer_list<juce::Component*>{&waveLabel, &waveMode, &toasts}) addAndMakeVisible(c);
}

void GeneralPreferencesPage::resized()
{
    auto area = getLocalBounds().reduced(16);
    auto r = area.removeFromTop(28);
    waveLabel.setBounds(r.removeFromLeft(120));
    waveMode.setBounds(r.removeFromLeft(380).reduced(0, 2));
    area.removeFromTop(12);
    toasts.setBounds(area.removeFromTop(26));
}

// ── Timeline ───────────────────────────────────────────────────────────────
TimelinePreferencesPage::TimelinePreferencesPage(AppPreferences& preferences) : prefs(preferences)
{
    auto toggle = [this](juce::ToggleButton& b, const char* key, bool fallback)
    {
        b.setToggleState(static_cast<bool>(prefs.get("ui", key, fallback)), juce::dontSendNotification);
        b.onClick = [this, &b, key]
        {
            prefs.set("ui", key, b.getToggleState());
            prefs.save();
        };
        addAndMakeVisible(b);
    };
    toggle(follow, "followPlayback", true);
    toggle(seedTempo, "seedProjectTempoFromFirstClip", true);
    toggle(matchTempo, "matchProjectTempoOnDrop", true);
    toggle(align, "alignClipsToGridOnAnalysis", true);
    styleLabel(skipLabel);
    fillCombo(skipTarget, {"markers", "timelineEnds"},
              {"Markers - Step through timeline markers", "Timeline ends - Jump to project start / end"},
              prefs.get("ui", "skipButtonTarget", "markers").toString());
    skipTarget.onChange = [this]
    {
        prefs.set("ui", "skipButtonTarget", juce::StringArray{"markers", "timelineEnds"}[skipTarget.getSelectedItemIndex()]);
        prefs.save();
    };
    addAndMakeVisible(skipLabel);
    addAndMakeVisible(skipTarget);
}

void TimelinePreferencesPage::resized()
{
    auto area = getLocalBounds().reduced(16);
    for (auto* b : {&follow, &seedTempo, &matchTempo, &align})
    {
        b->setBounds(area.removeFromTop(26));
        area.removeFromTop(4);
    }
    area.removeFromTop(8);
    auto r = area.removeFromTop(28);
    skipLabel.setBounds(r.removeFromLeft(120));
    skipTarget.setBounds(r.removeFromLeft(380).reduced(0, 2));
}

// ── Project ────────────────────────────────────────────────────────────────
ProjectPreferencesPage::ProjectPreferencesPage(AppPreferences& preferences) : prefs(preferences)
{
    for (auto* l : {&pathsHeading, &autosaveHeading, &cleanupHeading})
    {
        l->setFont(uiFont(13.0f, true));
        l->setColour(juce::Label::textColourId, colours::text);
    }
    for (auto* l : {&projectLabel, &clipLabel, &intervalLabel}) styleLabel(*l);
    for (auto* l : {&projectFolder, &clipFolder}) l->setColour(juce::Label::textColourId, colours::text);
    const auto clipDir = prefs.get("paths", "defaultClipDir", "").toString();
    projectFolder.setText(prefs.defaultProjectDirectory().getFullPathName(), juce::dontSendNotification);
    clipFolder.setText(clipDir.isNotEmpty() ? clipDir : juce::String("(home)"), juce::dontSendNotification);
    projectChange.onClick = [this] { chooseFolder("defaultProjectDir", projectFolder, "Project folder"); };
    clipChange.onClick = [this] { chooseFolder("defaultClipDir", clipFolder, "Clip folder"); };

    autosave.setToggleState(static_cast<bool>(prefs.get("autosave", "enabled", true)), juce::dontSendNotification);
    autosave.onClick = [this]
    {
        prefs.set("autosave", "enabled", autosave.getToggleState());
        prefs.save();
        interval.setEnabled(autosave.getToggleState());
    };
    interval.setSliderStyle(juce::Slider::LinearHorizontal);
    interval.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
    interval.setRange(5.0, 600.0, 5.0);
    interval.setValue(static_cast<double>(prefs.get("autosave", "intervalSeconds", 30)), juce::dontSendNotification);
    interval.setEnabled(autosave.getToggleState());
    interval.onDragEnd = [this]
    {
        prefs.set("autosave", "intervalSeconds", static_cast<int>(interval.getValue()));
        prefs.save();
    };

    saveProjectFile.setToggleState(static_cast<bool>(prefs.get("autosave", "saveProjectFile", true)), juce::dontSendNotification);
    saveProjectFile.onClick = [this]
    {
        prefs.set("autosave", "saveProjectFile", saveProjectFile.getToggleState());
        prefs.save();
    };
    addAndMakeVisible(saveProjectFile);

    cleanup.setToggleState(static_cast<bool>(prefs.get("ui", "cleanupProjectFiles", false)), juce::dontSendNotification);
    cleanup.onClick = [this]
    {
        prefs.set("ui", "cleanupProjectFiles", cleanup.getToggleState());
        prefs.save();
    };
    cleanupHint.setText("When you remove a stem or sample from the library, also delete its generated file from the "
                        "project's stems / samples folder. Cover art and tag data go once nothing else uses them. "
                        "Your original imported audio is never deleted. This cannot be undone.",
                        juce::dontSendNotification);
    cleanupHint.setFont(uiFont(11.0f));
    cleanupHint.setColour(juce::Label::textColourId, colours::textFaint);
    cleanupHint.setJustificationType(juce::Justification::topLeft);
    for (auto* c : std::initializer_list<juce::Component*>{&pathsHeading, &projectLabel, &projectFolder, &projectChange, &clipLabel,
                                                           &clipFolder, &clipChange, &autosaveHeading, &autosave, &intervalLabel,
                                                           &interval, &cleanupHeading, &cleanup, &cleanupHint})
        addAndMakeVisible(c);
}

void ProjectPreferencesPage::chooseFolder(const char* key, juce::Label& label, const juce::String& title)
{
    chooser = std::make_unique<juce::FileChooser>(title, juce::File(label.getText()).isDirectory() ? juce::File(label.getText())
                                                                                                    : prefs.defaultProjectDirectory());
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                         [this, key, &label](const juce::FileChooser& fc)
                         {
                             if (!fc.getResult().isDirectory()) return;
                             prefs.set("paths", key, fc.getResult().getFullPathName());
                             prefs.save();
                             label.setText(fc.getResult().getFullPathName(), juce::dontSendNotification);
                         });
}

void ProjectPreferencesPage::resized()
{
    auto area = getLocalBounds().reduced(16);
    pathsHeading.setBounds(area.removeFromTop(22));
    for (auto [label, value, button] : {std::tuple{&projectLabel, &projectFolder, &projectChange},
                                        std::tuple{&clipLabel, &clipFolder, &clipChange}})
    {
        auto r = area.removeFromTop(28);
        label->setBounds(r.removeFromLeft(120));
        button->setBounds(r.removeFromRight(90).reduced(0, 2));
        value->setBounds(r);
        area.removeFromTop(4);
    }
    area.removeFromTop(10);
    autosaveHeading.setBounds(area.removeFromTop(22));
    autosave.setBounds(area.removeFromTop(26));
    auto r = area.removeFromTop(28);
    intervalLabel.setBounds(r.removeFromLeft(120));
    interval.setBounds(r.removeFromLeft(300).reduced(0, 2));
    saveProjectFile.setBounds(area.removeFromTop(26));
    area.removeFromTop(14);
    cleanupHeading.setBounds(area.removeFromTop(22));
    cleanup.setBounds(area.removeFromTop(26));
    cleanupHint.setBounds(area.removeFromTop(48));
}

// ── Effects ────────────────────────────────────────────────────────────────
EffectsPreferencesPage::EffectsPreferencesPage(EngineClient& client, AppPreferences& preferences) : engine(client), prefs(preferences)
{
    styleLabel(brakeLabel);
    styleLabel(backspinLabel);
    fillCombo(brakeDuration, {"short", "medium", "long"}, {"Short stop", "Medium stop", "Long stop"},
              prefs.get("brake", "duration", "medium").toString());
    fillCombo(brakeCurve, {"linear", "curved", "steep"}, {"Linear", "Curved", "Steep"}, prefs.get("brake", "curve", "curved").toString());
    fillCombo(spinDuration, {"short", "medium", "long"}, {"Short spin", "Medium spin", "Long spin"},
              prefs.get("backspin", "duration", "long").toString());
    fillCombo(spinIntensity, {"gentle", "medium", "wild"}, {"Gentle", "Medium", "Wild"}, prefs.get("backspin", "intensity", "medium").toString());
    for (auto* c : {&brakeDuration, &brakeCurve, &spinDuration, &spinIntensity})
    {
        c->onChange = [this] { apply(); };
        addAndMakeVisible(c);
    }
    addAndMakeVisible(brakeLabel);
    addAndMakeVisible(backspinLabel);
}

void EffectsPreferencesPage::apply()
{
    const juce::StringArray durations{"short", "medium", "long"};
    prefs.set("brake", "duration", durations[brakeDuration.getSelectedItemIndex()]);
    prefs.set("brake", "curve", juce::StringArray{"linear", "curved", "steep"}[brakeCurve.getSelectedItemIndex()]);
    prefs.set("backspin", "duration", durations[spinDuration.getSelectedItemIndex()]);
    prefs.set("backspin", "intensity", juce::StringArray{"gentle", "medium", "wild"}[spinIntensity.getSelectedItemIndex()]);
    prefs.save();
    pushEnginePreferences(engine, prefs);
}

void EffectsPreferencesPage::resized()
{
    auto area = getLocalBounds().reduced(16);
    auto r = area.removeFromTop(28);
    brakeLabel.setBounds(r.removeFromLeft(100));
    brakeDuration.setBounds(r.removeFromLeft(160).reduced(0, 2));
    r.removeFromLeft(8);
    brakeCurve.setBounds(r.removeFromLeft(160).reduced(0, 2));
    area.removeFromTop(8);
    r = area.removeFromTop(28);
    backspinLabel.setBounds(r.removeFromLeft(100));
    spinDuration.setBounds(r.removeFromLeft(160).reduced(0, 2));
    r.removeFromLeft(8);
    spinIntensity.setBounds(r.removeFromLeft(160).reduced(0, 2));
}

// ── Stems ──────────────────────────────────────────────────────────────────
StemsPreferencesPage::StemsPreferencesPage(AppPreferences& preferences) : prefs(preferences)
{
    gpu.setToggleState(static_cast<bool>(prefs.get("stems", "useGpu", false)), juce::dontSendNotification);
    backup.setToggleState(static_cast<bool>(prefs.get("stems", "useBackupModel", false)), juce::dontSendNotification);
    gpu.onClick = [this] { prefs.set("stems", "useGpu", gpu.getToggleState()); prefs.save(); };
    backup.onClick = [this] { prefs.set("stems", "useBackupModel", backup.getToggleState()); prefs.save(); };
    addAndMakeVisible(gpu);
    addAndMakeVisible(backup);
    const juce::StringArray names{"Vocals", "Drums", "Bass", "Other"};
    const juce::StringArray strengthKeys{"vocalEnhanceStrength", "drumEnhanceStrength", "bassEnhanceStrength", "otherEnhanceStrength"};
    for (int i = 0; i < names.size(); ++i)
    {
        const auto& n = names[i];
        const auto enableKey = "enhance" + n;
        const auto strengthKey = strengthKeys[i];
        auto* t = enhance.add(new juce::ToggleButton("Enhance " + n.toLowerCase()));
        t->setToggleState(static_cast<bool>(prefs.get("stems", enableKey, false)), juce::dontSendNotification);
        t->onClick = [this, t, enableKey] { prefs.set("stems", enableKey, t->getToggleState()); prefs.save(); };
        addAndMakeVisible(t);
        auto* s = strength.add(new juce::ComboBox());
        fillCombo(*s, {"light", "medium", "strong"}, {"Light", "Medium", "Strong"}, prefs.get("stems", strengthKey, "medium").toString());
        s->onChange = [this, s, strengthKey]
        {
            prefs.set("stems", strengthKey, juce::StringArray{"light", "medium", "strong"}[s->getSelectedItemIndex()]);
            prefs.save();
        };
        addAndMakeVisible(s);
    }
    for (const auto* model : {&stems::vocalPack(), &stems::rhythmPack(), &stems::backupModel()})
        addAndMakeVisible(models.add(new stems::ModelRow(*model, prefs)));
}

void StemsPreferencesPage::resized()
{
    auto area = getLocalBounds().reduced(16);
    gpu.setBounds(area.removeFromTop(24));
    backup.setBounds(area.removeFromTop(24));
    area.removeFromTop(6);
    for (int i = 0; i < enhance.size(); ++i)
    {
        auto r = area.removeFromTop(26);
        enhance[i]->setBounds(r.removeFromLeft(180));
        strength[i]->setBounds(r.removeFromLeft(120).reduced(0, 2));
    }
    area.removeFromTop(8);
    for (auto* m : models)
    {
        m->setBounds(area.removeFromTop(40));
        area.removeFromTop(2);
    }
}

// ── MIDI ───────────────────────────────────────────────────────────────────
MidiPreferencesPage::MidiPreferencesPage(EngineClient& client, AppPreferences& preferences)
    : engine(client), prefs(preferences), subscriptions(client)
{
    deviceList.setModel(this);
    deviceList.setRowHeight(30);
    deviceList.setColour(juce::ListBox::backgroundColourId, colours::zinc950);
    addAndMakeVisible(deviceList);
    rescan.onClick = [this] { engine.send("MIDI_DEVICES_REQUEST"); };
    addAndMakeVisible(rescan);
    styleLabel(monitorLabel);
    addAndMakeVisible(monitorLabel);
    monitor.setMultiLine(true);
    monitor.setReadOnly(true);
    monitor.setFont(juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain)));
    addAndMakeVisible(monitor);
    subscriptions.on("MIDI_DEVICES_LIST", [this](const juce::var& p) { devices(p); });
    subscriptions.on("MIDI_MESSAGE",
                     [this](const juce::var& p)
                     {
                         auto hex = [](const juce::var& v) { return v.isVoid() ? juce::String("--") : juce::String::toHexString(static_cast<int>(v)).paddedLeft('0', 2); };
                         messages.push_front(p.getProperty("deviceIdentifier", {}).toString().substring(0, 24) + "  "
                                             + hex(p.getProperty("statusByte", {})) + " " + hex(p.getProperty("data1", {})) + " "
                                             + hex(p.getProperty("data2", {})));
                         while (messages.size() > 40) messages.pop_back();
                         juce::String text;
                         for (const auto& m : messages) text << m << "\n";
                         monitor.setText(text, false);
                     });
    if (const auto last = engine.latest("MIDI_DEVICES_LIST"); !last.isVoid()) devices(last);
    engine.send("MIDI_DEVICES_REQUEST");
}

void MidiPreferencesPage::devices(const juce::var& list)
{
    inputs = list.getProperty("inputs", {});
    deviceList.updateContent();
    deviceList.repaint();
}

int MidiPreferencesPage::getNumRows()
{
    return inputs.isArray() ? inputs.size() : 0;
}

void MidiPreferencesPage::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (!inputs.isArray() || row < 0 || row >= inputs.size()) return;
    const auto d = inputs[row];
    if (selected) g.fillAll(colours::sky600.withAlpha(0.3f));
    const bool enabled = static_cast<bool>(d.getProperty("enabled", false));
    g.setColour(enabled ? colours::emerald500 : colours::zinc600);
    g.fillEllipse(8.0f, height / 2.0f - 5.0f, 10.0f, 10.0f);
    g.setColour(static_cast<bool>(d.getProperty("connected", false)) ? colours::text : colours::textFaint);
    g.setFont(uiFont(13.0f, true));
    g.drawText(d.getProperty("name", {}).toString(), 26, 0, width / 2, height, juce::Justification::centredLeft, true);
    g.setColour(colours::textDim);
    g.setFont(uiFont(11.0f));
    const auto profile = d.getProperty("controllerProfile", {}).toString();
    g.drawText((enabled ? "Enabled" : "Disabled - click to enable") + (profile.isNotEmpty() ? "  -  " + profile : juce::String()),
               width / 2, 0, width / 2 - 8, height, juce::Justification::centredRight, true);
}

void MidiPreferencesPage::listBoxItemClicked(int row, const juce::MouseEvent& e)
{
    if (!inputs.isArray() || row < 0 || row >= inputs.size()) return;
    const auto d = inputs[row];
    const auto id = d.getProperty("identifier", {}).toString();
    if (e.mods.isPopupMenu())
    {
        const auto current = prefs.getSection("midiDevicePreferences").getProperty(id, {});
        juce::PopupMenu menu;
        menu.addItem("Crossfader left to right", true, current.getProperty("crossfaderDirection", "leftToRight").toString() == "leftToRight",
                     [this, id] { setDevicePref(id, "crossfaderDirection", "leftToRight"); });
        menu.addItem("Crossfader right to left", true, current.getProperty("crossfaderDirection", "leftToRight").toString() == "rightToLeft",
                     [this, id] { setDevicePref(id, "crossfaderDirection", "rightToLeft"); });
        menu.addSeparator();
        for (const auto& [value, label] : std::vector<std::pair<juce::String, juce::String>>{{"none", "No default deck"}, {"deck1", "Default to deck 1"}, {"deck2", "Default to deck 2"}})
            menu.addItem(label, true, current.getProperty("defaultDeck", "deck1").toString() == value,
                         [this, id, value = value] { setDevicePref(id, "defaultDeck", value); });
        menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
        return;
    }
    // Click toggles the input (the saved set holds only enabled ones).
    const bool now = !static_cast<bool>(d.getProperty("enabled", false));
    if (now) prefs.set("enabledMidiInputs", id, true);
    else if (auto* enabled = prefs.getSection("enabledMidiInputs").getDynamicObject()) enabled->removeProperty(id);
    prefs.save();
    pushEnginePreferences(engine, prefs);
}

void MidiPreferencesPage::setDevicePref(const juce::String& id, const juce::String& key, const juce::String& value)
{
    auto device = prefs.getSection("midiDevicePreferences").getProperty(id, {});
    auto* o = device.isObject() ? device.getDynamicObject()->clone().release() : new juce::DynamicObject();
    if (!o->hasProperty("scrubAudioEnabled")) o->setProperty("scrubAudioEnabled", false);
    if (!o->hasProperty("crossfaderDirection")) o->setProperty("crossfaderDirection", "leftToRight");
    if (!o->hasProperty("defaultDeck")) o->setProperty("defaultDeck", "deck1");
    o->setProperty(key, value);
    prefs.set("midiDevicePreferences", id, juce::var(o));
    prefs.save();
    pushEnginePreferences(engine, prefs);
}

void MidiPreferencesPage::resized()
{
    auto area = getLocalBounds().reduced(16);
    auto top = area.removeFromTop(28);
    rescan.setBounds(top.removeFromRight(90).reduced(0, 2));
    deviceList.setBounds(area.removeFromTop(area.getHeight() / 2));
    area.removeFromTop(6);
    monitorLabel.setBounds(area.removeFromTop(20));
    monitor.setBounds(area);
}

// ── Developer ──────────────────────────────────────────────────────────────
DeveloperPreferencesPage::DeveloperPreferencesPage(AppPreferences& preferences) : prefs(preferences)
{
    logging.setToggleState(static_cast<bool>(prefs.get("debug", "loggingEnabled", false)), juce::dontSendNotification);
    logging.onClick = [this]
    {
        prefs.set("debug", "loggingEnabled", logging.getToggleState());
        prefs.save();
    };
    const auto dir = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(appFolderName()).getChildFile("Diagnostics");
    folder.setText("Logs and crash reports: " + dir.getFullPathName(), juce::dontSendNotification);
    folder.setColour(juce::Label::textColourId, colours::textDim);
    open.onClick = [dir] { dir.startAsProcess(); };
    addAndMakeVisible(logging);
    addAndMakeVisible(folder);
    addAndMakeVisible(open);
}

void DeveloperPreferencesPage::resized()
{
    auto area = getLocalBounds().reduced(16);
    logging.setBounds(area.removeFromTop(26));
    area.removeFromTop(8);
    folder.setBounds(area.removeFromTop(24));
    area.removeFromTop(6);
    open.setBounds(area.removeFromTop(28).removeFromLeft(150));
}

// ── Timeline ───────────────────────────────────────────────────────────────
} // namespace stemcel::native
