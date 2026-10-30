#include "PreferencesDialog.h"

#include "PreferencePages.h"
#include "Theme.h"

namespace stemcel::native
{
namespace
{
constexpr int kLabelWidth = 120;
constexpr int kRowHeight = 28;

juce::Component::SafePointer<PreferencesDialog> openDialog;
} // namespace

AudioPreferencesPage::AudioPreferencesPage(EngineClient& engineClient, AppPreferences& preferences)
    : engine(engineClient), prefs(preferences), subscriptions(engineClient)
{
    for (auto* label : {&driverLabel, &deviceLabel})
    {
        label->setFont(uiFont(13.0f));
        label->setColour(juce::Label::textColourId, colours::textDim);
        addAndMakeVisible(label);
    }
    driverBox.onChange = [this] { selectionChanged(); };
    deviceBox.onChange = [this] { selectionChanged(); };
    addAndMakeVisible(driverBox);
    addAndMakeVisible(deviceBox);

    keepAwake.onClick = [this]
    {
        const auto device = deviceBox.getText();
        prefs.setKeepAwakeFor(device, keepAwake.getToggleState());
        prefs.save();
        auto* p = new juce::DynamicObject();
        p->setProperty("enabled", keepAwake.getToggleState());
        engine.send("AUDIO_KEEP_AWAKE_SET", juce::var(p));
    };
    addAndMakeVisible(keepAwake);

    rateLabel.setFont(uiFont(13.0f));
    rateLabel.setColour(juce::Label::textColourId, colours::textDim);
    rateBox.addItem("44 100 Hz - CD quality, lighter load", 44100);
    rateBox.addItem("48 000 Hz - Video standard", 48000);
    rateBox.setSelectedId(static_cast<int>(prefs.get("ui", "defaultProjectSampleRate", 44100)) == 48000 ? 48000 : 44100,
                          juce::dontSendNotification);
    rateBox.onChange = [this]
    {
        prefs.set("ui", "defaultProjectSampleRate", rateBox.getSelectedId());
        prefs.save();
    };
    addAndMakeVisible(rateLabel);
    addAndMakeVisible(rateBox);

    infoLabel.setFont(uiFont(12.0f));
    infoLabel.setColour(juce::Label::textColourId, colours::textFaint);
    infoLabel.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(infoLabel);

    subscriptions.on("AUDIO_DEVICES_LIST", [this](const juce::var& list) { applyDeviceList(list); });
    applyDeviceList(engine.latest("AUDIO_DEVICES_LIST"));

    auto* refresh = new juce::DynamicObject();
    refresh->setProperty("refresh", true);
    engine.send("AUDIO_DEVICES_REQUEST", juce::var(refresh));
}

void AudioPreferencesPage::applyDeviceList(const juce::var& list)
{
    if (list.isVoid()) return;
    deviceList = list;
    const juce::ScopedValueSetter<bool> guard(updating, true);

    const auto currentType = list.getProperty("currentTypeName", {}).toString();
    const auto currentDevice = list.getProperty("currentDeviceName", {}).toString();

    driverBox.clear(juce::dontSendNotification);
    deviceBox.clear(juce::dontSendNotification);
    int typeId = 1;
    if (const auto* types = list.getProperty("types", {}).getArray())
    {
        for (const auto& type : *types)
        {
            const auto typeName = type.getProperty("name", {}).toString();
            driverBox.addItem(typeName, typeId);
            if (typeName == currentType)
            {
                driverBox.setSelectedId(typeId, juce::dontSendNotification);
                int deviceId = 1;
                if (const auto* devices = type.getProperty("devices", {}).getArray())
                {
                    for (const auto& device : *devices)
                    {
                        deviceBox.addItem(device.toString(), deviceId);
                        if (device.toString() == currentDevice)
                            deviceBox.setSelectedId(deviceId, juce::dontSendNotification);
                        ++deviceId;
                    }
                }
            }
            ++typeId;
        }
    }

    keepAwake.setToggleState(prefs.keepAwakeFor(currentDevice), juce::dontSendNotification);

    juce::StringArray info;
    if (currentDevice.isEmpty())
        info.add("No output device is open.");
    else
        info.add("Open: " + currentDevice + " (" + currentType + ")");
    if (const auto rate = list.getProperty("currentSampleRate", {}); !rate.isVoid())
        info.add(juce::String(static_cast<double>(rate) / 1000.0, 1) + " kHz, buffer "
                 + list.getProperty("currentBufferSize", "?").toString() + " samples");
    if (const auto latency = list.getProperty("outputLatencyMs", {}); !latency.isVoid())
        info.add("Output latency " + juce::String(static_cast<double>(latency), 1) + " ms");
    if (static_cast<bool>(list.getProperty("fellBackToDefault", false)))
        info.add("The preferred device was unavailable, so the system default is in use.");
    infoLabel.setText(info.joinIntoString("\n"), juce::dontSendNotification);
}

void AudioPreferencesPage::selectionChanged()
{
    if (updating) return;

    const auto typeName = driverBox.getText();
    // A new driver starts on its first device; the engine falls back to its default.
    const auto deviceName = (driverBox.getText() == deviceList.getProperty("currentTypeName", {}).toString())
                                ? deviceBox.getText()
                                : juce::String();
    prefs.setAudioOutput(typeName, deviceName);
    prefs.save();

    auto* p = new juce::DynamicObject();
    p->setProperty("typeName", typeName);
    p->setProperty("deviceName", deviceName.isEmpty() ? juce::var() : juce::var(deviceName));
    engine.send("AUDIO_DEVICE_SELECT", juce::var(p));
}

void AudioPreferencesPage::resized()
{
    auto area = getLocalBounds().reduced(20);
    auto row = [&area]
    {
        auto r = area.removeFromTop(kRowHeight);
        area.removeFromTop(10);
        return r;
    };
    auto driverRow = row();
    driverLabel.setBounds(driverRow.removeFromLeft(kLabelWidth));
    driverBox.setBounds(driverRow);
    auto deviceRow = row();
    deviceLabel.setBounds(deviceRow.removeFromLeft(kLabelWidth));
    deviceBox.setBounds(deviceRow);
    auto awakeRow = row();
    awakeRow.removeFromLeft(kLabelWidth);
    keepAwake.setBounds(awakeRow);
    auto rateRow = row();
    rateLabel.setBounds(rateRow.removeFromLeft(kLabelWidth));
    rateBox.setBounds(rateRow.removeFromLeft(320));
    area.removeFromLeft(kLabelWidth);
    infoLabel.setBounds(area.removeFromTop(80));
}

PreferencesDialog::PreferencesDialog(EngineClient& engine, AppPreferences& prefs)
    : juce::DialogWindow("Preferences", colours::background, true, true)
{
    auto* tabs = new juce::TabbedComponent(juce::TabbedButtonBar::TabsAtTop);
    tabs->setTabBarDepth(32);
    tabs->addTab("General", colours::background, new GeneralPreferencesPage(prefs), true);
    tabs->addTab("Timeline", colours::background, new TimelinePreferencesPage(prefs), true);
    tabs->addTab("Project", colours::background, new ProjectPreferencesPage(prefs), true);
    tabs->addTab("Audio", colours::background, new AudioPreferencesPage(engine, prefs), true);
    tabs->addTab("MIDI", colours::background, new MidiPreferencesPage(engine, prefs), true);
    tabs->addTab("Effects", colours::background, new EffectsPreferencesPage(engine, prefs), true);
    tabs->addTab("Stems", colours::background, new StemsPreferencesPage(prefs), true);
    tabs->addTab("Developer", colours::background, new DeveloperPreferencesPage(prefs), true);
    tabs->setSize(680, 460);
    setContentOwned(tabs, true);
    setUsingNativeTitleBar(true);
    setResizable(false, false);
}

void PreferencesDialog::closeButtonPressed()
{
    delete this;
}

void PreferencesDialog::show(EngineClient& engine, AppPreferences& prefs)
{
    if (openDialog != nullptr)
    {
        openDialog->toFront(true);
        return;
    }
    auto* dialog = new PreferencesDialog(engine, prefs);
    openDialog = dialog;
    dialog->centreWithSize(dialog->getWidth(), dialog->getHeight());
    dialog->setVisible(true);
}

} // namespace stemcel::native
