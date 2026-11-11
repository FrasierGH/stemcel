#include "PerformanceView.h"

#include "Log.h"
#include "Theme.h"

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
    : ctx(std::move(context)), subscriptions(ctx.engine), browser(ctx.mirror, ctx.rekordbox)
{
    setWantsKeyboardFocus(true);
    for (int d = 1; d <= kDecks; ++d) build(d);
    crossfader = std::make_unique<ControlSlider>(ctx.engine, "crossfader", 0, juce::Slider::LinearHorizontal, "Crossfader");
    addAndMakeVisible(*crossfader);
    addAndMakeVisible(browser);
    browser.onLoad = [this](const BrowserRow& row, int deck) { loadRow(row, deck); };

    addAndMakeVisible(exitButton);
    exitButton.setTooltip("Back to the timeline (Esc); the take is written into it");
    exitButton.onClick = [this] { if (ctx.exit) ctx.exit(); };
    addAndMakeVisible(recButton);
    recButton.setClickingTogglesState(true);
    recButton.setToggleState(true, juce::dontSendNotification);
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
    addAndMakeVisible(status);
    status.setFont(uiFont(12.0F));
    status.setColour(juce::Label::textColourId, colours::textDim);
    addAndMakeVisible(midiLine);
    midiLine.setFont(uiFont(11.0F));
    midiLine.setColour(juce::Label::textColourId, colours::textFaint);
    midiLine.setJustificationType(juce::Justification::centredRight);

    subscriptions.on("PERF_STATE", [this](const juce::var& p) { onState(p); });
    subscriptions.on("PERF_LOADED", [this](const juce::var& p) { onLoaded(p); });
    subscriptions.on("PERF_UI", [this](const juce::var& p) { onUi(p); });
    subscriptions.on("PERF_CUES", [this](const juce::var& p) { onCues(p); });
    subscriptions.on("PERF_REC", [this](const juce::var& p) { take.add(p); });
    subscriptions.on("MIDI_DEVICES_LIST", [this](const juce::var& p) { enableControllers(p); });
    ctx.mirror.addChangeListener(this);

    ctx.engine.send("PERF_OPEN");
    ctx.engine.send("MIDI_DEVICES_REQUEST");
    take.clear();
    auto* rec = new juce::DynamicObject();
    rec->setProperty("on", true);
    ctx.engine.send("PERF_RECORD", juce::var(rec));
    startTimerHz(60);
}

PerformanceView::~PerformanceView()
{
    log::info("performance", "view: closing");
    stopTimer();
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

// --- Engine state -----------------------------------------------------------------------------

void PerformanceView::onState(const juce::var& p)
{
    readState(p, state);
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
        for (int m = 0; m < 8; ++m) d.modes[static_cast<size_t>(m)]->setLit(s.padMode == kModes[m]);
        for (int pad = 0; pad < 8; ++pad)
        {
            bool lit = false;
            if (s.padMode == "hotCue") lit = s.hotCues[static_cast<size_t>(pad)].set();
            else if (s.padMode == "beatLoop")
            {
                const double beat = s.bpm > 0.0 ? 60.0 / s.bpm : 0.5;
                const double lengths[8] = {0.25, 0.5, 1, 2, 4, 8, 16, 32};
                lit = s.loopActive && std::abs((s.loopOutSec - s.loopInSec) - lengths[pad] * beat) < 0.002;
            }
            else if (s.padMode == "keyShift") lit = s.semitones == kKeyPages[juce::jlimit(0, 2, s.keyPage)][pad];
            else if (s.padMode == "keyboard") lit = s.keyboardCue < 0 ? s.hotCues[static_cast<size_t>(pad)].set() : true;
            d.pads[static_cast<size_t>(pad)]->setLit(lit);
            d.pads[static_cast<size_t>(pad)]->setText(padLabel(i + 1, pad));
        }
    }
    crossfader->follow(state.crossfader);
    if (state.midi.size() > 0) midiLine.setText("MIDI  " + state.midi[state.midi.size() - 1], juce::dontSendNotification);
    juce::String text;
    text << (state.cueOutput ? "Headphones: outputs 3-4" : "Headphones: this output has no 3-4");
    if (state.recording) text << "   |   Recording " << juce::String(take.lengthSec(), 0) << " s";
    else text << "   |   Not recording";
    status.setText(text, juce::dontSendNotification);
    if (recButton.getToggleState() != state.recording && !recButton.isMouseButtonDown())
        recButton.setToggleState(state.recording, juce::dontSendNotification);
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
    if (const auto* item = ctx.mirror.model().findLibraryItem(info.itemId))
    {
        info.audioPath = item->audioPath();
        info.anchorSec = item->beatAnchorSec;
    }
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
}

void PerformanceView::onUi(const juce::var& p)
{
    const auto control = p.getProperty("control", "").toString();
    const int deck = static_cast<int>(p.getProperty("deck", 0));
    const double value = static_cast<double>(p.getProperty("value", 0.0));
    if (control == "browse") browser.browse(static_cast<int>(value));
    else if (control == "browseShift")
    {
        // SHIFT + turn: zoom the close-up waveforms.
        for (auto& d : decks) d.display->setZoom(d.display->zoom() * (value > 0 ? 0.8 : 1.25));
    }
    else if (control == "browsePress" && value > 0.5) browser.press(false);
    else if (control == "browsePressShift" && value > 0.5) browser.press(true);
    else if (control == "load" && value > 0.5) loadButton(deck);
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
        o->setProperty("fromDeck", deck == 1 ? 2 : 1);
        ctx.engine.send("PERF_LOAD", juce::var(o));
        d.lastLoadPress = 0.0;
        return;
    }
    d.lastLoadPress = now;
    if (const auto* row = browser.selectedRow()) loadRow(*row, deck);
}

void PerformanceView::loadRow(const BrowserRow& row, int deck)
{
    if (row.itemId.isNotEmpty())
    {
        loadItem(row.itemId, deck);
        return;
    }
    // A rekordbox track joins the library first (with rekordbox's grid and key).
    ImportRequest request;
    request.file = juce::File(row.path);
    request.hints.rekordboxId = row.rekordboxId;
    request.hints.title = row.title;
    request.hints.artist = row.artist;
    request.hints.key = row.key;
    request.hints.bpm = row.bpm;
    juce::Component::SafePointer<PerformanceView> safe(this);
    if (ctx.toast) ctx.toast("Importing " + row.title + "...", true);
    ctx.importer.import(std::vector<ImportRequest>{request},
                        [safe, deck](const juce::StringArray& ids)
                        {
                            if (safe == nullptr || ids.isEmpty()) return;
                            safe->ctx.mirror.refreshNow();
                            safe->loadItem(ids[0], deck);
                        });
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
    const auto cues = item->raw.getProperty("perfCues", {});
    if (cues.isObject())
    {
        o->setProperty("hotCues", cues.getProperty("hot", {}));
        o->setProperty("memoryCues", cues.getProperty("memory", {}));
    }
    ctx.engine.send("PERF_LOAD", juce::var(o));
}

void PerformanceView::finishTake(std::function<void()> then)
{
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
            const double startMs = v.ctx.playheadMs ? v.ctx.playheadMs() : 0.0;
            const double lengthMs = v.take.lengthSec() * 1000.0;
            if (v.take.hasAudio() && v.take.write(v.ctx.engine, v.ctx.mirror, v.ctx.actions, startMs, ++v.takeNumber))
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
    if (controllersEnabled) return;
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
    juce::Array<juce::var> list;
    for (const auto& id : ids) list.add(id);
    auto* o = new juce::DynamicObject();
    o->setProperty("identifiers", list);
    ctx.engine.send("MIDI_INPUTS_SET", juce::var(o));
    if (ctx.toast) ctx.toast("Controller: " + names, true);
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
    if (s.padMode == "sampler") return "S" + juce::String(pad + 1);
    return "FX " + juce::String(pad + 1);
}

// --- Frame updates ----------------------------------------------------------------------------

void PerformanceView::timerCallback()
{
    const double now = nowMs();
    for (int i = 0; i < kDecks; ++i)
    {
        auto& d = decks[static_cast<size_t>(i)];
        const auto& s = state.decks[static_cast<size_t>(i)];
        const bool moving = s.playing || s.previewing || s.touched;
        const double pos = d.posAt + (moving ? (now - d.receivedAt) / 1000.0 * s.rate : 0.0);
        d.display->update(s, infos[static_cast<size_t>(i)], pos, state.masterDeck == i + 1);
    }
    repaint(mixerArea);
}

void PerformanceView::changeListenerCallback(juce::ChangeBroadcaster*) { browser.refresh(); }

bool PerformanceView::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        if (ctx.exit) ctx.exit();
        return true;
    }
    return true; // the timeline's shortcuts stay out of the way here
}

// --- Layout -----------------------------------------------------------------------------------

void PerformanceView::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
    // The mixer's captions and meters.
    g.setColour(colours::panel);
    g.fillRoundedRectangle(mixerArea.toFloat(), 6.0F);
    g.setFont(uiFont(10.0F, true));
    g.setColour(colours::textFaint);
    for (auto& d : decks)
        for (auto* s : {d.trim.get(), d.eqHi.get(), d.eqMid.get(), d.eqLow.get(), d.filter.get()})
            g.drawText(s->caption().toUpperCase(), s->getBounds().translated(0, s->getHeight() - 4).withHeight(12),
                       juce::Justification::centred);
    for (int i = 0; i < kDecks; ++i)
    {
        const auto& s = state.decks[static_cast<size_t>(i)];
        auto fader = decks[static_cast<size_t>(i)].fader->getBounds().toFloat();
        auto meter = juce::Rectangle<float>(i == 0 ? fader.getRight() + 3.0F : fader.getX() - 9.0F, fader.getY(), 6.0F, fader.getHeight());
        drawWell(g, meter);
        const double db = s.peak > 1.0e-5F ? 20.0 * std::log10(s.peak) : -100.0;
        const float level = static_cast<float>(juce::jlimit(0.0, 1.0, (db + 48.0) / 48.0));
        g.setColour(db > -1.0 ? colours::red500 : (db > -9.0 ? colours::amber400 : colours::emerald500));
        g.fillRect(meter.withTop(meter.getBottom() - meter.getHeight() * level));
    }
}

void PerformanceView::layoutDeck(int deck, juce::Rectangle<int> area)
{
    auto& d = decks[static_cast<size_t>(deck - 1)];
    const bool left = deck == 1;
    // The tempo slider on the outer edge, as on the unit.
    auto tempoCol = left ? area.removeFromLeft(34) : area.removeFromRight(34);
    d.tempo->setBounds(tempoCol.reduced(4, 8));
    d.display->setBounds(area.removeFromTop(juce::jmax(150, area.getHeight() - 170)));
    area.removeFromTop(6);
    auto row1 = area.removeFromTop(30);
    const int w = row1.getWidth() / 9;
    for (auto* b : {d.loopIn.get(), d.loopOut.get(), d.reloop.get(), d.halve.get(), d.dbl.get(), d.keyLock.get(),
                    d.quantize.get(), d.sync.get(), d.loadButton.get()})
        b->setBounds(row1.removeFromLeft(w).reduced(2));
    area.removeFromTop(4);
    auto modes = area.removeFromTop(22);
    const int mw = modes.getWidth() / 8;
    for (auto& m : d.modes) m->setBounds(modes.removeFromLeft(mw).reduced(1));
    area.removeFromTop(4);
    auto transport = left ? area.removeFromLeft(90) : area.removeFromRight(90);
    d.cue->setBounds(transport.removeFromTop(transport.getHeight() / 2).reduced(3));
    d.play->setBounds(transport.reduced(3));
    const int pw = area.getWidth() / 4, ph = area.getHeight() / 2;
    for (int i = 0; i < 8; ++i)
        d.pads[static_cast<size_t>(i)]->setBounds(area.getX() + (i % 4) * pw, area.getY() + (i / 4) * ph, pw, ph);
    for (auto& p : d.pads) p->setBounds(p->getBounds().reduced(3));
}

void PerformanceView::layoutMixer(juce::Rectangle<int> area)
{
    mixerArea = area;
    auto r = area.reduced(8);
    crossfader->setBounds(r.removeFromBottom(30).reduced(30, 4));
    const int half = r.getWidth() / 2;
    for (int i = 0; i < kDecks; ++i)
    {
        auto& d = decks[static_cast<size_t>(i)];
        auto col = i == 0 ? r.withWidth(half) : r.withTrimmedLeft(half);
        const int knob = juce::jmin(54, col.getWidth() - 10);
        for (auto* s : {d.trim.get(), d.eqHi.get(), d.eqMid.get(), d.eqLow.get(), d.filter.get()})
            s->setBounds(col.removeFromTop(knob + 4).withSizeKeepingCentre(knob, knob));
        d.hpCue->setBounds(col.removeFromTop(26).reduced(6, 2));
        col.removeFromTop(4);
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
    status.setBounds(bar.removeFromLeft(bar.getWidth() / 2));
    midiLine.setBounds(bar);
    area.removeFromTop(4);
    auto browserArea = area.removeFromBottom(juce::jmax(150, area.getHeight() / 3));
    browser.setBounds(browserArea);
    area.removeFromBottom(6);
    const int mixerW = juce::jlimit(170, 240, area.getWidth() / 6);
    const int deckW = (area.getWidth() - mixerW - 12) / 2;
    layoutDeck(1, area.removeFromLeft(deckW));
    area.removeFromLeft(6);
    layoutDeck(2, area.removeFromRight(deckW));
    area.removeFromRight(6);
    layoutMixer(area);
}

} // namespace stemcel::native::perf
