#include "PerformanceView.h"

#include "Log.h"
#include "Theme.h"
#include "collection/Collection.h"
#include "DjFx.h"

namespace stemcel::native::perf
{
namespace
{
constexpr const char* kModes[8] = {"hotCue", "beatLoop", "beatJump", "sampler", "keyboard", "padFx1", "padFx2", "keyShift"};
constexpr const char* kModeLabels[8] = {"HOT CUE", "BEAT LOOP", "BEAT JUMP", "SAMPLER", "KEYBOARD", "PAD FX1", "PAD FX2", "KEY SHIFT"};
constexpr const char* kBeatLoopLabels[8] = {"1/4", "1/2", "1", "2", "4", "8", "16", "32"};
constexpr const char* kBeatJumpValues[3][4] = {{"FINE", "1/8", "1/4", "1/2"}, {"1", "2", "4", "8"}, {"16", "32", "64", "128"}};
constexpr int kKeyPages[3][8] = {{4, 5, 6, 7, 0, 1, 2, 3}, {-4, -3, -2, -1, -8, -7, -6, -5}, {8, 9, 10, 11, -12, -11, -10, -9}};
const juce::Colour kPadLit{0xffff4040};

double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }
} // namespace

PerformanceView::PerformanceView(Context context)
    : ctx(std::move(context)), subscriptions(ctx.engine), browser(ctx.mirror, ctx.rekordbox),
      driver(ctx.engine, ctx.mirror, ctx.actions, ctx.live)
{
    for (int d = 1; d <= kDecks; ++d)
    {
        auto& box = targetBoxes[static_cast<size_t>(d - 1)];
        box = std::make_unique<juce::ComboBox>();
        box->setTooltip("What this channel plays: its deck, or a track in the timeline (the knobs and FX then act on it)");
        box->onChange = [this, d]
        {
            const int id = targetBoxes[static_cast<size_t>(d - 1)]->getSelectedId();
            const juce::String track = id >= 2 && id - 2 < static_cast<int>(targetIds.size()) ? targetIds[static_cast<size_t>(id - 2)] : juce::String();
            driver.bind(d, track);
            if (track.isEmpty()) decks[static_cast<size_t>(d - 1)].display->showDeck();
        };
        addAndMakeVisible(*box);
    }
    setWantsKeyboardFocus(true);
    for (int d = 1; d <= kDecks; ++d) build(d);
    crossfader = std::make_unique<ControlSlider>(ctx.engine, "crossfader", 0, juce::Slider::LinearHorizontal, "Crossfader");
    addAndMakeVisible(*crossfader);
    buildFx();
    addAndMakeVisible(browser);
    browser.onLoad = [this](const BrowserRow& row, int deck) { loadRow(row, deck); };

    addAndMakeVisible(exitButton);
    exitButton.setTooltip("Back to the timeline (Esc); the take is written into it");
    exitButton.onClick = [this] { if (ctx.exit) ctx.exit(); };
    addAndMakeVisible(recButton);
    recButton.setClickingTogglesState(true);
    recButton.setToggleState(false, juce::dontSendNotification);
    recButton.setTooltip("Record into the timeline: turning it off writes the take");
    recButton.onClick = [this]
    {
        const bool on = recButton.getToggleState();
        if (!on) finishTake();
        else
        {
            take.clear();
            auto* o = new juce::DynamicObject();
            o->setProperty("on", true);
            ctx.engine.send("PERF_RECORD", juce::var(o));
        }
    };
    // The timeline across the top, and the platters (on unless turned off).
    timelineStrip = std::make_unique<TimelineStrip>(ctx.mirror, ctx.actions, ctx.live,
                                                    [this](double ms) { ctx.actions.seek(ms); });
    timelineStrip->pending = [this] { return pendingTake; };
    timelineStrip->onSelection = [this]
    {
        juce::Component::SafePointer<PerformanceView> safe(this);
        juce::Timer::callAfterDelay(200, [safe] { if (safe != nullptr) safe->browser.refresh(); });
    };
    addAndMakeVisible(*timelineStrip);
    for (int d = 0; d < kDecks; ++d)
    {
        // In each deck screen's top corner, beside its title and overview.
        auto& platter = platters[static_cast<size_t>(d)];
        platter = std::make_unique<JogPlatter>(d + 1);
        auto& display = *decks[static_cast<size_t>(d)].display;
        display.addChildComponent(*platter);
        display.setPlatter(platter.get());
    }
    addAndMakeVisible(plattersButton);
    plattersButton.setClickingTogglesState(true);
    plattersButton.setToggleState(static_cast<bool>(ctx.prefs.get("performance", "platters", true)), juce::dontSendNotification);
    plattersButton.setTooltip("Spinning platters beside the decks' waveforms");
    plattersButton.onClick = [this]
    {
        ctx.prefs.set("performance", "platters", plattersButton.getToggleState());
        resized();
    };
    setUpRedo();
    addAndMakeVisible(fourButton);
    fourButton.setClickingTogglesState(true);
    fourButton.setToggleState(static_cast<bool>(ctx.prefs.get("performance", "fourDecks", false)), juce::dontSendNotification);
    fourButton.setTooltip("Four decks: 1 and 2 above, 3 and 4 below, with four channels on the mixer");
    fourButton.onClick = [this]
    {
        ctx.prefs.set("performance", "fourDecks", fourButton.getToggleState());
        ctx.prefs.save();
        // Back to two: the controller's sides back on decks 1 and 2.
        if (!fourDecks())
            for (int side = 1; side <= 2; ++side)
            {
                auto* o = new juce::DynamicObject();
                o->setProperty("side", side);
                o->setProperty("on", false);
                ctx.engine.send("PERF_DECK_LAYER", juce::var(o));
            }
        resized();
    };
    for (int side = 1; side <= 2; ++side)
    {
        auto& b = layerButtons[static_cast<size_t>(side - 1)];
        b.setButtonText(side == 1 ? "CTRL 1" : "CTRL 2");
        b.setTooltip(side == 1 ? "The controller's left deck: deck 1 or deck 3" : "The controller's right deck: deck 2 or deck 4");
        b.onClick = [this, side]
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("side", side);
            o->setProperty("on", !state.layers[static_cast<size_t>(side - 1)]);
            ctx.engine.send("PERF_DECK_LAYER", juce::var(o));
        };
        addChildComponent(b);
    }
    addAndMakeVisible(stackButton);
    stackButton.setClickingTogglesState(true);
    stackButton.setToggleState(static_cast<bool>(ctx.prefs.get("performance", "stackedWaveforms", false)), juce::dontSendNotification);
    stackButton.setTooltip("The decks' waveforms one above the other, on one time scale, so their beats line up");
    stackButton.onClick = [this]
    {
        ctx.prefs.set("performance", "stackedWaveforms", stackButton.getToggleState());
        ctx.prefs.save();
        resized();
    };
    addAndMakeVisible(autoGainButton);
    autoGainButton.setClickingTogglesState(true);
    autoGainButton.setToggleState(static_cast<bool>(ctx.prefs.get("performance", "autoGain", true)), juce::dontSendNotification);
    autoGainButton.setTooltip("Auto gain: each deck's level evened out by its track's loudness, as rekordbox does");
    autoGainButton.onClick = [this]
    {
        ctx.prefs.set("performance", "autoGain", autoGainButton.getToggleState());
        for (int d = 1; d <= kDecks; ++d) applyAutoGain(d);
    };
    addAndMakeVisible(status);
    status.setFont(uiFont(12.0F));
    status.setColour(juce::Label::textColourId, colours::textDim);
    addAndMakeVisible(midiLine);
    midiLine.setFont(uiFont(11.0F));
    midiLine.setColour(juce::Label::textColourId, colours::textFaint);
    midiLine.setJustificationType(juce::Justification::centredRight);

    subscriptions.on("PERF_STATE", [this](const juce::var& p) { onState(p); });
    subscriptions.on("PERF_LOADED", [this](const juce::var& p) { onLoaded(p); });
    subscriptions.on("PERF_LOAD_COMPLETE", [this](const juce::var& p) { onLoadComplete(p); });
    subscriptions.on("PERF_ITEM_ATTACHED", [this](const juce::var& p) { onItemAttached(p); });
    subscriptions.on("PERF_STEMS_READY", [this](const juce::var& p) { onStemsReady(p); });
    take.stemsOf = [this](const juce::String& itemId) { return stemItemsFor(itemId); };
    subscriptions.on("PERF_UI", [this](const juce::var& p) { onUi(p); });
    subscriptions.on("PERF_CUES", [this](const juce::var& p) { onCues(p); });
    subscriptions.on("PERF_REC", [this](const juce::var& p) { take.add(p); });
    subscriptions.on("PERF_SAMPLE_LOADED",
                     [this](const juce::var& p)
                     {
                         const int slot = static_cast<int>(p.getProperty("slot", -1));
                         if (slot >= 0 && slot < 64) sampleNames[static_cast<size_t>(slot)] = p.getProperty("title", "").toString();
                     });
    subscriptions.on("MIDI_DEVICES_LIST", [this](const juce::var& p) { enableControllers(p); });
    subscriptions.on("MIXDOWN_DONE", [this](const juce::var& p) { regionRendered(p); });
    ctx.mirror.addChangeListener(this);
    refreshTargets();

    ctx.engine.send("PERF_OPEN");
    ctx.engine.send("MIDI_DEVICES_REQUEST");
    take.clear();
    startTimerHz(60);
}

PerformanceView::~PerformanceView()
{
    log::info("performance", "view: closing");
    stopTimer();
    driver.finish();
    ctx.mirror.removeChangeListener(this);
    ctx.engine.send("PERF_CLOSE");
    restoreControllers();
}

void PerformanceView::build(int deck)
{
    auto& d = decks[static_cast<size_t>(deck - 1)];
    auto& engine = ctx.engine;
    d.display = std::make_unique<DeckDisplay>(deck, ctx.waveforms);
    d.display->onSeek = [&engine, deck](double sec) { sendControl(engine, "seek", deck, sec); };
    addAndMakeVisible(*d.display);
    auto hold = [&](std::unique_ptr<HoldButton>& b, const juce::String& text, const juce::String& control, juce::Colour lit)
    {
        b = std::make_unique<HoldButton>(text, lit);
        b->onPress = [&engine, control, deck](bool down) { sendControl(engine, control, deck, down ? 1.0 : 0.0); };
        addAndMakeVisible(*b);
    };
    hold(d.play, "PLAY", "play", colours::emerald500);
    hold(d.cue, "CUE", "cue", colours::amber500);
    d.play->setRound(true);
    d.cue->setRound(true);
    hold(d.sync, "SYNC", "sync", colours::accent);
    hold(d.loopIn, "IN", "loopIn", colours::amber400);
    hold(d.loopOut, "OUT", "loopOut", colours::amber400);
    hold(d.reloop, "RELOOP", "reloopExit", colours::amber400);
    hold(d.halve, "1/2X", "callPrev", colours::amber400);
    hold(d.dbl, "2X", "callNext", colours::amber400);
    d.keyLock = std::make_unique<HoldButton>("MT", colours::accent);
    d.keyLock->onPress = [&engine, deck](bool down) { if (down) sendControl(engine, "keyLock", deck, -1.0); };
    addAndMakeVisible(*d.keyLock);
    d.quantize = std::make_unique<HoldButton>("Q", colours::accent);
    d.quantize->onPress = [&engine, deck](bool down) { if (down) sendControl(engine, "quantize", deck, -1.0); };
    addAndMakeVisible(*d.quantize);
    d.hpCue = std::make_unique<HoldButton>("CUE", colours::amber400);
    d.hpCue->onPress = [&engine, deck](bool down) { if (down) sendControl(engine, "hpCue", deck, 1.0); };
    addAndMakeVisible(*d.hpCue);
    d.loadButton = std::make_unique<HoldButton>("LOAD " + juce::String(deck));
    d.loadButton->onPress = [this, deck](bool down) { if (down) loadButton(deck); };
    addAndMakeVisible(*d.loadButton);
    // Stem Mix: the stem pads (separating the track first when the library has no stems of it).
    d.stemsButton = std::make_unique<HoldButton>("STEMS", stemColour(1));
    d.stemsButton->onPress = [this, deck](bool down) { if (down) stemsButtonPressed(deck); };
    addAndMakeVisible(*d.stemsButton);

    auto slider = [&](std::unique_ptr<ControlSlider>& s, const juce::String& control, juce::Slider::SliderStyle style,
                      const juce::String& caption)
    {
        s = std::make_unique<ControlSlider>(engine, control, deck, style, caption);
        addAndMakeVisible(*s);
    };
    slider(d.tempo, "tempo", juce::Slider::LinearVertical, "Tempo");
    slider(d.trim, "trim", juce::Slider::RotaryHorizontalVerticalDrag, "Trim");
    slider(d.eqHi, "eqHi", juce::Slider::RotaryHorizontalVerticalDrag, "Hi");
    slider(d.eqMid, "eqMid", juce::Slider::RotaryHorizontalVerticalDrag, "Mid");
    slider(d.eqLow, "eqLow", juce::Slider::RotaryHorizontalVerticalDrag, "Low");
    slider(d.filter, "filter", juce::Slider::RotaryHorizontalVerticalDrag, "Filter");
    slider(d.fader, "fader", juce::Slider::LinearVertical, "Channel fader");
    // The tempo slider's − end is at the top, as on Pioneer gear.
    d.tempo->setInverted(true);

    for (int i = 0; i < 8; ++i)
    {
        auto& pad = d.pads[static_cast<size_t>(i)];
        pad = std::make_unique<HoldButton>("", kPadLit);
        pad->onPress = [this, deck, i](bool down)
        {
            const auto& s = state.decks[static_cast<size_t>(deck - 1)];
            const bool shift = juce::ModifierKeys::currentModifiers.isShiftDown();
            sendControl(ctx.engine, shift ? "padShift" : "pad", deck, down ? 1.0 : 0.0, i, s.padMode);
        };
        addAndMakeVisible(*pad);
        auto& mode = d.modes[static_cast<size_t>(i)];
        mode = std::make_unique<HoldButton>(kModeLabels[i], colours::accent);
        mode->onPress = [this, deck, i](bool down) { if (down) sendControl(ctx.engine, "padMode", deck, 1.0, 0, kModes[i]); };
        addAndMakeVisible(*mode);
    }
}

void PerformanceView::buildFx()
{
    auto& engine = ctx.engine;
    auto press = [&](std::unique_ptr<HoldButton>& b, const juce::String& text, const juce::String& control, juce::Colour lit = colours::accent)
    {
        b = std::make_unique<HoldButton>(text, lit);
        b->onPress = [&engine, control](bool down) { sendControl(engine, control, 0, down ? 1.0 : 0.0); };
        addAndMakeVisible(*b);
    };
    press(fxPrev, "<", "fxSelectPrev");
    press(fxNext, ">", "fxSelect");
    press(beatPrev, "<", "fxBeatPrev");
    press(beatNext, ">", "fxBeatNext");
    press(fxOnButton, "ON", "fxOn", colours::red500);
    press(releaseButton, "RELEASE", "fxRelease", colours::amber500);
    // Indexed by CH SELECT code: 1, 2, M, 3, 4.
    const char* channelNames[5] = {"1", "2", "M", "3", "4"};
    for (int code = 0; code < 5; ++code)
    {
        auto& b = fxChannelButtons[static_cast<size_t>(code)];
        b = std::make_unique<HoldButton>(channelNames[code], colours::accent);
        b->onPress = [&engine, code](bool down) { if (down) sendControl(engine, "fxChannel", 0, 1.0, code); };
        addAndMakeVisible(*b);
    }
    fxLevelKnob = std::make_unique<ControlSlider>(engine, "fxLevel", 0, juce::Slider::RotaryHorizontalVerticalDrag, "Level/Depth");
    addAndMakeVisible(*fxLevelKnob);
    samplerVolumeKnob = std::make_unique<ControlSlider>(engine, "samplerVolume", 0, juce::Slider::RotaryHorizontalVerticalDrag, "Sampler");
    addAndMakeVisible(*samplerVolumeKnob);
    for (int i = 0; i < static_cast<int>(djfx::ColorFx::count); ++i) colorFxBox.addItem(juce::String("CFX ") + djfx::colorFxName(i), i + 1);
    colorFxBox.setTooltip("Sound Color FX on the FILTER knobs");
    colorFxBox.onChange = [this] { sendControl(ctx.engine, "colorFxSelect", 0, colorFxBox.getSelectedId() - 1); };
    addAndMakeVisible(colorFxBox);
    releaseFxBox.addItem("Release: Vinyl Brake", 1);
    releaseFxBox.addItem("Release: Echo", 2);
    releaseFxBox.addItem("Release: Spin Back", 3);
    releaseFxBox.setTooltip("SHIFT + ON/OFF, and the Release FX pad");
    releaseFxBox.onChange = [this] { sendControl(ctx.engine, "releaseFxSelect", 0, releaseFxBox.getSelectedId() - 1); };
    addAndMakeVisible(releaseFxBox);
    for (int b = 0; b < 4; ++b) samplerBankBox.addItem("Sampler bank " + juce::String(b + 1), b + 1);
    samplerBankBox.onChange = [this] { sendControl(ctx.engine, "samplerBank", 0, samplerBankBox.getSelectedId() - 1); };
    addAndMakeVisible(samplerBankBox);
}

void PerformanceView::loadSampleSlot(int slot)
{
    // SHIFT + an idle sampler pad: the browser's track goes into that slot.
    if (slot < 0 || slot >= 64 || state.sampler.isPlaying(slot)) return;
    const auto* row = browser.selectedRow();
    if (row == nullptr || row->itemId.isEmpty()) return;
    auto* o = new juce::DynamicObject();
    o->setProperty("slot", slot);
    o->setProperty("libraryItemId", row->itemId);
    o->setProperty("title", row->title);
    ctx.engine.send("PERF_SAMPLER_LOAD", juce::var(o));
}

// --- Engine state -----------------------------------------------------------------------------

void PerformanceView::onState(const juce::var& p)
{
    {
        // The meters: a new peak shows at once; otherwise the reading falls 20 dB a second.
        const double now = juce::Time::getMillisecondCounterHiRes();
        const double fallDb = meterAt > 0.0 ? std::min(1.0, (now - meterAt) / 1000.0) * 20.0 : 100.0;
        const auto fall = static_cast<float>(std::pow(10.0, -fallDb / 20.0));
        meterAt = now;
        if (const auto* list = p.getProperty("decks", {}).getArray())
            for (int d = 0; d < std::min(kDecks, list->size()); ++d)
                for (size_t side = 0; side < 2; ++side)
                {
                    const auto& entry = (*list)[d];
                    const auto peak = static_cast<float>(static_cast<double>(entry.getProperty(side == 0 ? "peakL" : "peakR", entry.getProperty("peak", 0.0))));
                    auto& shown = meterLevels[static_cast<size_t>(d)][side];
                    shown = std::max(peak, shown * fall);
                    meterHolds[static_cast<size_t>(d)][side].update(peak, now);
                }
        // Only the meters and their readings change: the fader's column, with the meter beside it.
        for (int d = 0; d < kDecks; ++d)
            repaint(decks[static_cast<size_t>(d)].fader->getBounds().expanded(20, 16).getUnion(meterArea(d + 1).expanded(8.0F, 2.0F).toNearestInt()));
    }
    readState(p, state);
    driver.update(state);
    const double now = nowMs();
    for (int i = 0; i < kDecks; ++i)
    {
        auto& d = decks[static_cast<size_t>(i)];
        const auto& s = state.decks[static_cast<size_t>(i)];
        d.posAt = s.posSec;
        d.receivedAt = now;
        d.play->setLit(s.playing || s.previewing);
        d.cue->setLit(s.loaded && (s.atCue || s.previewing));
        d.sync->setLit(s.sync, s.master ? colours::amber400 : colours::accent);
        d.keyLock->setLit(s.keyLock);
        d.quantize->setLit(s.quantize);
        d.loopIn->setLit(s.loopActive || s.loopInPending);
        d.loopOut->setLit(s.loopActive);
        d.reloop->setLit(s.loopActive);
        d.hpCue->setLit(s.hpCue);
        d.tempo->follow(s.slider);
        d.trim->follow(s.trim);
        d.eqHi->follow(s.eqHi);
        d.eqMid->follow(s.eqMid);
        d.eqLow->follow(s.eqLow);
        d.filter->follow(s.filter);
        d.fader->follow(s.fader);
        const bool stemMix = s.padMode == "stems";
        for (int m = 0; m < 8; ++m)
            d.modes[static_cast<size_t>(m)]->setLit(s.padMode == kModes[m] || (stemMix && m == 3),
                                                    stemMix && m == 3 ? stemColour(1) : colours::accent);
        d.stemsButton->setLit(stemMix || s.hasStems, stemMix ? stemColour(1) : stemColour(1).withAlpha(0.45F));
        d.stemsButton->setText(stemsButtonText(i + 1));
        if (stemMix != !legendAreas[static_cast<size_t>(i)].isEmpty()) resized(); // the key comes and goes
        if (stemMix) repaint(legendAreas[static_cast<size_t>(i)]);
        for (int pad = 0; pad < 8; ++pad)
        {
            bool lit = false;
            if (stemMix)
            {
                const int bit = 1 << (pad % kStems);
                const bool there = !s.hasStems || (s.stemsPresent & bit) != 0;
                lit = s.hasStems && there && (pad < kStems ? (s.stemsOff & bit) == 0 : s.stemsOff == (s.stemsPresent & ~bit));
                d.pads[static_cast<size_t>(pad)]->setLit(lit, stemColour(pad % kStems));
                d.pads[static_cast<size_t>(pad)]->setText(padLabel(i + 1, pad));
                continue;
            }
            if (s.padMode == "hotCue") lit = s.hotCues[static_cast<size_t>(pad)].set();
            else if (s.padMode == "beatLoop")
            {
                const double beat = s.bpm > 0.0 ? 60.0 / s.bpm : 0.5;
                const double lengths[8] = {0.25, 0.5, 1, 2, 4, 8, 16, 32};
                lit = s.loopActive && std::abs((s.loopOutSec - s.loopInSec) - lengths[pad] * beat) < 0.002;
            }
            else if (s.padMode == "keyShift") lit = s.semitones == kKeyPages[juce::jlimit(0, 2, s.keyPage)][pad];
            else if (s.padMode == "keyboard") lit = s.keyboardCue < 0 ? s.hotCues[static_cast<size_t>(pad)].set() : true;
            else if (s.padMode == "sampler")
            {
                const int slot = state.sampler.bank * 16 + (i % 2) * 8 + pad;
                lit = state.sampler.isLoaded(slot);
            }
            else if (s.padMode == "padFx1" || s.padMode == "padFx2") lit = s.padFxHeld == pad || s.padFxHeld == pad + 8;
            d.pads[static_cast<size_t>(pad)]->setLit(lit, colours::accent);
            d.pads[static_cast<size_t>(pad)]->setText(padLabel(i + 1, pad));
        }
    }
    crossfader->follow(state.crossfader);
    fxLevelKnob->follow(state.fx.level);
    samplerVolumeKnob->follow(state.sampler.volume);
    fxOnButton->setLit(state.fx.on);
    for (int code = 0; code < 5; ++code) fxChannelButtons[static_cast<size_t>(code)]->setLit(state.fx.channel == code);
    for (size_t side = 0; side < 2; ++side)
        if (layerButtons[side].getToggleState() != state.layers[side])
        {
            layerButtons[side].setToggleState(state.layers[side], juce::dontSendNotification);
            layerButtons[side].setButtonText(side == 0 ? (state.layers[0] ? "CTRL 3" : "CTRL 1") : (state.layers[1] ? "CTRL 4" : "CTRL 2"));
        }
    if (colorFxBox.getSelectedId() != state.fx.colorType + 1) colorFxBox.setSelectedId(state.fx.colorType + 1, juce::dontSendNotification);
    if (releaseFxBox.getSelectedId() != state.fx.releaseType + 1) releaseFxBox.setSelectedId(state.fx.releaseType + 1, juce::dontSendNotification);
    if (samplerBankBox.getSelectedId() != state.sampler.bank + 1) samplerBankBox.setSelectedId(state.sampler.bank + 1, juce::dontSendNotification);
    repaint(fxArea);
    if (state.midi.size() > 0) midiLine.setText("MIDI  " + state.midi[state.midi.size() - 1], juce::dontSendNotification);
    juce::String text;
    text << (state.cueOutput ? "Headphones: " + state.cueDevice
                             : juce::String("Headphones: no cue output (connect the controller, or an output with 4 channels)"));
    if (state.recording) text << "   |   Recording " << juce::String(take.lengthSec(), 0) << " s";
    else text << "   |   Not recording";
    status.setText(text, juce::dontSendNotification);
    if (recButton.getToggleState() != state.recording && !recButton.isMouseButtonDown())
        recButton.setToggleState(state.recording, juce::dontSendNotification);
    trackTake();
}

void PerformanceView::trackTake()
{
    // While REC is on: where the take will land (the playhead when it started), and each
    // deck's stretches since, while it is heard (playing or held on a cue, its fader up).
    if (!state.recording)
    {
        trackingTake = false;
        if (!pendingTake.empty()) pendingTake.clear();
        openStretch.fill(-1);
        return;
    }
    if (!trackingTake)
    {
        trackingTake = true;
        takeStartMs = ctx.playheadMs ? ctx.playheadMs() : ctx.live.positionMs;
        takeStartClock = state.clock;
        pendingTake.clear();
        openStretch.fill(-1);
    }
    const double rate = state.sampleRate > 0.0 ? state.sampleRate : 44100.0;
    const double at = takeStartMs + (state.clock - takeStartClock) / rate * 1000.0;
    for (int d = 0; d < kDecks; ++d)
    {
        const auto& s = state.decks[static_cast<size_t>(d)];
        const bool heard = (s.playing || s.previewing) && s.fader > 0.001F && !s.bound;
        auto& open = openStretch[static_cast<size_t>(d)];
        if (!heard)
        {
            open = -1;
            continue;
        }
        if (open < 0)
        {
            pendingTake.push_back({d + 1, at, at});
            open = static_cast<int>(pendingTake.size()) - 1;
        }
        pendingTake[static_cast<size_t>(open)].endMs = at;
    }
}

void PerformanceView::onLoaded(const juce::var& p)
{
    log::info("performance", "view: loaded " + juce::JSON::toString(p, true));
    const int deck = static_cast<int>(p.getProperty("deck", 0));
    if (deck < 1 || deck > kDecks) return;
    if (!static_cast<bool>(p.getProperty("ok", false)))
    {
        if (ctx.toast) ctx.toast("Couldn't load deck " + juce::String(deck) + ": " + p.getProperty("error", "").toString(), false);
        return;
    }
    auto& info = infos[static_cast<size_t>(deck - 1)];
    info.serial = static_cast<int>(p.getProperty("serial", 0));
    info.itemId = p.getProperty("libraryItemId", "").toString();
    info.title = p.getProperty("title", "").toString();
    info.artist = p.getProperty("artist", "").toString();
    info.key = p.getProperty("key", "").toString();
    info.bpm = static_cast<double>(p.getProperty("bpm", 0.0));
    info.lengthSec = static_cast<double>(p.getProperty("lengthSec", 0.0));
    info.hasLoudness = p.hasProperty("lufs");
    info.lufs = static_cast<double>(p.getProperty("lufs", 0.0));
    if (const auto* item = ctx.mirror.model().findLibraryItem(info.itemId))
    {
        info.audioPath = item->audioPath();
        info.anchorSec = item->beatAnchorSec;
        info.beats = item->beats;
    }
    else
    {
        // Straight from its file, while it joins the library: the grid the load carried.
        info.audioPath = p.getProperty("path", "").toString();
        info.anchorSec = static_cast<double>(p.getProperty("anchorSec", 0.0));
        info.beats.clear();
        if (const auto* beats = p.getProperty("beats", {}).getArray())
            for (const auto& b : *beats) info.beats.push_back(static_cast<double>(b));
    }
    applyAutoGain(deck);
}

void PerformanceView::onLoadComplete(const juce::var& p)
{
    // The whole track has been read: its length, and its loudness for auto gain.
    const int deck = static_cast<int>(p.getProperty("deck", 0));
    if (deck < 1 || deck > kDecks) return;
    auto& info = infos[static_cast<size_t>(deck - 1)];
    if (info.serial != static_cast<int>(p.getProperty("serial", -1))) return;
    info.lengthSec = static_cast<double>(p.getProperty("lengthSec", info.lengthSec));
    if (p.hasProperty("lufs"))
    {
        info.hasLoudness = true;
        info.lufs = static_cast<double>(p["lufs"]);
        // The DJ collection keeps the loudness, for the next export and load.
        const auto path = p.getProperty("path", "").toString();
        if (const auto* item = ctx.mirror.model().findLibraryItem(info.itemId))
            dj::Collection::shared().setLoudness(item->filePath, info.lufs);
        else if (info.audioPath.isNotEmpty())
            dj::Collection::shared().setLoudness(info.audioPath, info.lufs);
        juce::ignoreUnused(path);
    }
    applyAutoGain(deck);
}

void PerformanceView::onItemAttached(const juce::var& p)
{
    const int deck = static_cast<int>(p.getProperty("deck", 0));
    if (deck < 1 || deck > kDecks) return;
    auto& info = infos[static_cast<size_t>(deck - 1)];
    if (info.serial != static_cast<int>(p.getProperty("serial", -1))) return;
    info.itemId = p.getProperty("libraryItemId", "").toString();
    if (const auto* item = ctx.mirror.model().findLibraryItem(info.itemId)) info.audioPath = item->audioPath();
}

void PerformanceView::applyAutoGain(int deck)
{
    // Toward a reference loudness: down by up to 12 dB, up by up to 6 (what a timeline
    // track's level reaches, so a recorded take still sounds as played). Nothing when off
    // or not measured.
    constexpr double kReferenceLufs = -10.0;
    const auto& info = infos[static_cast<size_t>(deck - 1)];
    const double gain = autoGainButton.getToggleState() && info.hasLoudness ? juce::jlimit(-12.0, 6.0, kReferenceLufs - info.lufs) : 0.0;
    sendControl(ctx.engine, "autoGain", deck, gain);
}

void PerformanceView::onCues(const juce::var& p)
{
    // Hot and memory cues stay with the item, for the next time it is loaded.
    const auto itemId = p.getProperty("libraryItemId", "").toString();
    if (itemId.isEmpty()) return;
    auto* cues = new juce::DynamicObject();
    cues->setProperty("hot", p.getProperty("hotCues", {}));
    cues->setProperty("memory", p.getProperty("memoryCues", {}));
    auto* o = new juce::DynamicObject();
    o->setProperty("itemId", itemId);
    o->setProperty("cues", juce::var(cues));
    ctx.engine.send("LIBRARY_ITEM_SET_PERF_CUES", juce::var(o));
    // And with the track in the DJ collection, where the export and library mode find them.
    if (const auto* item = ctx.mirror.model().findLibraryItem(itemId))
        if (const auto* track = dj::Collection::shared().findByPath(item->filePath))
            dj::Collection::shared().update(track->id, [&cues](dj::Track& t) { t.cues = dj::cuesFromPerf(juce::var(cues), t.cues); });
}

void PerformanceView::onUi(const juce::var& p)
{
    const auto control = p.getProperty("control", "").toString();
    const int deck = static_cast<int>(p.getProperty("deck", 0));
    const double value = static_cast<double>(p.getProperty("value", 0.0));
    // Redo: a deck control on a channel playing the take takes the deck over.
    if (redoTakesOver(control, deck)) return;
    // A channel driving a timeline track: its transport and jog work the timeline.
    if (deck >= 1 && deck <= kDecks && driver.boundTrack(deck).isNotEmpty() &&
        (control.startsWith("play") || control.startsWith("cue") || control.startsWith("jog")))
    {
        driver.transport(control, value);
        return;
    }
    if (control == "browse") browser.browse(static_cast<int>(value));
    else if (control == "browseShift")
    {
        // SHIFT + turn: zoom the close-up waveforms.
        for (auto& d : decks) d.display->setZoom(d.display->zoom() * (value > 0 ? 0.8 : 1.25));
    }
    else if (control == "browsePress" && value > 0.5) browser.press(false);
    else if (control == "browsePressShift" && value > 0.5) browser.press(true);
    else if (control == "load" && value > 0.5) loadButton(deck);
    else if (control == "samplerLoad")
        loadSampleSlot(state.sampler.bank * 16 + ((deck - 1) % 2) * 8 + static_cast<int>(p.getProperty("index", 0)));
}

void PerformanceView::loadButton(int deck)
{
    if (deck < 1 || deck > kDecks) return;
    auto& d = decks[static_cast<size_t>(deck - 1)];
    const double now = nowMs();
    // LOAD twice quickly: Instant Doubles — the other deck's track, where it is.
    if (now - d.lastLoadPress < 400.0)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("deck", deck);
        o->setProperty("fromDeck", deck % 2 == 1 ? deck + 1 : deck - 1); // the deck beside it
        ctx.engine.send("PERF_LOAD", juce::var(o));
        d.lastLoadPress = 0.0;
        return;
    }
    d.lastLoadPress = now;
    if (const auto* row = browser.selectedRow()) loadRow(*row, deck);
}

void PerformanceView::loadRow(const BrowserRow& row, int deck)
{
    if (row.isRegion)
    {
        renderRegion(row, deck);
        return;
    }
    if (driver.boundTrack(deck).isNotEmpty()) targetBoxes[static_cast<size_t>(deck - 1)]->setSelectedId(1); // back to the deck
    deckRegions[static_cast<size_t>(deck - 1)].reset();
    if (row.itemId.isNotEmpty())
    {
        loadItem(row.itemId, deck);
        return;
    }
    // A DJ collection track: the project's item for its file, or it joins the library with
    // the collection's grid, key and cues.
    if (row.collectionId != 0)
    {
        const auto* track = dj::Collection::shared().find(row.collectionId);
        if (track == nullptr) return;
        for (const auto& item : ctx.mirror.model().library)
            if (item.kind == "source" && dj::normalisedPath(item.filePath) == dj::normalisedPath(track->path))
            {
                loadItem(item.id, deck);
                return;
            }
        ImportRequest request;
        request.file = juce::File(track->path);
        request.hints.title = track->title;
        request.hints.artist = track->artist;
        request.hints.key = track->key;
        request.hints.bpm = track->bpm;
        juce::DynamicObject::Ptr hints = new juce::DynamicObject();
        hints->setProperty("title", track->displayTitle());
        hints->setProperty("artist", track->artist);
        hints->setProperty("key", track->key);
        if (const auto beats = track->beatTimes(); track->bpm > 0.0 && !beats.empty())
        {
            double anchor = beats.front();
            for (size_t i = 0; i < beats.size() && i < 4; ++i)
                if (track->beatNumberAt(static_cast<int>(i)) == 1)
                {
                    anchor = beats[i];
                    break;
                }
            request.hints.beatAnchorSec = anchor;
            hints->setProperty("bpm", track->bpm);
            hints->setProperty("beatAnchorSec", anchor);
            juce::Array<juce::var> list;
            for (const double b : beats) list.add(b);
            hints->setProperty("beats", list);
        }
        if (track->hasLoudness) hints->setProperty("lufs", track->loudnessLufs);
        request.hints.perfCues = dj::toPerfCues(*track);
        if (request.hints.perfCues.isObject())
        {
            hints->setProperty("hotCues", request.hints.perfCues.getProperty("hot", {}));
            hints->setProperty("memoryCues", request.hints.perfCues.getProperty("memory", {}));
        }
        loadFile(request.file, deck, hints, request);
        return;
    }
    // A rekordbox track: on the deck at once, and into the library (with rekordbox's grid and key).
    ImportRequest request;
    request.file = juce::File(row.path);
    request.hints.rekordboxId = row.rekordboxId;
    request.hints.title = row.title;
    request.hints.artist = row.artist;
    request.hints.key = row.key;
    request.hints.bpm = row.bpm;
    juce::DynamicObject::Ptr hints = new juce::DynamicObject();
    hints->setProperty("title", row.title);
    hints->setProperty("artist", row.artist);
    hints->setProperty("key", row.key);
    if (row.bpm > 0.0) hints->setProperty("bpm", row.bpm);
    loadFile(request.file, deck, hints, request);
}

void PerformanceView::loadFile(const juce::File& file, int deck, const juce::DynamicObject::Ptr& hints, ImportRequest request)
{
    log::info("performance", "view: load " + file.getFullPathName() + " to deck " + juce::String(deck));
    hints->setProperty("deck", deck);
    hints->setProperty("path", file.getFullPathName());
    ctx.engine.send("PERF_LOAD", juce::var(hints.get()));
    // The take's clips need a library item: the import gives the deck one when it ends.
    juce::Component::SafePointer<PerformanceView> safe(this);
    const auto path = file.getFullPathName();
    ctx.importer.import(std::vector<ImportRequest>{std::move(request)},
                        [safe, path](const juce::StringArray& ids)
                        {
                            if (safe == nullptr || ids.isEmpty()) return;
                            safe->ctx.mirror.refreshNow();
                            auto* o = new juce::DynamicObject();
                            o->setProperty("path", path);
                            o->setProperty("libraryItemId", ids[0]);
                            safe->ctx.engine.send("PERF_ATTACH_ITEM", juce::var(o));
                        });
}

void PerformanceView::preloadSelected()
{
    // Once the cursor has rested on a track for a moment.
    const auto* row = browser.selectedRow();
    juce::String path;
    if (row != nullptr && !row->isRegion)
    {
        if (row->itemId.isNotEmpty())
        {
            if (const auto* item = ctx.mirror.model().findLibraryItem(row->itemId)) path = item->filePath;
        }
        else if (row->collectionId != 0)
        {
            if (const auto* track = dj::Collection::shared().find(row->collectionId)) path = track->path;
        }
        else
            path = row->path;
    }
    const double now = nowMs();
    if (path != preloadCandidate)
    {
        preloadCandidate = path;
        preloadSince = now;
        return;
    }
    if (path.isEmpty() || path == preloaded || now - preloadSince < 250.0) return;
    preloaded = path;
    auto* o = new juce::DynamicObject();
    o->setProperty("path", path);
    ctx.engine.send("PERF_PRELOAD", juce::var(o));
}

void PerformanceView::loadItem(const juce::String& itemId, int deck)
{
    log::info("performance", "view: load " + itemId + " to deck " + juce::String(deck));
    const auto* item = ctx.mirror.model().findLibraryItem(itemId);
    if (item == nullptr) return;
    auto* o = new juce::DynamicObject();
    o->setProperty("deck", deck);
    o->setProperty("libraryItemId", itemId);
    o->setProperty("title", item->displayName());
    o->setProperty("artist", item->raw.getProperty("artist", ""));
    o->setProperty("key", item->key);
    if (item->bpm > 0.0)
    {
        o->setProperty("bpm", item->bpm);
        o->setProperty("beatAnchorSec", item->beatAnchorSec);
    }
    if (!item->beats.empty())
    {
        juce::Array<juce::var> beats;
        for (const double b : item->beats) beats.add(b);
        o->setProperty("beats", beats);
    }
    // The DJ collection's cues for the track, else the item's own.
    auto cues = item->raw.getProperty("perfCues", {});
    if (const auto* track = dj::Collection::shared().findByPath(item->filePath); track != nullptr && !track->cues.empty())
        cues = dj::toPerfCues(*track);
    if (cues.isObject())
    {
        o->setProperty("hotCues", cues.getProperty("hot", {}));
        o->setProperty("memoryCues", cues.getProperty("memory", {}));
    }
    ctx.engine.send("PERF_LOAD", juce::var(o));
}

void PerformanceView::renderRegion(const BrowserRow& row, int deck)
{
    // The stretch is rendered (as it sounds) and loaded like any track.
    const auto folder = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile("Stemcel").getChildFile("Regions");
    folder.createDirectory();
    const auto file = folder.getChildFile("region-" + juce::Uuid().toString().substring(0, 8) + ".wav");
    PendingRegion pending;
    pending.path = file.getFullPathName();
    pending.deck = deck;
    pending.region = {row.regionTrackId, row.regionStartMs, row.regionEndMs, {}};
    pendingRegions.push_back(pending);
    auto* o = new juce::DynamicObject();
    o->setProperty("outputPath", file.getFullPathName());
    o->setProperty("format", "wav");
    o->setProperty("sampleRate", state.sampleRate > 0.0 ? juce::roundToInt(state.sampleRate) : 48000);
    o->setProperty("bitDepth", 24);
    o->setProperty("lengthMode", "range");
    o->setProperty("startMs", row.regionStartMs);
    o->setProperty("lengthMs", row.regionEndMs); // where the render ends
    if (row.regionTrackId.isNotEmpty()) o->setProperty("onlyTrackId", row.regionTrackId);
    if (ctx.toast) ctx.toast("Rendering " + row.title + " (" + row.artist + ") for deck " + juce::String(deck) + "...", true);
    ctx.engine.send("MIXDOWN_START", juce::var(o));
}

void PerformanceView::regionRendered(const juce::var& payload)
{
    const auto path = payload.getProperty("filePath", "").toString();
    const auto it = std::find_if(pendingRegions.begin(), pendingRegions.end(),
                                 [&](const PendingRegion& p) { return juce::File(p.path) == juce::File(path); });
    if (it == pendingRegions.end()) return;
    const auto pending = *it;
    pendingRegions.erase(it);
    juce::Component::SafePointer<PerformanceView> safe(this);
    ctx.importer.import(juce::Array<juce::File>{juce::File(path)},
                        [safe, pending](const juce::StringArray& ids)
                        {
                            if (safe == nullptr || ids.isEmpty()) return;
                            auto& v = *safe;
                            v.ctx.mirror.refreshNow();
                            if (v.driver.boundTrack(pending.deck).isNotEmpty())
                                v.targetBoxes[static_cast<size_t>(pending.deck - 1)]->setSelectedId(1);
                            v.loadItem(ids[0], pending.deck);
                            auto region = pending.region;
                            region.itemId = ids[0];
                            v.deckRegions[static_cast<size_t>(pending.deck - 1)] = region;
                            // A take lands where the stretch was.
                            if (v.ctx.seekTimeline) v.ctx.seekTimeline(region.fromMs);
                        });
}

void PerformanceView::clearRegion(const juce::String& trackId, double fromMs, double toMs)
{
    // The stretch empties (the clips split at its edges), nothing after it moves.
    for (const double edge : {fromMs, toMs})
    {
        ctx.mirror.refreshNow();
        std::vector<juce::String> toSplit;
        for (const auto& t : ctx.mirror.model().tracks)
            if (trackId.isEmpty() || t.id == trackId)
                for (const auto& c : t.clips)
                    if (c.offsetMs < edge - 0.5 && c.endMs() > edge + 0.5) toSplit.push_back(c.id);
        for (const auto& id : toSplit) ctx.actions.splitClipAt(id, edge);
    }
    ctx.mirror.refreshNow();
    std::vector<juce::String> inside;
    for (const auto& t : ctx.mirror.model().tracks)
        if (trackId.isEmpty() || t.id == trackId)
            for (const auto& c : t.clips)
                if (c.offsetMs >= fromMs - 0.5 && c.endMs() <= toMs + 0.5) inside.push_back(c.id);
    if (!inside.empty()) ctx.actions.removeClips(inside);
}

void PerformanceView::refreshTargets()
{
    targetIds.clear();
    for (const auto& t : ctx.mirror.model().tracks) targetIds.push_back(t.id);
    for (int d = 1; d <= kDecks; ++d)
    {
        auto& box = *targetBoxes[static_cast<size_t>(d - 1)];
        const auto bound = driver.boundTrack(d);
        box.clear(juce::dontSendNotification);
        box.addItem("Deck " + juce::String(d), 1);
        int selected = 1;
        for (size_t i = 0; i < targetIds.size(); ++i)
        {
            const auto* t = ctx.mirror.model().findTrack(targetIds[i]);
            box.addItem("Timeline: " + (t != nullptr ? t->name : targetIds[i]), static_cast<int>(i) + 2);
            if (targetIds[i] == bound) selected = static_cast<int>(i) + 2;
        }
        box.setSelectedId(selected, juce::dontSendNotification);
    }
}

void PerformanceView::finishTake(std::function<void()> then)
{
    // Leaving in a redo: its pass is written first, the channels go back to their decks.
    if (redo.active) stopRedo();
    auto* o = new juce::DynamicObject();
    o->setProperty("on", false);
    ctx.engine.send("PERF_RECORD", juce::var(o));
    // The engine's last events (the closing segments) arrive on its next tick.
    juce::Component::SafePointer<PerformanceView> safe(this);
    auto writeIt = [safe, then]
    {
        if (safe != nullptr)
        {
            auto& v = *safe;
            double startMs = v.ctx.playheadMs ? v.ctx.playheadMs() : 0.0;
            const double lengthMs = v.take.lengthSec() * 1000.0;
            std::vector<Region> regions;
            for (const auto& r : v.deckRegions)
                if (r.has_value()) regions.push_back(*r);
            // A take with a timeline stretch on a deck lines up with where that stretch came from.
            for (const auto& r : regions)
                if (const auto aligned = v.take.alignedStart(r.itemId, r.fromMs); aligned.has_value())
                {
                    startMs = *aligned;
                    break;
                }
            auto replace = [&v, regions]
            {
                for (const auto& r : regions) v.clearRegion(r.trackId, r.fromMs, r.toMs);
            };
            if (v.take.hasAudio() && v.take.write(v.ctx.engine, v.ctx.mirror, v.ctx.actions, startMs, ++v.takeNumber, replace))
            {
                // The next take follows this one.
                if (v.ctx.seekTimeline) v.ctx.seekTimeline(startMs + lengthMs);
                if (v.ctx.toast)
                    v.ctx.toast("Performance written into the timeline (" + juce::String(lengthMs / 1000.0, 0) + " s)", true);
            }
            v.take.clear();
        }
        if (then) then();
    };
    juce::Timer::callAfterDelay(120, writeIt);
}

void PerformanceView::enableControllers(const juce::var& envelope)
{
    if (controllersEnabled)
    {
        // The list after enabling: was the controller's port opened? Another program holding it
        // (rekordbox, or its helper for a moment after it quits) keeps it from us; try again.
        if (openAttempts == 0 || controllerOpened) return;
        const auto* inputs = envelope.getProperty("inputs", {}).getArray();
        if (inputs == nullptr) return;
        bool allOpen = true;
        for (const auto& input : *inputs)
            if (controllerIds.contains(input.getProperty("identifier", "").toString()) && !static_cast<bool>(input.getProperty("enabled", false)))
                allOpen = false;
        if (allOpen)
        {
            controllerOpened = true;
            if (ctx.toast) ctx.toast("Controller: " + controllerNames, true);
            return;
        }
        if (openAttempts >= 4)
        {
            if (ctx.toast)
                ctx.toast("Couldn't open the " + controllerNames + ": another program has it (rekordbox?). Close it, then leave and reopen Performance Mode", false);
            openAttempts = 0;
            return;
        }
        juce::Component::SafePointer<PerformanceView> safe(this);
        juce::Timer::callAfterDelay(1500, [safe] { if (safe != nullptr) safe->openControllers(); });
        return;
    }
    // A smoke test drives the decks with injected MIDI; a real controller on the desk stays out.
    if (juce::JUCEApplicationBase::getCommandLineParameters().contains("--smoke-test")) return;
    const auto* inputs = envelope.getProperty("inputs", {}).getArray();
    if (inputs == nullptr) return;
    juce::StringArray ids;
    if (const auto* enabled = ctx.prefs.getSection("enabledMidiInputs").getDynamicObject())
        for (const auto& p : enabled->getProperties())
            if (static_cast<bool>(p.value)) ids.addIfNotAlreadyThere(p.name.toString());
    bool any = false;
    juce::String names;
    for (const auto& input : *inputs)
        if (input.getProperty("performanceMapping", "").toString().isNotEmpty())
        {
            ids.addIfNotAlreadyThere(input.getProperty("identifier", "").toString());
            controllerIds.add(input.getProperty("identifier", "").toString());
            names << (names.isEmpty() ? "" : ", ") << input.getProperty("performanceMapping", "").toString();
            any = true;
        }
    controllersEnabled = true;
    if (!any)
    {
        if (ctx.toast) ctx.toast("No supported DJ controller connected: the mouse works too", true);
        return;
    }
    controllerNames = names;
    controllerInputs = ids;
    openControllers();
}

void PerformanceView::rescanControllers()
{
    controllersEnabled = false;
    controllerOpened = false;
    openAttempts = 0;
    controllerIds.clear();
    controllerInputs.clear();
    ctx.engine.send("MIDI_DEVICES_REQUEST"); // answered in enableControllers
}

void PerformanceView::openControllers()
{
    // Answered by a MIDI_DEVICES_LIST saying which inputs opened (enableControllers above).
    ++openAttempts;
    juce::Array<juce::var> list;
    for (const auto& id : controllerInputs) list.add(id);
    auto* o = new juce::DynamicObject();
    o->setProperty("identifiers", list);
    ctx.engine.send("MIDI_INPUTS_SET", juce::var(o));
}

void PerformanceView::restoreControllers()
{
    if (controllerIds.isEmpty()) return;
    juce::Array<juce::var> list;
    if (const auto* enabled = ctx.prefs.getSection("enabledMidiInputs").getDynamicObject())
        for (const auto& p : enabled->getProperties())
            if (static_cast<bool>(p.value)) list.add(p.name.toString());
    auto* o = new juce::DynamicObject();
    o->setProperty("identifiers", list);
    ctx.engine.send("MIDI_INPUTS_SET", juce::var(o));
}

juce::String PerformanceView::padLabel(int deck, int pad) const
{
    const auto& s = state.decks[static_cast<size_t>(deck - 1)];
    if (s.padMode == "stems")
    {
        // A two-stem split has no BASS or DRUMS: those pads are blank.
        if (s.hasStems && (s.stemsPresent & (1 << (pad % kStems))) == 0) return {};
        return (pad < kStems ? juce::String() : juce::String("SOLO ")) + kStemNames[pad % kStems];
    }
    if (s.padMode == "hotCue") return juce::String::charToString(static_cast<juce::juce_wchar>('A' + pad));
    if (s.padMode == "beatLoop") return kBeatLoopLabels[pad];
    if (s.padMode == "beatJump")
    {
        const auto* value = kBeatJumpValues[juce::jlimit(0, 2, s.beatJumpPage)][pad / 2];
        return pad % 2 == 0 ? juce::String("< ") + value : juce::String(value) + " >";
    }
    if (s.padMode == "keyShift" || s.padMode == "keyboard")
    {
        if (s.padMode == "keyboard" && s.keyboardCue < 0) return "cue " + juce::String::charToString(static_cast<juce::juce_wchar>('A' + pad));
        const int k = kKeyPages[juce::jlimit(0, 2, s.keyPage)][pad];
        return (k > 0 ? "+" : "") + juce::String(k);
    }
    if (s.padMode == "sampler")
    {
        const int slot = state.sampler.bank * 16 + ((deck - 1) % 2) * 8 + pad;
        const auto& name = sampleNames[static_cast<size_t>(slot)];
        return name.isNotEmpty() ? name : "S" + juce::String(slot % 16 + 1);
    }
    // Pad FX: the preset's effect and length (SHIFT pads are the second 8).
    static const int types[2][8] = {{1, 1, 11, 11, 5, 6, 7, 4}, {1, 2, 11, 11, 5, 5, 3, 1}};
    static const char* steps[2][8] = {{"1/2", "3/4", "1/4", "1/8", "1/4", "1", "2", "4"},
                                      {"1/4", "1/4", "1/16", "1/2", "1/8", "1/2", "1", "REL"}};
    const int bank = s.padMode == "padFx2" ? 1 : 0;
    return juce::String(djfx::beatFxName(types[bank][pad])) + " " + steps[bank][pad];
}

// --- Frame updates ----------------------------------------------------------------------------

void PerformanceView::timerCallback()
{
    const double now = nowMs();
    for (int i = 0; i < kDecks; ++i)
    {
        auto& d = decks[static_cast<size_t>(i)];
        const auto& s = state.decks[static_cast<size_t>(i)];
        if (const auto& track = driver.boundTrack(i + 1); track.isNotEmpty())
        {
            std::vector<DeckDisplay::TrackClip> clips;
            juce::String name;
            if (const auto* t = ctx.mirror.model().findTrack(track))
            {
                name = t->name;
                for (const auto& c : t->clips)
                {
                    const auto* item = ctx.mirror.model().findLibraryItem(c.libraryItemId);
                    clips.push_back({c.offsetMs, c.endMs(), c.inMs, c.effectiveTempoRatio, item != nullptr ? item->audioPath() : juce::String()});
                }
            }
            d.display->showTrack(name, std::move(clips), ctx.live.positionMs, ctx.mirror.model().contentEndMs(), ctx.live.playing);
            continue;
        }
        const bool moving = s.playing || s.previewing || s.touched;
        const double target = d.posAt + (moving ? (now - d.receivedAt) / 1000.0 * s.rate : 0.0);
        const double predicted = d.shownPos + (moving ? (now - d.shownAt) / 1000.0 * s.rate : 0.0);
        const double error = target - predicted;
        // A jump (a cue, a loop, a seek) or a stop shows at once; otherwise the drift is eased out.
        d.shownPos = !moving || std::abs(error) > 0.05 ? target : predicted + error * 0.12;
        d.shownAt = now;
        const double pos = d.shownPos;
        d.display->update(s, infos[static_cast<size_t>(i)], pos, state.masterDeck == i + 1);
        if (auto& platter = platters[static_cast<size_t>(i)]; platter->isVisible()) platter->update(s, pos);
    }
    if (stacked != nullptr && stacked->isVisible()) stacked->repaint();
    stemTick();
    preloadSelected();
    redoTick();
}

void PerformanceView::changeListenerCallback(juce::ChangeBroadcaster*)
{
    browser.refresh();
    refreshTargets();
    // A track added or removed: the strip's lanes take their height.
    if (timelineStrip != nullptr && timelineStrip->getHeight() != timelineStrip->preferredHeight()) resized();
}

bool PerformanceView::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        if (ctx.exit) ctx.exit();
        return true;
    }
    return false; // on to the app's shortcuts (the workspace leaves its own out while we show)
}

// --- Layout -----------------------------------------------------------------------------------

void PerformanceView::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
    // The mixer's captions and meters.
    g.setColour(colours::panel);
    g.fillRoundedRectangle(mixerArea.toFloat(), 6.0F);
    // The Beat FX section's display: effect, beats, tempo.
    if (!fxArea.isEmpty())
    {
        auto display = fxArea.reduced(4).removeFromTop(38).toFloat();
        drawWell(g, display);
        g.setColour(state.fx.on ? colours::red500 : colours::text);
        g.setFont(uiFont(13.0F, true));
        g.drawText(state.fx.typeName, display.reduced(6, 2).removeFromTop(18), juce::Justification::centredLeft);
        // It opens the list of effects when clicked.
        juce::Path arrow;
        const auto tip = display.reduced(8.0F, 0.0F).removeFromRight(9.0F).withSizeKeepingCentre(9.0F, 5.0F);
        arrow.addTriangle(tip.getX(), tip.getY(), tip.getRight(), tip.getY(), tip.getCentreX(), tip.getBottom());
        g.setColour(colours::textDim);
        g.fillPath(arrow);
        g.setFont(uiFont(11.0F));
        const juce::String ch = state.fx.channel == kFxMaster ? "MASTER" : "CH" + juce::String(fxChannelUnit(state.fx.channel));
        g.drawText(state.fx.stepLabel + " beat   " + ch + "   " + juce::String(state.fx.bpm, 1) + (state.fx.tap ? " TAP" : " BPM"),
                   display.reduced(6, 2).removeFromBottom(16), juce::Justification::centredLeft);
    }
    for (int deck = 1; deck <= kDecks; ++deck) paintStemLegend(g, deck);
    g.setFont(uiFont(10.0F, true));
    g.setColour(colours::textFaint);
    for (auto& d : decks)
        for (auto* s : {d.trim.get(), d.eqHi.get(), d.eqMid.get(), d.eqLow.get(), d.filter.get()})
            if (s->isVisible())
                g.drawText(s->caption().toUpperCase(), s->getBounds().translated(0, s->getHeight() - 4).withHeight(12),
                           juce::Justification::centred);
    // Each channel's level beside its fader, drawn as the transport bar's master meter is: L and
    // R in segments (green, amber above -12 dB, red above -3 dB) with their peak holds, the
    // scale's marks on the outside, and the louder side's reading in dB under the fader.
    for (int i = 0; i < kDecks; ++i)
    {
        const auto& levels = meterLevels[static_cast<size_t>(i)];
        const auto& holds = meterHolds[static_cast<size_t>(i)];
        if (!decks[static_cast<size_t>(i)].fader->isVisible()) continue;
        const auto area = meterArea(i + 1);
        if (area.getHeight() < 8.0F) continue;
        meter::drawSegments(g, area, levels, {holds[0].held, holds[1].held}, true);
        g.setColour(colours::textFaint);
        for (const int mark : meter::kMarks)
        {
            const float y = area.getBottom() - meter::proportion(static_cast<float>(mark)) * area.getHeight();
            g.fillRect(meterOnRight[static_cast<size_t>(i)] ? area.getRight() + 2.0F : area.getX() - 5.0F, y - 0.5F, 3.0F, 1.0F);
        }
        const auto fader = decks[static_cast<size_t>(i)].fader->getBounds().toFloat();
        const double db = db::linearToDb(std::max(levels[0], levels[1]));
        auto readout = juce::Rectangle<float>(fader.getX() - 16.0F, fader.getBottom() + 2.0F, fader.getWidth() + 32.0F, 12.0F);
        g.setFont(uiFont(10.0F, true));
        g.setColour(db > -12.0 ? meter::zoneColour(static_cast<float>(db)) : colours::textDim);
        g.drawText(db <= -60.0 ? juce::String("-inf") : juce::String(db, 1), readout, juce::Justification::centred);
    }
}

juce::Rectangle<float> PerformanceView::meterArea(int deck) const
{
    const auto fader = decks[static_cast<size_t>(deck - 1)].fader->getBounds().toFloat();
    constexpr float kWidth = 11.0F;
    return {meterOnRight[static_cast<size_t>(deck - 1)] ? fader.getRight() + 3.0F : fader.getX() - 3.0F - kWidth, fader.getY() + 4.0F, kWidth,
            std::max(0.0F, fader.getHeight() - 8.0F)};
}

juce::Rectangle<int> PerformanceView::fxDisplayArea() const
{
    return fxArea.isEmpty() ? juce::Rectangle<int>() : fxArea.reduced(4).removeFromTop(38);
}

void PerformanceView::mouseMove(const juce::MouseEvent& e)
{
    setMouseCursor(fxDisplayArea().contains(e.getPosition()) ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void PerformanceView::mouseDown(const juce::MouseEvent& e)
{
    if (fxDisplayArea().contains(e.getPosition())) showFxMenu();
}

void PerformanceView::showFxMenu()
{
    // In FX SELECT's order (rekordbox's), the one on now ticked.
    juce::PopupMenu menu;
    menu.addSectionHeader("Beat FX");
    for (const auto type : djfx::kBeatFxOrder)
    {
        const int id = static_cast<int>(type);
        menu.addItem(id + 1, djfx::beatFxName(id), true, state.fx.type == id);
    }
    const auto target = fxDisplayArea();
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(localAreaToGlobal(target)).withMinimumWidth(target.getWidth()),
                       [safe = juce::Component::SafePointer<PerformanceView>(this)](int chosen)
                       {
                           if (safe != nullptr && chosen > 0) sendControl(safe->ctx.engine, "fxTypeSelect", 0, chosen - 1);
                       });
}

void PerformanceView::layoutDeck(int deck, juce::Rectangle<int> area, bool compact)
{
    auto& d = decks[static_cast<size_t>(deck - 1)];
    const bool left = deck % 2 == 1; // decks 1 and 3 on the left
    // The tempo slider on the outer edge, as on the unit.
    auto tempoCol = left ? area.removeFromLeft(34) : area.removeFromRight(34);
    d.tempo->setBounds(tempoCol.reduced(4, 8));
    targetBoxes[static_cast<size_t>(deck - 1)]->setBounds(area.removeFromTop(24).withWidth(juce::jmin(320, area.getWidth())).reduced(0, 1));
    area.removeFromTop(2);
    // With four decks (or STACKED) the close-ups are in the stacked lanes; the deck screens keep
    // the title, the time and the overview.
    const bool stackedShown = stackButton.getToggleState() || compact;
    auto displayArea = area.removeFromTop(stackedShown ? DeckDisplay::compactHeight() : juce::jmax(150, area.getHeight() - 170));
    d.display->setCloseUpShown(!stackedShown);
    platters[static_cast<size_t>(deck - 1)]->setVisible(plattersButton.getToggleState());
    d.display->setBounds(displayArea);
    d.display->resized(); // the platter in or out of its corner
    d.display->repaint();
    area.removeFromTop(6);
    auto row1 = area.removeFromTop(30);
    const int w = row1.getWidth() / 10;
    for (auto* b : {d.loopIn.get(), d.loopOut.get(), d.reloop.get(), d.halve.get(), d.dbl.get(), d.keyLock.get(),
                    d.quantize.get(), d.sync.get(), d.stemsButton.get(), d.loadButton.get()})
        b->setBounds(row1.removeFromLeft(w).reduced(2));
    area.removeFromTop(4);
    auto modes = area.removeFromTop(22);
    const int mw = modes.getWidth() / 8;
    for (auto& m : d.modes) m->setBounds(modes.removeFromLeft(mw).reduced(1));
    area.removeFromTop(4);
    auto& legend = legendAreas[static_cast<size_t>(deck - 1)];
    legend = {};
    if (state.decks[static_cast<size_t>(deck - 1)].padMode == "stems")
    {
        legend = (left ? area.withTrimmedLeft(90) : area.withTrimmedRight(90)).removeFromTop(16);
        area.removeFromTop(18);
    }
    // CUE over PLAY at the bottom of the deck, round, as on the controller (they sat level with
    // the top pads before, which looked too high).
    auto transport = left ? area.removeFromLeft(90) : area.removeFromRight(90);
    const int button = juce::jlimit(36, 72, juce::jmin(transport.getWidth() - 12, (transport.getHeight() - 10) / 2));
    d.play->setBounds(transport.removeFromBottom(button + 4).withSizeKeepingCentre(button, button));
    transport.removeFromBottom(6);
    d.cue->setBounds(transport.removeFromBottom(button + 4).withSizeKeepingCentre(button, button));
    const int pw = area.getWidth() / 4, ph = area.getHeight() / 2;
    for (int i = 0; i < 8; ++i)
        d.pads[static_cast<size_t>(i)]->setBounds(area.getX() + (i % 4) * pw, area.getY() + (i / 4) * ph, pw, ph);
    for (auto& p : d.pads) p->setBounds(p->getBounds().reduced(3));
}

void PerformanceView::setDeckShown(int deck, bool shown)
{
    auto& d = decks[static_cast<size_t>(deck - 1)];
    for (juce::Component* c : std::initializer_list<juce::Component*>{
             d.display.get(), d.play.get(), d.cue.get(), d.sync.get(), d.keyLock.get(), d.quantize.get(), d.loopIn.get(),
             d.loopOut.get(), d.reloop.get(), d.halve.get(), d.dbl.get(), d.tempo.get(), d.trim.get(), d.eqHi.get(),
             d.eqMid.get(), d.eqLow.get(), d.filter.get(), d.fader.get(), d.hpCue.get(), d.loadButton.get(),
             d.stemsButton.get(), targetBoxes[static_cast<size_t>(deck - 1)].get()})
        if (c != nullptr) c->setVisible(shown);
    for (auto& p : d.pads) p->setVisible(shown);
    for (auto& m : d.modes) m->setVisible(shown);
    if (!shown) legendAreas[static_cast<size_t>(deck - 1)] = {};
}

void PerformanceView::layoutMixer(juce::Rectangle<int> area)
{
    mixerArea = area;
    auto r = area.reduced(8);
    crossfader->setBounds(r.removeFromBottom(30).reduced(30, 4));
    // The Beat FX section and the sampler under the channels: rows sized to fit its height.
    fxArea = r.removeFromBottom(212);
    auto fx = fxArea.reduced(4);
    fx.removeFromTop(42);
    auto row = fx.removeFromTop(26);
    const int w = row.getWidth() / 4;
    fxPrev->setBounds(row.removeFromLeft(w).reduced(2));
    fxNext->setBounds(row.removeFromLeft(w).reduced(2));
    beatPrev->setBounds(row.removeFromLeft(w).reduced(2));
    beatNext->setBounds(row.removeFromLeft(w).reduced(2));
    row = fx.removeFromTop(26);
    // CH SELECT: 1 2 M, or 1 2 3 4 M with four decks; ON takes the rest.
    const std::vector<int> codes = fourDecks() ? std::vector<int>{0, 1, 3, 4, 2} : std::vector<int>{0, 1, 2};
    const int cw = row.getWidth() / (static_cast<int>(codes.size()) + 1);
    for (int code = 0; code < 5; ++code)
        fxChannelButtons[static_cast<size_t>(code)]->setVisible(std::find(codes.begin(), codes.end(), code) != codes.end());
    for (const int code : codes) fxChannelButtons[static_cast<size_t>(code)]->setBounds(row.removeFromLeft(cw).reduced(2));
    fxOnButton->setBounds(row.reduced(2));
    row = fx.removeFromTop(40);
    fxLevelKnob->setBounds(row.removeFromLeft(row.getWidth() / 2).withSizeKeepingCentre(36, 36));
    releaseButton->setBounds(row.reduced(2, 7));
    colorFxBox.setBounds(fx.removeFromTop(24).reduced(2));
    releaseFxBox.setBounds(fx.removeFromTop(24).reduced(2));
    // The sampler: its bank, and its volume as a knob at the row's end (whatever is left of the
    // section's height, at least a combo box's).
    row = fx.withHeight(juce::jmax(26, fx.getHeight()));
    const int volume = juce::jlimit(22, 30, row.getHeight() - 2);
    samplerVolumeKnob->setBounds(row.removeFromRight(volume + 6).withSizeKeepingCentre(volume, volume));
    samplerBankBox.setBounds(row.withSizeKeepingCentre(row.getWidth(), 24).reduced(2, 0));
    // The channels left to right as the decks sit: 3 1 2 4 with four (as rekordbox's), else 1 2.
    // The meters sit on each strip's inner side.
    const std::vector<int> order = fourDecks() ? std::vector<int>{3, 1, 2, 4} : std::vector<int>{1, 2};
    const int columnWidth = r.getWidth() / static_cast<int>(order.size());
    for (size_t column = 0; column < order.size(); ++column)
    {
        const int i = order[column] - 1;
        auto& d = decks[static_cast<size_t>(i)];
        meterOnRight[static_cast<size_t>(i)] = column < order.size() / 2;
        auto col = r.withX(r.getX() + static_cast<int>(column) * columnWidth).withWidth(columnWidth);
        // The knobs give way before the fader does: it keeps 90 px, with the meter readouts
        // and the CUE button under and over it.
        const int knob = juce::jmax(22, juce::jmin(44, juce::jmin(col.getWidth() - 10, (col.getHeight() - 26 - 4 - 14 - 90) / 5 - 4)));
        for (auto* s : {d.trim.get(), d.eqHi.get(), d.eqMid.get(), d.eqLow.get(), d.filter.get()})
            s->setBounds(col.removeFromTop(knob + 4).withSizeKeepingCentre(knob, knob));
        d.hpCue->setBounds(col.removeFromTop(26).reduced(6, 2));
        col.removeFromTop(4);
        col.removeFromBottom(14); // the meter's reading
        d.fader->setBounds(col.withSizeKeepingCentre(30, col.getHeight()));
    }
}

void PerformanceView::resized()
{
    auto area = getLocalBounds().reduced(6);
    topBar = area.removeFromTop(30);
    auto bar = topBar;
    exitButton.setBounds(bar.removeFromLeft(70).reduced(2));
    recButton.setBounds(bar.removeFromLeft(70).reduced(2));
    autoGainButton.setBounds(bar.removeFromLeft(96).reduced(2));
    plattersButton.setBounds(bar.removeFromLeft(70).reduced(2));
    stackButton.setBounds(bar.removeFromLeft(80).reduced(2));
    fourButton.setBounds(bar.removeFromLeft(76).reduced(2));
    for (auto& b : layerButtons)
    {
        b.setVisible(fourDecks());
        if (fourDecks()) b.setBounds(bar.removeFromLeft(66).reduced(2));
    }
    redoButton.setBounds(bar.removeFromLeft(64).reduced(2));
    keepButton.setBounds(bar.removeFromLeft(56).reduced(2));
    joinLight.setBounds(bar.removeFromLeft(44).reduced(2));
    status.setBounds(bar.removeFromLeft(bar.getWidth() / 2));
    midiLine.setBounds(bar);
    area.removeFromTop(4);
    timelineStrip->setBounds(area.removeFromTop(timelineStrip->preferredHeight()));
    area.removeFromTop(4);
    if (stacked == nullptr)
    {
        stacked = std::make_unique<StackedWaveforms>();
        addChildComponent(*stacked);
    }
    // Four decks always show their close-ups stacked (1 2 3 4); two, when STACKED is on.
    for (int deck = 3; deck <= kDecks; ++deck) setDeckShown(deck, fourDecks());
    std::vector<std::pair<int, DeckDisplay*>> lanes;
    for (int deck = 1; deck <= (fourDecks() ? 4 : 2); ++deck) lanes.emplace_back(deck, decks[static_cast<size_t>(deck - 1)].display.get());
    stacked->setDecks(std::move(lanes));
    stacked->setVisible(stackButton.getToggleState() || fourDecks());
    // The mixer column needs its height (the knobs, a fader long enough to meter, the FX section
    // and the crossfader). The stacked waveforms and the browser give way for it: the browser
    // first, down to a few rows, then the waveforms, down to two short lanes.
    constexpr int kMixerNeeds = 532, kBrowserLeast = 110, kStackedLeast = 100;
    int stackedHeight = stacked->isVisible() ? (fourDecks() ? juce::jlimit(180, 340, area.getHeight() * 7 / 20)
                                                             : juce::jlimit(120, 260, area.getHeight() * 3 / 10))
                                             : 0;
    int browserHeight = juce::jmax(150, (area.getHeight() - stackedHeight) / 3);
    const auto mixerRoom = [&] { return area.getHeight() - stackedHeight - (stackedHeight > 0 ? 4 : 0) - browserHeight - 6; };
    if (mixerRoom() < kMixerNeeds) browserHeight = juce::jmax(kBrowserLeast, browserHeight - (kMixerNeeds - mixerRoom()));
    if (stackedHeight > 0 && mixerRoom() < kMixerNeeds) stackedHeight = juce::jmax(kStackedLeast, stackedHeight - (kMixerNeeds - mixerRoom()));
    if (stackedHeight > 0)
    {
        stacked->setBounds(area.removeFromTop(stackedHeight));
        area.removeFromTop(4);
    }
    auto browserArea = area.removeFromBottom(browserHeight);
    browser.setBounds(browserArea);
    area.removeFromBottom(6);
    const int mixerW = fourDecks() ? juce::jlimit(300, 440, area.getWidth() * 2 / 9) : juce::jlimit(170, 240, area.getWidth() / 6);
    const int deckW = (area.getWidth() - mixerW - 12) / 2;
    auto leftSide = area.removeFromLeft(deckW);
    area.removeFromLeft(6);
    auto rightSide = area.removeFromRight(deckW);
    area.removeFromRight(6);
    if (fourDecks())
    {
        // Decks 1 and 2 above, 3 and 4 below, as rekordbox lays four out.
        auto upperLeft = leftSide.removeFromTop(leftSide.getHeight() / 2);
        auto upperRight = rightSide.removeFromTop(rightSide.getHeight() / 2);
        leftSide.removeFromTop(4);
        rightSide.removeFromTop(4);
        layoutDeck(1, upperLeft, true);
        layoutDeck(2, upperRight, true);
        layoutDeck(3, leftSide, true);
        layoutDeck(4, rightSide, true);
    }
    else
    {
        layoutDeck(1, leftSide);
        layoutDeck(2, rightSide);
    }
    layoutMixer(area);
}

} // namespace stemcel::native::perf
