#include "RecordDialog.h"

#include "Db.h"
#include "Theme.h"

namespace stemcel::native
{
namespace
{
constexpr int kProtocol = 1;
const juce::StringArray kWindowModes{"playhead", "start", "selection"};

juce::var payload(std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();
    for (const auto& [k, v] : props)
        if (!v.isVoid()) o->setProperty(k, v);
    return juce::var(o);
}

juce::String clock(double ms)
{
    const int total = static_cast<int>(ms / 1000.0);
    return juce::String(total / 60) + ":" + juce::String(total % 60).paddedLeft('0', 2) + "."
           + juce::String(static_cast<int>(std::fmod(ms, 1000.0) / 100.0));
}
} // namespace

RecordDialog::RecordDialog(EngineClient& client, ProjectMirror& projectMirror, TimelineActions& timelineActions,
                           TimelineViewState& viewState, AppPreferences& preferences)
    : engine(client), mirror(projectMirror), actions(timelineActions), view(viewState), prefs(preferences), subscriptions(client)
{
    for (auto* l : {&inputLabel, &channelLabel, &gainLabel, &whereLabel})
    {
        l->setColour(juce::Label::textColourId, colours::textDim);
        l->setFont(uiFont(13.0f));
        addAndMakeVisible(l);
    }
    statusLabel.setFont(uiFont(18.0f, true));
    statusLabel.setColour(juce::Label::textColourId, colours::text);
    statusLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(statusLabel);
    calibrationLabel.setFont(uiFont(11.0f));
    calibrationLabel.setColour(juce::Label::textColourId, colours::textFaint);
    addAndMakeVisible(calibrationLabel);

    input.onChange = [this]
    {
        const int i = input.getSelectedItemIndex();
        if (i >= 0) control("selectInput", {{"input", payload({{"typeName", inputTypes[i]}, {"deviceName", inputDevices[i]}})}});
    };
    format.addItem("Mono", 1);
    format.addItem("Stereo", 2);
    format.setTooltip("Record one input channel (mono) or a pair (stereo)");
    format.onChange = [this]
    {
        // Keep the channel chosen, as the first of a pair or on its own.
        const int id = channels.getSelectedId() - 1;
        const int count = format.getSelectedId() == 2 ? 2 : 1;
        int first = id >= 0 ? id / 2 : 0;
        if (count == 2 && first + 1 >= channelNames.size()) first = juce::jmax(0, channelNames.size() - 2);
        fillChannels(first, count);
        control("selectChannels", {{"firstChannel", first}, {"channelCount", count}});
    };
    channels.onChange = [this]
    {
        // Ids encode (firstChannel * 2 + count - 1) + 1.
        const int id = channels.getSelectedId() - 1;
        if (id >= 0) control("selectChannels", {{"firstChannel", id / 2}, {"channelCount", id % 2 + 1}});
    };
    gain.setSliderStyle(juce::Slider::LinearHorizontal);
    gain.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 20);
    gain.setRange(-24.0, 24.0, 0.5);
    gain.setTextValueSuffix(" dB");
    gain.setValue(static_cast<double>(prefs.get("audioInput", "gainDb", 0.0)), juce::dontSendNotification);
    gain.onDragEnd = [this]
    {
        prefs.set("audioInput", "gainDb", gain.getValue());
        prefs.save();
    };
    gain.onValueChange = [this] { control("setInputGain", {{"gainDb", gain.getValue()}}); };
    where.addItem("Playhead", 1);
    where.addItem("Project start", 2);
    where.addItem("Timeline selection", 3);
    where.setSelectedId(1, juce::dontSendNotification);
    where.onChange = [this] { control("setWindowMode", {{"mode", kWindowModes[where.getSelectedId() - 1]}}); };
    countIn.addItem("No count-in", 1);
    countIn.addItem("1 bar count-in", 2);
    countIn.setSelectedId(1, juce::dontSendNotification);
    countIn.onChange = [this] { control("setCountInBars", {{"bars", countIn.getSelectedId() - 1}}); };
    mode.addItem("Music (tempo + beats)", 1);
    mode.addItem("Sample (no tempo)", 2);
    mode.setSelectedId(1, juce::dontSendNotification);
    mode.onChange = [this] { control("setRecordingMode", {{"mode", mode.getSelectedId() == 1 ? "music" : "simple"}}); };
    click.onClick = [this] { control("setClickEnabled", {{"enabled", click.getToggleState()}}); };
    monitor.onClick = [this] { control("setMonitorEnabled", {{"enabled", monitor.getToggleState()}}); };
    cleanup.onClick = [this] { control("setCleanupEnabled", {{"enabled", cleanup.getToggleState()}}); };

    recordButton.setColour(juce::TextButton::buttonColourId, colours::red600);
    recordButton.onClick = [this]
    {
        if (status == "recording" || status == "countIn") control("stop");
        else control("start");
    };
    calibrateButton.setTooltip("Measures how late recorded audio arrives (play a click into the microphone)");
    calibrateButton.onClick = [this]
    {
        if (sessionId.isEmpty()) return;
        if (calibrating)
        {
            engine.send("RECORD_CALIBRATE_CANCEL", payload({{"protocolVersion", kProtocol}, {"sessionId", sessionId}}));
            calibrating = false;
            pushCalibration();
            updateEnablement();
            return;
        }
        calibrating = true;
        calibrationLabel.setText("Measuring: turn speakers up so the microphone hears the clicks...", juce::dontSendNotification);
        engine.send("RECORD_CALIBRATE_START", payload({{"protocolVersion", kProtocol}, {"sessionId", sessionId}}));
    };
    closeButton.onClick = [this] { close(); };
    // Remembered for the session, as the renderer's recording store does.
    static bool rememberedSplit = false, rememberedEachAsStereo = false;
    splitChannels.setToggleState(rememberedSplit, juce::dontSendNotification);
    eachAsStereo.setToggleState(rememberedEachAsStereo, juce::dontSendNotification);
    saveAsStereo.onClick = [this]
    {
        if (!ready.isObject()) return;
        engine.send("RECORD_RECORDING_SET_STEREO", payload({{"protocolVersion", kProtocol},
                                                            {"sessionId", sessionId},
                                                            {"recordingId", ready.getProperty("recordingId", {})},
                                                            {"enabled", saveAsStereo.getToggleState()}}));
    };
    splitChannels.onClick = [this]
    {
        rememberedSplit = splitChannels.getToggleState();
        updateEnablement();
    };
    eachAsStereo.onClick = [this] { rememberedEachAsStereo = eachAsStereo.getToggleState(); };
    for (auto* c : {&saveAsStereo, &splitChannels, &eachAsStereo}) addChildComponent(c);
    toTimeline.setColour(juce::TextButton::buttonColourId, colours::sky600);
    takeWaves = std::make_unique<WaveformStore>(engine.host().peaksCache());
    takeWaves->addChangeListener(this);
    playTake.onClick = [this] { toggleAudition(); };
    withArrangement.onClick = [this]
    {
        // The arrangement can be brought in and dropped again while the take rolls.
        if (!withArrangement.getToggleState()) stopArrangement();
        else if (auditionPlaying) startArrangement(auditionPositionMs);
        else if (ready.isObject())
            engine.send("TRANSPORT_SEEK", payload({{"positionMs", takeStartMs()}}));
    };
    static double rememberedReviewBackingGain = 1.0;
    backingVolume.setSliderStyle(juce::Slider::LinearHorizontal);
    backingVolume.setTextBoxStyle(juce::Slider::TextBoxRight, false, 44, 18);
    backingVolume.setRange(0.0, 100.0, 1.0);
    backingVolume.setTextValueSuffix("%");
    backingVolume.setValue(rememberedReviewBackingGain * 100.0, juce::dontSendNotification);
    backingVolume.setDoubleClickReturnValue(true, 100.0);
    backingVolume.onValueChange = [this]
    {
        rememberedReviewBackingGain = backingVolume.getValue() / 100.0;
        control("setBackingGain", {{"gain", rememberedReviewBackingGain}});
    };
    for (auto* l : {&summaryLabel, &noticeLabel, &backingLabel})
    {
        l->setFont(uiFont(12.0f));
        l->setColour(juce::Label::textColourId, l == &noticeLabel ? colours::amber400 : colours::textDim);
    }
    noticeLabel.setJustificationType(juce::Justification::topLeft);
    for (auto* c : std::initializer_list<juce::Component*>{&playTake, &withArrangement, &summaryLabel, &noticeLabel, &backingLabel, &backingVolume})
        addChildComponent(c);
    subscriptions.on("PREVIEW_STATE",
                     [this](const juce::var& p)
                     {
                         if (auditionFile.isEmpty()) return;
                         const bool loaded = static_cast<bool>(p.getProperty("isLoaded", false));
                         const bool playing = static_cast<bool>(p.getProperty("isPlaying", false));
                         if (auditionWaiting && loaded && !playing)
                         {
                             auditionWaiting = false;
                             engine.send("PREVIEW_PLAY");
                             return;
                         }
                         if (auditionWaiting) return;
                         auditionLoaded = loaded;
                         if (playing && !auditionPlaying && withArrangement.getToggleState()) startArrangement(auditionPositionMs);
                         if (!playing && auditionPlaying) stopArrangement();
                         auditionPlaying = playing;
                         updateEnablement();
                     });
    subscriptions.on("PREVIEW_POSITION", [this](const juce::var& p)
                     {
                         if (auditionFile.isEmpty()) return;
                         auditionPositionMs = static_cast<double>(p.getProperty("positionMs", 0.0));
                         repaint(takeWaveArea);
                     });
    subscriptions.on("PREVIEW_ENDED", [this](const juce::var&)
                     {
                         if (auditionFile.isEmpty()) return;
                         auditionPlaying = false;
                         auditionPositionMs = 0.0;
                         stopArrangement();
                         updateEnablement();
                     });
    toTimeline.onClick = [this] { commit("timeline"); };
    toLibrary.onClick = [this] { commit("library"); };
    discard.onClick = [this]
    {
        control("discard");
        releaseAudition();
        control("setBackingGain", {{"gain", 1.0}});
        ready = juce::var();
        updateEnablement();
    };
    for (auto* c : std::initializer_list<juce::Component*>{&input, &format, &channels, &gain, &where, &countIn, &mode, &click, &monitor, &cleanup,
                                                           &recordButton, &calibrateButton, &closeButton, &takeName, &toTimeline,
                                                           &toLibrary, &discard})
        addAndMakeVisible(c);

    subscriptions.on("RECORD_INPUTS_LIST",
                     [this](const juce::var& p)
                     {
                         input.clear(juce::dontSendNotification);
                         inputTypes.clear();
                         inputDevices.clear();
                         if (const auto* types = p.getProperty("types", {}).getArray())
                             for (const auto& t : *types)
                                 if (const auto* devices = t.getProperty("devices", {}).getArray())
                                     for (const auto& d : *devices)
                                     {
                                         inputTypes.add(t.getProperty("name", {}).toString());
                                         inputDevices.add(d.toString());
                                         input.addItem(inputTypes[inputTypes.size() - 1] + ": " + d.toString(), inputTypes.size());
                                     }
                         for (int i = 0; i < inputDevices.size(); ++i)
                             if (inputDevices[i] == inputDevice && inputTypes[i] == inputType)
                                 input.setSelectedItemIndex(i, juce::dontSendNotification);
                     });
    subscriptions.on("RECORD_SESSION_STATE", [this](const juce::var& p) { sessionState(p); });
    subscriptions.on("RECORD_INPUT_LEVEL",
                     [this](const juce::var& p)
                     {
                         peakL = juce::jmax(peakL, static_cast<float>(p.getProperty("peakL", 0.0)));
                         peakR = juce::jmax(peakR, static_cast<float>(p.getProperty("peakR", 0.0)));
                     });
    subscriptions.on("RECORD_RECORDING_READY", [this](const juce::var& p) { recordingReady(p); });
    subscriptions.on("RECORD_CALIBRATE_STATE", [this](const juce::var& p) { calibrationState(p); });

    // Open with the remembered input (the same `audioInput` preference Electron uses).
    const auto typeName = prefs.get("audioInput", "typeName", {}).toString();
    const auto deviceName = prefs.get("audioInput", "deviceName", {}).toString();
    engine.send("RECORD_SESSION_OPEN",
                payload({{"protocolVersion", kProtocol},
                         {"input", typeName.isNotEmpty() && deviceName.isNotEmpty()
                                       ? payload({{"typeName", typeName}, {"deviceName", deviceName}})
                                       : juce::var()}}));
    engine.send("RECORD_INPUTS_REQUEST", payload({}));
    updateEnablement();
    startTimerHz(30);
    setSize(600, 640);
}

double RecordDialog::takeStartMs() const
{
    // A take keeps a little audio before its anchor so an early attack is not clipped; the
    // arrangement rolls from where the file starts, or the take is heard late against it.
    return juce::jmax(0.0, static_cast<double>(ready.getProperty("anchorMs", 0.0)) - static_cast<double>(ready.getProperty("preRollMs", 0.0)));
}

void RecordDialog::toggleAudition()
{
    if (!ready.isObject()) return;
    if (auditionPlaying || auditionWaiting)
    {
        engine.send("PREVIEW_STOP");
        auditionPlaying = auditionWaiting = false;
        auditionPositionMs = 0.0;
        stopArrangement();
        updateEnablement();
        return;
    }
    if (auditionLoaded)
    {
        engine.send("PREVIEW_PLAY");
        return;
    }
    auditionFile = ready.getProperty("filePath", {}).toString();
    auditionWaiting = true;
    auditionPositionMs = 0.0;
    auto* p = new juce::DynamicObject();
    p->setProperty("libraryItemId", "");
    p->setProperty("filePath", auditionFile);
    p->setProperty("inMs", 0);
    p->setProperty("durationMs", 0);
    engine.send("PREVIEW_LOAD", juce::var(p));
    updateEnablement();
}

void RecordDialog::startArrangement(double fromTakeMs)
{
    if (!ready.isObject() || arrangementRolling) return;
    engine.send("TRANSPORT_SEEK", payload({{"positionMs", takeStartMs() + fromTakeMs}}));
    engine.send("TRANSPORT_PLAY");
    arrangementRolling = true;
}

void RecordDialog::stopArrangement()
{
    // Leave the timeline as the take found it: stopped, where the take starts.
    if (!arrangementRolling) return;
    arrangementRolling = false;
    engine.send("TRANSPORT_PAUSE");
    if (ready.isObject()) engine.send("TRANSPORT_SEEK", payload({{"positionMs", takeStartMs()}}));
}

void RecordDialog::releaseAudition()
{
    stopArrangement();
    if (auditionFile.isNotEmpty() && (auditionLoaded || auditionPlaying || auditionWaiting)) engine.send("PREVIEW_UNLOAD");
    auditionFile = {};
    auditionLoaded = auditionPlaying = auditionWaiting = false;
    auditionPositionMs = 0.0;
}

void RecordDialog::mouseDown(const juce::MouseEvent& e)
{
    if (!takeWaveArea.contains(e.getPosition()) || !ready.isObject()) return;
    const double duration = static_cast<double>(ready.getProperty("durationMs", 0.0));
    const double ms = juce::jlimit(0.0, duration, (e.x - takeWaveArea.getX()) / static_cast<double>(juce::jmax(1, takeWaveArea.getWidth())) * duration);
    auditionPositionMs = ms;
    if (auditionLoaded || auditionPlaying) engine.send("PREVIEW_SEEK", payload({{"positionMs", ms}}));
    if (arrangementRolling) engine.send("TRANSPORT_SEEK", payload({{"positionMs", takeStartMs() + ms}}));
    repaint(takeWaveArea);
}

RecordDialog::~RecordDialog()
{
    takeWaves->removeChangeListener(this);
    releaseAudition();
    // The setup's backing level comes back once the review is gone.
    if (sessionId.isNotEmpty()) control("setBackingGain", {{"gain", 1.0}});
    if (sessionId.isNotEmpty())
        engine.send("RECORD_SESSION_CLOSE", payload({{"protocolVersion", kProtocol}, {"sessionId", sessionId}}));
}

void RecordDialog::show(EngineClient& engine, ProjectMirror& mirror, TimelineActions& actions, TimelineViewState& view,
                        AppPreferences& prefs)
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(new RecordDialog(engine, mirror, actions, view, prefs));
    options.dialogTitle = "Record Audio";
    options.dialogBackgroundColour = colours::background;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}

void RecordDialog::control(const juce::String& action, std::initializer_list<std::pair<const char*, juce::var>> props)
{
    if (sessionId.isEmpty()) return;
    auto p = payload(props);
    p.getDynamicObject()->setProperty("protocolVersion", kProtocol);
    p.getDynamicObject()->setProperty("sessionId", sessionId);
    p.getDynamicObject()->setProperty("action", action);
    engine.send("RECORD_SESSION_CONTROL", p);
}

void RecordDialog::sessionState(const juce::var& state)
{
    const auto id = state.getProperty("sessionId", {}).toString();
    const bool first = sessionId.isEmpty();
    sessionId = id;
    status = state.getProperty("status", "idle").toString();
    recordedMs = static_cast<double>(state.getProperty("recordedMs", 0.0));
    const auto in = state.getProperty("input", {});
    if (in.isObject())
    {
        const auto device = in.getProperty("deviceName", {}).toString();
        const auto type = in.getProperty("typeName", {}).toString();
        sampleRate = static_cast<double>(in.getProperty("sampleRate", 0.0));
        if (device != inputDevice || type != inputType)
        {
            inputDevice = device;
            inputType = type;
            prefs.set("audioInput", "typeName", type);
            prefs.set("audioInput", "deviceName", device);
            prefs.save();
            for (int i = 0; i < inputDevices.size(); ++i)
                if (inputDevices[i] == device && inputTypes[i] == type) input.setSelectedItemIndex(i, juce::dontSendNotification);
            pushCalibration();
        }
        channelNames.clear();
        if (const auto* names = in.getProperty("channelNames", {}).getArray())
            for (const auto& n : *names) channelNames.add(n.toString());
        fillChannels(static_cast<int>(state.getProperty("firstChannel", 0)), static_cast<int>(state.getProperty("channelCount", 1)));
    }
    where.setSelectedId(juce::jmax(1, kWindowModes.indexOf(state.getProperty("windowMode", "playhead").toString()) + 1),
                        juce::dontSendNotification);
    where.setItemEnabled(3, static_cast<bool>(state.getProperty("hasSelection", false)));
    monitor.setToggleState(static_cast<bool>(state.getProperty("monitorEnabled", false)), juce::dontSendNotification);
    monitor.setEnabled(static_cast<bool>(state.getProperty("monitorAvailable", true)));
    cleanup.setToggleState(static_cast<bool>(state.getProperty("cleanupEnabled", false)), juce::dontSendNotification);
    click.setToggleState(static_cast<bool>(state.getProperty("clickEnabled", false)), juce::dontSendNotification);
    countIn.setSelectedId(static_cast<int>(state.getProperty("countInBars", 0)) + 1, juce::dontSendNotification);
    mode.setSelectedId(state.getProperty("recordingMode", "music").toString() == "simple" ? 2 : 1, juce::dontSendNotification);

    if (status == "error")
        statusLabel.setText(state.getProperty("error", "Recording failed").toString(), juce::dontSendNotification);
    if (first)
    {
        if (std::abs(gain.getValue()) > 0.01) control("setInputGain", {{"gainDb", gain.getValue()}});
        pushCalibration();
    }
    updateEnablement();
}

void RecordDialog::recordingReady(const juce::var& r)
{
    releaseAudition();
    ready = r;
    if (saveAsStereo.getToggleState() && static_cast<int>(r.getProperty("channelCount", 0)) == 1
        && !static_cast<bool>(r.getProperty("stereoDuplicated", false)))
        engine.send("RECORD_RECORDING_SET_STEREO", payload({{"protocolVersion", kProtocol}, {"sessionId", sessionId},
                                                            {"recordingId", r.getProperty("recordingId", {})}, {"enabled", true}}));
    control("setBackingGain", {{"gain", backingVolume.getValue() / 100.0}});
    takeName.setText(r.getProperty("suggestedName", "Recording").toString(), false);
    updateEnablement();
}

void RecordDialog::commit(const juce::String& destination)
{
    if (!ready.isObject()) return;
    int next = 1;
    for (const auto& item : mirror.model().library)
        if (item.id.startsWithChar('l') && item.id.substring(1).containsOnly("0123456789"))
            next = juce::jmax(next, item.id.substring(1).getIntValue() + 1);
    juce::String trackId;
    if (destination == "timeline")
    {
        trackId = view.selectedTrackId;
        if (mirror.model().findTrack(trackId) == nullptr)
        {
            trackId = actions.addTrack();
            mirror.refreshNow();
        }
    }
    const auto name = takeName.getText().trim().isNotEmpty() ? takeName.getText().trim() : ready.getProperty("suggestedName", "Recording").toString();
    // Only a stereo take that was not duplicated from mono has two channels to separate.
    const bool split = canSplit() && splitChannels.getToggleState();
    auto commitPayload = payload({{"protocolVersion", kProtocol},
                                  {"sessionId", sessionId},
                                  {"recordingId", ready.getProperty("recordingId", {})},
                                  {"itemId", "l" + juce::String(next)},
                                  {"name", name},
                                  {"destination", destination},
                                  {"trackId", trackId.isNotEmpty() ? juce::var(trackId) : juce::var()},
                                  {"clipId", destination == "timeline" ? juce::var(EngineClient::newId("clip")) : juce::var()},
                                  {"splitChannels", split},
                                  {"splitAsStereo", split && eachAsStereo.getToggleState()}});
    if (split)
    {
        commitPayload.getDynamicObject()->setProperty("secondItemId", "l" + juce::String(next + 1));
        if (destination == "timeline") commitPayload.getDynamicObject()->setProperty("secondClipId", EngineClient::newId("clip"));
    }
    engine.send("RECORD_RECORDING_COMMIT", commitPayload);
    releaseAudition();
    control("setBackingGain", {{"gain", 1.0}});
    ready = juce::var();
    updateEnablement();
}

juce::String RecordDialog::calibrationKey() const
{
    const auto output = engine.latest("AUDIO_DEVICES_LIST").getProperty("currentDeviceName", {}).toString();
    return inputDevice.trim() + " | " + output.trim();
}

void RecordDialog::pushCalibration()
{
    // Native calibrations live beside Electron's (whose keys hold a NUL JUCE strings can't).
    const auto entry = prefs.get("nativeLatencyCalibrations", calibrationKey(), {});
    const auto roundTrip = entry.getProperty("roundTripMs", {});
    control("setCalibration", {{"roundTripMs", roundTrip}});
    calibrationLabel.setText(roundTrip.isVoid() ? juce::String("Latency: not calibrated (using driver figures)")
                                                : "Latency calibrated: " + juce::String(static_cast<double>(roundTrip), 1) + " ms round trip",
                             juce::dontSendNotification);
}

void RecordDialog::calibrationState(const juce::var& state)
{
    const auto s = state.getProperty("status", {}).toString();
    if (s == "measuring")
    {
        calibrationLabel.setText("Measuring: " + state.getProperty("clicksDetected", 0).toString() + " / "
                                     + state.getProperty("clicksTotal", 0).toString() + " clicks heard",
                                 juce::dontSendNotification);
        return;
    }
    calibrating = false;
    if (s == "measured" && !state.getProperty("roundTripMs", {}).isVoid())
    {
        auto* entry = new juce::DynamicObject();
        entry->setProperty("roundTripMs", state.getProperty("roundTripMs", 0.0));
        entry->setProperty("manual", false);
        entry->setProperty("sampleRate", sampleRate);
        entry->setProperty("measuredAt", juce::Time::getCurrentTime().toISO8601(true));
        prefs.set("nativeLatencyCalibrations", calibrationKey(), juce::var(entry));
        prefs.save();
        pushCalibration();
    }
    else if (s == "failed")
    {
        calibrationLabel.setText("Calibration failed: " + state.getProperty("error", "no clicks heard").toString(), juce::dontSendNotification);
    }
}

bool RecordDialog::canSplit() const
{
    return ready.isObject() && static_cast<int>(ready.getProperty("channelCount", 0)) == 2
           && !static_cast<bool>(ready.getProperty("stereoDuplicated", false));
}

void RecordDialog::updateEnablement()
{
    const bool open = sessionId.isNotEmpty();
    const bool busy = status == "recording" || status == "countIn" || status == "finalising";
    const bool reviewing = ready.isObject();
    for (auto* c : std::initializer_list<juce::Component*>{&input, &format, &channels, &gain, &where, &countIn, &mode, &click, &monitor, &cleanup,
                                                           &calibrateButton})
        c->setEnabled(open && !busy && !reviewing);
    gain.setEnabled(open && !reviewing);
    recordButton.setEnabled(open && !reviewing && status != "finalising");
    recordButton.setButtonText(status == "recording" || status == "countIn" ? "Stop" : "Record");
    for (auto* c : std::initializer_list<juce::Component*>{&takeName, &toTimeline, &toLibrary, &discard}) c->setVisible(reviewing);
    for (auto* c : std::initializer_list<juce::Component*>{&playTake, &withArrangement, &summaryLabel, &backingLabel, &backingVolume})
        c->setVisible(reviewing);
    playTake.setButtonText(auditionPlaying || auditionWaiting ? "Stop" : "Play");
    if (reviewing)
    {
        const int channelCount = static_cast<int>(ready.getProperty("channelCount", 1));
        summaryLabel.setText(clock(static_cast<double>(ready.getProperty("durationMs", 0.0))) + "  -  "
                                 + (channelCount == 2 ? "stereo" : "mono") + "  -  "
                                 + juce::String(juce::roundToInt(static_cast<double>(ready.getProperty("bpm", 0.0)))) + " BPM",
                             juce::dontSendNotification);
        juce::StringArray notices;
        if (static_cast<bool>(ready.getProperty("hitLengthCap", false)))
            notices.add("Recording stopped at the maximum take length; the take is kept.");
        if (const int dropped = static_cast<int>(ready.getProperty("droppedSamples", 0)); dropped > 0)
            notices.add(juce::String(dropped / juce::jmax(1.0, static_cast<double>(ready.getProperty("sampleRate", 44100))) * 1000.0, 0)
                        + " ms of audio was lost while recording (the computer could not keep up).");
        notices.add(canSplit() && splitChannels.getToggleState()
                        ? "Adding this to the timeline places both halves where you recorded them, each on a track of its own."
                        : "Adding this to the timeline places it where you recorded it - on the selected track when that track is empty, otherwise on a new track.");
        noticeLabel.setText(notices.joinIntoString("\n"), juce::dontSendNotification);
    }
    noticeLabel.setVisible(reviewing);
    const bool duplicated = reviewing && static_cast<bool>(ready.getProperty("stereoDuplicated", false));
    saveAsStereo.setVisible(reviewing && (static_cast<int>(ready.getProperty("channelCount", 0)) == 1 || duplicated));
    saveAsStereo.setToggleState(duplicated, juce::dontSendNotification);
    splitChannels.setVisible(reviewing && canSplit());
    eachAsStereo.setVisible(reviewing && canSplit());
    eachAsStereo.setEnabled(splitChannels.getToggleState());
    calibrateButton.setButtonText(calibrating ? "Cancel calibration" : "Calibrate latency...");
    if (calibrating) calibrateButton.setEnabled(open);
    if (!reviewing && status != "error")
        statusLabel.setText(status == "countIn"      ? juce::String("Count-in...")
                            : status == "recording"  ? "Recording  " + clock(recordedMs)
                            : status == "finalising" ? juce::String("Finishing the take...")
                            : open                   ? juce::String("Ready")
                                                     : juce::String("Opening the input..."),
                            juce::dontSendNotification);
    else if (reviewing)
        statusLabel.setText("Take: " + clock(static_cast<double>(ready.getProperty("durationMs", 0.0))), juce::dontSendNotification);
}

void RecordDialog::timerCallback()
{
    // Meter decay.
    peakL *= 0.85f;
    peakR *= 0.85f;
    if (status == "recording") updateEnablement();
    repaint(meterArea);
}

void RecordDialog::close()
{
    if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState(0);
}

void RecordDialog::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
    if (ready.isObject() && !takeWaveArea.isEmpty())
    {
        g.setColour(colours::zinc950);
        g.fillRoundedRectangle(takeWaveArea.toFloat(), 4.0f);
        const double duration = static_cast<double>(ready.getProperty("durationMs", 0.0));
        if (const auto peaks = takeWaves->get(ready.getProperty("filePath", {}).toString()); peaks != nullptr && duration > 0.0)
            drawPeaks(g, *peaks, takeWaveArea.toFloat().reduced(2.0f), 0.0, duration, false, colours::sky400);
        if (duration > 0.0 && (auditionLoaded || auditionPlaying || auditionPositionMs > 0.0))
        {
            const float x = takeWaveArea.getX() + static_cast<float>(auditionPositionMs / duration) * takeWaveArea.getWidth();
            g.setColour(colours::red500);
            g.drawLine(x, static_cast<float>(takeWaveArea.getY()), x, static_cast<float>(takeWaveArea.getBottom()), 1.5f);
        }
    }
    g.setColour(colours::zinc950);
    g.fillRect(meterArea);
    for (int ch = 0; ch < 2; ++ch)
    {
        const float peak = ch == 0 ? peakL : peakR;
        const double pos = db::dbToPosition(db::linearToDb(peak), 0.0);
        auto bar = meterArea.withHeight(meterArea.getHeight() / 2).withY(meterArea.getY() + ch * meterArea.getHeight() / 2).reduced(0, 1);
        g.setColour(peak >= 0.99f ? colours::red500 : peak > 0.7f ? colours::amber400 : colours::emerald500);
        g.fillRect(bar.removeFromLeft(static_cast<int>(bar.getWidth() * pos)));
    }
}

void RecordDialog::fillChannels(int firstChannel, int channelCount)
{
    // Mono lists each channel; stereo each adjacent pair (ids: firstChannel * 2 + count).
    const bool stereo = channelCount == 2 && channelNames.size() >= 2;
    format.setSelectedId(stereo ? 2 : 1, juce::dontSendNotification);
    format.setItemEnabled(2, channelNames.size() >= 2);
    channels.clear(juce::dontSendNotification);
    for (int c = 0; c < channelNames.size(); ++c)
    {
        if (!stereo) channels.addItem(channelNames[c], c * 2 + 1);
        else if (c + 1 < channelNames.size()) channels.addItem(channelNames[c] + " + " + channelNames[c + 1], c * 2 + 2);
    }
    channels.setSelectedId(firstChannel * 2 + (stereo ? 2 : 1), juce::dontSendNotification);
}

void RecordDialog::resized()
{
    auto area = getLocalBounds().reduced(16);
    auto row = [&area](juce::Label* l, juce::Component& c)
    {
        auto r = area.removeFromTop(28);
        if (l != nullptr) l->setBounds(r.removeFromLeft(100));
        c.setBounds(r.reduced(0, 2));
        area.removeFromTop(4);
    };
    row(&inputLabel, input);
    {
        auto r = area.removeFromTop(28);
        channelLabel.setBounds(r.removeFromLeft(100));
        format.setBounds(r.removeFromLeft(100).reduced(0, 2));
        r.removeFromLeft(6);
        channels.setBounds(r.reduced(0, 2));
        area.removeFromTop(4);
    }
    row(&gainLabel, gain);
    meterArea = area.removeFromTop(14).withTrimmedLeft(100);
    area.removeFromTop(8);
    row(&whereLabel, where);
    {
        auto r = area.removeFromTop(28);
        countIn.setBounds(r.removeFromLeft(r.getWidth() / 2 - 4).reduced(0, 2));
        r.removeFromLeft(8);
        mode.setBounds(r.reduced(0, 2));
        area.removeFromTop(4);
    }
    {
        auto r = area.removeFromTop(26);
        click.setBounds(r.removeFromLeft(100));
        monitor.setBounds(r.removeFromLeft(130));
        cleanup.setBounds(r.removeFromLeft(140));
        area.removeFromTop(6);
    }
    statusLabel.setBounds(area.removeFromTop(34));
    {
        auto r = area.removeFromTop(30);
        recordButton.setBounds(r.withSizeKeepingCentre(140, 30));
        area.removeFromTop(8);
    }
    {
        auto w = area.removeFromTop(56);
        playTake.setBounds(w.removeFromLeft(70).withSizeKeepingCentre(70, 30));
        w.removeFromLeft(8);
        takeWaveArea = w;
        summaryLabel.setBounds(area.removeFromTop(20));
        auto a = area.removeFromTop(26);
        withArrangement.setBounds(a.removeFromLeft(220));
        backingLabel.setBounds(a.removeFromLeft(100));
        backingVolume.setBounds(a.removeFromLeft(170));
        area.removeFromTop(4);
        auto r = area.removeFromTop(28);
        takeName.setBounds(r.reduced(0, 2));
        area.removeFromTop(4);
        auto b = area.removeFromTop(30);
        toTimeline.setBounds(b.removeFromLeft(150));
        b.removeFromLeft(6);
        toLibrary.setBounds(b.removeFromLeft(140));
        b.removeFromLeft(6);
        discard.setBounds(b.removeFromLeft(90));
        area.removeFromTop(4);
        auto o = area.removeFromTop(24);
        saveAsStereo.setBounds(o.removeFromLeft(140));
        splitChannels.setBounds(o.removeFromLeft(140));
        eachAsStereo.setBounds(o.removeFromLeft(140));
        noticeLabel.setBounds(area.removeFromTop(44));
    }
    auto bottom = getLocalBounds().reduced(16).removeFromBottom(30);
    closeButton.setBounds(bottom.removeFromRight(90));
    bottom.removeFromRight(8);
    calibrateButton.setBounds(bottom.removeFromLeft(160));
    bottom.removeFromLeft(8);
    calibrationLabel.setBounds(bottom);
}

} // namespace stemcel::native
