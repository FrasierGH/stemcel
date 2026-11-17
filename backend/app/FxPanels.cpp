#include "FxPanels.h"

#include "Theme.h"
#include "Automation.h"
#include "DjFx.h"
#include "performance/PerfCurves.h"
#include "AudioEngine.h"
#include "EngineHost.h"
#include "PluginCatalogue.h"
#include "TimelineActions.h"

#include <tuple>

namespace stemcel::native
{
namespace
{
juce::var object(std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();
    for (const auto& [k, v] : props)
        if (!v.isVoid()) o->setProperty(k, v);
    return juce::var(o);
}

double num(const juce::var& o, const char* key, double fallback)
{
    const auto v = o.getProperty(key, {});
    return v.isVoid() ? fallback : static_cast<double>(v);
}

juce::String percent(double v)
{
    return juce::String(juce::roundToInt(v * 100.0)) + "%";
}

juce::String db(double v)
{
    return (v > 0 ? "+" : "") + juce::String(v, 1) + " dB";
}

constexpr int kModuleHeight = 112;
const juce::StringArray kNoteValues{"1/4", "1/8", "1/8T", "1/16"};
} // namespace

// ── FxKnob ─────────────────────────────────────────────────────────────────
FxKnob::FxKnob(const juce::String& text, double min, double max, double step, double defaultValue, Send sendFn,
               std::function<juce::String(double)> format)
    : send(std::move(sendFn))
{
    slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 64, 16);
    slider.setRange(min, max, step);
    slider.setDoubleClickReturnValue(true, defaultValue);
    slider.setValue(defaultValue, juce::dontSendNotification);
    slider.setColour(juce::Slider::rotarySliderFillColourId, colours::sky500);
    slider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    if (format) slider.textFromValueFunction = format;
    slider.updateText();
    slider.onDragStart = [this] { gestureId = juce::Uuid().toDashedString(); };
    slider.onValueChange = [this]
    {
        // A change outside a drag (double-click reset, typing) is its own complete step.
        const bool dragging = slider.isMouseButtonDown();
        send(slider.getValue(), dragging ? gestureId : juce::String(), !dragging);
    };
    slider.onDragEnd = [this]
    {
        send(slider.getValue(), gestureId, true);
        gestureId.clear();
    };
    addAndMakeVisible(slider);
    label.setText(text, juce::dontSendNotification);
    label.setJustificationType(juce::Justification::centred);
    label.setFont(uiFont(11.0f));
    label.setColour(juce::Label::textColourId, colours::textDim);
    addAndMakeVisible(label);
}

void FxKnob::setCurve(std::function<double(double)> toValue, std::function<double(double)> toTravel)
{
    const double value = slider.getValue();
    juce::NormalisableRange<double> range(slider.getMinimum(), slider.getMaximum(),
                                          [toValue](double, double, double travel) { return toValue(travel); },
                                          [toTravel](double, double, double v) { return toTravel(v); });
    slider.setNormalisableRange(range);
    slider.setValue(value, juce::dontSendNotification);
}

void FxKnob::setValue(double value)
{
    if (!slider.isMouseButtonDown()) slider.setValue(value, juce::dontSendNotification);
}

void FxKnob::resized()
{
    auto area = getLocalBounds();
    label.setBounds(area.removeFromTop(14));
    slider.setBounds(area);
}

// ── FxModule ───────────────────────────────────────────────────────────────
FxModule::FxModule(const juce::String& moduleTitle) : title(moduleTitle)
{
    automate.setClickingTogglesState(false);
    automate.setTooltip("Show or hide this effect's automation lanes on the timeline, to draw them by hand");
    addChildComponent(automate);
}

void FxModule::addTitleButton(juce::Component& button, int width)
{
    addAndMakeVisible(button);
    titleButtons.emplace_back(&button, width);
}

void FxModule::setAutomation(juce::StringArray laneIds, std::function<void()> toggle)
{
    lanes = std::move(laneIds);
    automate.onClick = std::move(toggle);
    automate.setVisible(!lanes.isEmpty());
}

void FxModule::setTitle(const juce::String& newTitle)
{
    title = newTitle;
    repaint();
}

FxKnob& FxModule::add(std::unique_ptr<FxKnob> knob)
{
    addAndMakeVisible(*knob);
    knobs.push_back(std::move(knob));
    return *knobs.back();
}

void FxModule::addExtra(juce::Component& extra, int width)
{
    addAndMakeVisible(extra);
    extras.emplace_back(&extra, width);
}

int FxModule::preferredWidth() const
{
    int w = 16 + static_cast<int>(knobs.size()) * 72;
    for (const auto& e : extras) w += e.second + 6;
    return juce::jmax(110, w);
}

void FxModule::paint(juce::Graphics& g)
{
    g.setColour(colours::zinc800);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0f);
    g.setColour(colours::text);
    g.setFont(uiFont(12.0f, true));
    int right = getWidth() - 8 - (automate.isVisible() ? 22 : 0);
    for (const auto& b : titleButtons) right -= b.second + 2;
    g.drawText(title, 8, 4, juce::jmax(10, right - 8), 16, juce::Justification::centredLeft, true);
}

void FxModule::resized()
{
    auto area = getLocalBounds().reduced(8, 4);
    auto titleRow = area.removeFromTop(18);
    if (automate.isVisible()) automate.setBounds(titleRow.removeFromRight(20).reduced(0, 1));
    for (auto& [b, w] : titleButtons)
    {
        titleRow.removeFromRight(2);
        b->setBounds(titleRow.removeFromRight(w).reduced(0, 1));
    }
    for (auto& k : knobs) k->setBounds(area.removeFromLeft(72));
    for (auto& [c, w] : extras)
    {
        area.removeFromLeft(6);
        c->setBounds(area.removeFromLeft(w).withSizeKeepingCentre(w, juce::jmin(area.getHeight(), 80)));
    }
}

namespace
{
void layoutModules(std::vector<std::unique_ptr<FxModule>>& modules, juce::Rectangle<int> area)
{
    int x = area.getX();
    int y = area.getY();
    for (auto& m : modules)
    {
        const int w = m->preferredWidth();
        if (x + w > area.getRight() && x > area.getX())
        {
            x = area.getX();
            y += kModuleHeight + 8;
        }
        m->setBounds(x, y, w, kModuleHeight);
        x += w + 8;
    }
}

int modulesHeight(const std::vector<std::unique_ptr<FxModule>>& modules, int width)
{
    int x = 0, rowsCount = 1;
    for (const auto& m : modules)
    {
        const int w = m->preferredWidth();
        if (x + w > width && x > 0)
        {
            x = 0;
            ++rowsCount;
        }
        x += w + 8;
    }
    return rowsCount * (kModuleHeight + 8);
}
} // namespace

// ── Track FX ───────────────────────────────────────────────────────────────
namespace
{
// A knob's project-state key to its automation lane (Automation.cpp params / masterParams).
const std::map<juce::String, juce::String>& knobLanes()
{
    static const std::map<juce::String, juce::String> lanes{
        {"toneBassDb", "toneBass"}, {"toneMidDb", "toneMid"}, {"toneTrebleDb", "toneTreble"}, {"toneFilter", "filter"},
        {"sendReverb", "reverbSend"}, {"sendDelay", "delaySend"}, {"punchAmount", "punch"},
        {"saturationDrive", "saturationDrive"}, {"saturationMix", "saturationMix"}, {"bitCrusherRate", "bitCrusherRate"},
        {"bitCrusherBits", "bitCrusherBits"}, {"bitCrusherBoost", "bitCrusherBoost"}, {"bitCrusherMix", "bitCrusherMix"},
        {"reverbSize", "reverbSize"}, {"reverbDecay", "reverbDecay"}, {"reverbTone", "reverbTone"}, {"reverbMix", "reverbMix"},
        {"delayFeedback", "delayFeedback"}, {"delayTone", "delayTone"}, {"delayMix", "delayMix"}, {"mixGlueAmount", "glue"}, {"mixGlueMakeupDb", "glueMakeup"}};
    return lanes;
}

std::vector<std::pair<juce::String, juce::String>> lanesFor(const std::map<juce::String, FxKnob*>& knobs)
{
    std::vector<std::pair<juce::String, juce::String>> out;
    for (const auto& [key, knob] : knobs)
    {
        const auto it = knobLanes().find(key);
        out.emplace_back(key, it != knobLanes().end() ? it->second : juce::String());
    }
    return out;
}
} // namespace

std::vector<std::pair<juce::String, juce::String>> TrackFxView::knobAutomation() const
{
    return lanesFor(knobs);
}

std::vector<std::pair<juce::String, juce::String>> ProjectFxView::knobAutomation() const
{
    return lanesFor(knobs);
}

TrackFxView::TrackFxView(LibraryContext& context) : ctx(context)
{
    heading.setFont(uiFont(13.0f, true));
    heading.setColour(juce::Label::textColourId, colours::text);
    addAndMakeVisible(heading);
    viewport.setViewedComponent(&content, false);
    viewport.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport);

    auto knob = [this](FxModule& m, const juce::String& key, const juce::String& label, double min, double max,
                       double step, double def, FxKnob::Send send, std::function<juce::String(double)> fmt = {})
    {
        auto& k = m.add(std::make_unique<FxKnob>(label, min, max, step, def, std::move(send), std::move(fmt)));
        knobs[key] = &k;
    };
    auto module = [this](const juce::String& title) -> FxModule&
    {
        modules.push_back(std::make_unique<FxModule>(title));
        content.addAndMakeVisible(*modules.back());
        return *modules.back();
    };

    addEffect.setTooltip("Add one of performance mode's effects, or a plugin, to this track");
    addEffect.onClick = [this] { showAddMenu(); };
    addAndMakeVisible(addEffect);

    // Tone and filter turn as performance mode's EQ and FILTER knobs do (the same curves, the
    // same DSP), so a take's curves sit where the knobs were: kill at the left, 0 dB at the
    // centre, +6 dB at the right; the filter low-pass left, high-pass right, a detent between.
    auto& tone = module("Tone");
    for (const auto& [key, label, field] : std::vector<std::tuple<juce::String, juce::String, const char*>>{
             {"toneBassDb", "Bass", "bassDb"}, {"toneMidDb", "Mid", "midDb"}, {"toneTrebleDb", "Treble", "trebleDb"}})
    {
        knob(tone, key, label, stemcel::perf::curves::kEqKillDb, 6.0, 0.0, 0.0,
             [this, field = juce::String(field)](double v, const juce::String& g, bool end)
             { sendTrack("TRACK_SET_TONE", {{field.toRawUTF8(), v}}, g, end); },
             [](double v) { return v <= stemcel::perf::curves::kEqKillDb + 0.5 ? juce::String("Kill") : db(v); });
        knobs[key]->setCurve([](double travel) { return stemcel::perf::curves::eqDb(travel); },
                             [](double v)
                             {
                                 if (v >= 0.0) return juce::jlimit(0.5, 1.0, 0.5 + v / 12.0);
                                 return v <= stemcel::perf::curves::kEqKillDb + 1.0e-3 ? 0.0 : 0.5 * std::pow(10.0, v / 20.0);
                             });
    }
    auto& filter = module("Filter");
    knob(filter, "toneFilter", "LP / HP", -1.0, 1.0, 0.0, 0.0,
         [this](double v, const juce::String& g, bool end) { sendTrack("TRACK_SET_TONE", {{"filter", v}}, g, end); },
         [](double v) { return std::abs(v) < 0.005 ? juce::String("Off") : (v < 0 ? "LP " : "HP ") + percent(std::abs(v)); });
    knobs["toneFilter"]->setCurve([](double travel) { return stemcel::perf::curves::filterAmount(travel); },
                                  [](double v)
                                  {
                                      constexpr double kDead = 0.03;
                                      if (std::abs(v) < 1.0e-6) return 0.5;
                                      return 0.5 + std::copysign(kDead + std::abs(v) * (1.0 - kDead), v) * 0.5;
                                  });
    auto& sends = module("Sends");
    knob(sends, "sendReverb", "Room", 0.0, 1.0, 0.01, 0.0,
         [this](double v, const juce::String& g, bool end)
         {
             const auto* t = ctx.mirror.model().findTrack(ctx.view.selectedTrackId);
             sendTrack("TRACK_SET_SENDS", {{"reverbSend", v}, {"delaySend", t ? num(t->raw, "sendDelay", 0.0) : 0.0}}, g, end);
         },
         percent);
    knob(sends, "sendDelay", "Echo", 0.0, 1.0, 0.01, 0.0,
         [this](double v, const juce::String& g, bool end)
         {
             const auto* t = ctx.mirror.model().findTrack(ctx.view.selectedTrackId);
             sendTrack("TRACK_SET_SENDS", {{"delaySend", v}, {"reverbSend", t ? num(t->raw, "sendReverb", 0.0) : 0.0}}, g, end);
         },
         percent);
    auto& punch = module("Punch");
    knob(punch, "punchAmount", "Amount", 0.0, 1.0, 0.01, 0.0,
         [this](double v, const juce::String& g, bool end) { sendTrack("TRACK_SET_PUNCH", {{"amount", v}}, g, end); }, percent);
    auto& saturation = module("Saturation");
    knob(saturation, "saturationDrive", "Drive", 0.0, 1.0, 0.01, 0.0,
         [this](double v, const juce::String& g, bool end) { sendTrack("TRACK_SET_SATURATION", {{"drive", v}}, g, end); }, percent);
    knob(saturation, "saturationMix", "Mix", 0.0, 1.0, 0.01, 1.0,
         [this](double v, const juce::String& g, bool end) { sendTrack("TRACK_SET_SATURATION", {{"mix", v}}, g, end); }, percent);
    auto& crusherModule = module("Bit crusher");
    crusher = &crusherModule;
    crusherRemove.setTooltip("Take the bit crusher off this track");
    crusherRemove.onClick = [this]
    {
        crusherAdded.erase(ctx.view.selectedTrackId);
        sendTrack("TRACK_SET_BIT_CRUSHER", {{"rate", 1.0}, {"bits", 16.0}, {"boost", 0.0}, {"mix", 0.0}}, {}, false);
    };
    crusherModule.addTitleButton(crusherRemove, 22);
    auto& crusher = crusherModule;
    knob(crusher, "bitCrusherRate", "Rate", 0.01, 1.0, 0.01, 1.0,
         [this](double v, const juce::String& g, bool end) { sendTrack("TRACK_SET_BIT_CRUSHER", {{"rate", v}}, g, end); }, percent);
    knob(crusher, "bitCrusherBits", "Bits", 1.0, 16.0, 1.0, 16.0,
         [this](double v, const juce::String& g, bool end) { sendTrack("TRACK_SET_BIT_CRUSHER", {{"bits", v}}, g, end); },
         [](double v) { return juce::String(juce::roundToInt(v)); });
    knob(crusher, "bitCrusherBoost", "Boost", 0.0, 1.0, 0.01, 0.0,
         [this](double v, const juce::String& g, bool end) { sendTrack("TRACK_SET_BIT_CRUSHER", {{"boost", v}}, g, end); }, percent);
    knob(crusher, "bitCrusherMix", "Mix", 0.0, 1.0, 0.01, 0.0,
         [this](double v, const juce::String& g, bool end) { sendTrack("TRACK_SET_BIT_CRUSHER", {{"mix", v}}, g, end); }, percent);

    // Each module's (A): the lanes of its knobs.
    for (auto& m : modules)
    {
        juce::StringArray lanes;
        for (const auto& [key, knob] : knobs)
            if (knob->getParentComponent() == m.get())
                if (const auto it = knobLanes().find(key); it != knobLanes().end()) lanes.add(it->second);
        auto* raw = m.get();
        m->setAutomation(lanes, [this, raw] { toggleLanes(*raw); });
    }

    ctx.mirror.addChangeListener(this);
    ctx.view.addChangeListener(this);
    sync();
}

TrackFxView::~TrackFxView()
{
    ctx.mirror.removeChangeListener(this);
    ctx.view.removeChangeListener(this);
}

void TrackFxView::sendTrack(const juce::String& type, std::initializer_list<std::pair<const char*, juce::var>> props,
                            const juce::String& gestureId, bool gestureEnd)
{
    if (ctx.mirror.model().findTrack(ctx.view.selectedTrackId) == nullptr) return;
    auto payload = object(props);
    payload.getDynamicObject()->setProperty("trackId", ctx.view.selectedTrackId);
    if (gestureId.isNotEmpty()) payload.getDynamicObject()->setProperty("gestureId", gestureId);
    if (gestureEnd) payload.getDynamicObject()->setProperty("gestureEnd", true);
    ctx.engine.send(type, payload);
}

void TrackFxView::changeListenerCallback(juce::ChangeBroadcaster*)
{
    sync();
}

void TrackFxView::sync()
{
    const auto* track = ctx.mirror.model().findTrack(ctx.view.selectedTrackId);
    heading.setText(track != nullptr ? "Track FX - " + track->name : juce::String("Select a track to edit its effects."),
                    juce::dontSendNotification);
    content.setEnabled(track != nullptr);
    addEffect.setEnabled(track != nullptr);
    syncEffects(track != nullptr ? track->raw.getProperty("plugins", {}) : juce::var());
    if (track == nullptr) return;
    static const std::map<juce::String, double> defaults{{"saturationMix", 1.0}, {"bitCrusherRate", 1.0}, {"bitCrusherBits", 16.0}};
    for (auto& [key, knob] : knobs)
    {
        const auto it = defaults.find(key);
        knob->setValue(num(track->raw, key.toRawUTF8(), it != defaults.end() ? it->second : 0.0));
    }
    // Which modules' lanes the timeline shows.
    const auto shown = automation::lanesOf(*track);
    const auto showing = [&](const FxModule& m)
    {
        if (m.automationLanes().isEmpty()) return false;
        for (const auto& id : m.automationLanes())
            if (std::none_of(shown.begin(), shown.end(), [&](const automation::Lane& l) { return l.paramId == id; })) return false;
        return true;
    };
    for (auto& m : modules) m->setAutomationShown(showing(*m));
    for (auto& e : effects) e.module->setAutomationShown(showing(*e.module));
    const bool crusherShown = crusherInUse(*track) || crusherAdded.count(track->id) != 0;
    if (crusher != nullptr && crusher->isVisible() != crusherShown)
    {
        crusher->setVisible(crusherShown);
        relayout();
    }
}

bool TrackFxView::crusherInUse(const TrackModel& track)
{
    return num(track.raw, "bitCrusherMix", 0.0) > 0.0 || num(track.raw, "bitCrusherBits", 16.0) < 16.0 ||
           num(track.raw, "bitCrusherRate", 1.0) < 1.0 || num(track.raw, "bitCrusherBoost", 0.0) > 0.0 ||
           !automation::pointsOf(track, "bitCrusherMix").empty();
}

FxModule* TrackFxView::effectModuleForTest(const juce::String& slotId)
{
    for (auto& e : effects)
        if (e.slotId == slotId) return e.module.get();
    return nullptr;
}

void TrackFxView::toggleLanes(FxModule& module)
{
    const auto* track = ctx.mirror.model().findTrack(ctx.view.selectedTrackId);
    if (track == nullptr || module.automationLanes().isEmpty()) return;
    auto lanes = automation::lanesOf(*track);
    const auto has = [&](const juce::String& id)
    { return std::any_of(lanes.begin(), lanes.end(), [&](const automation::Lane& l) { return l.paramId == id; }); };
    bool all = true;
    for (const auto& id : module.automationLanes()) all = all && has(id);
    if (all)
        lanes.erase(std::remove_if(lanes.begin(), lanes.end(), [&](const automation::Lane& l) { return module.automationLanes().contains(l.paramId); }),
                    lanes.end());
    else
        for (const auto& id : module.automationLanes())
            if (!has(id)) lanes.push_back({id, automation::kDefaultLaneHeight});
    ctx.actions.setAutomationLanes(track->id, lanes);
}

void TrackFxView::showAddMenu()
{
    if (ctx.mirror.model().findTrack(ctx.view.selectedTrackId) == nullptr) return;
    juce::PopupMenu beat, color, plugins;
    std::map<juce::String, juce::PopupMenu> byMaker;
    for (const auto& d : ctx.engine.host().engine().pluginCatalogue().getAvailablePlugins())
    {
        if (d.isInstrument) continue;
        const auto identifier = d.fileOrIdentifier;
        auto add = [this, identifier]
        { ctx.engine.send("TRACK_ADD_PLUGIN", object({{"trackId", ctx.view.selectedTrackId}, {"identifier", identifier}})); };
        if (d.pluginFormatName == "Stemcel") (d.category == "Beat FX" ? beat : color).addItem(d.name, add);
        else byMaker[d.manufacturerName.isNotEmpty() ? d.manufacturerName : "Other"].addItem(d.name + "  (" + d.pluginFormatName + ")", add);
    }
    for (auto& [maker, sub] : byMaker) plugins.addSubMenu(maker, sub);
    juce::PopupMenu menu;
    // The bit crusher (built in, off the bar until added): an audible start, 8 bits at half rate.
    const auto trackId = ctx.view.selectedTrackId;
    menu.addItem("Bit crusher", crusher != nullptr && !crusher->isVisible(),
                 false,
                 [this, trackId]
                 {
                     crusherAdded.insert(trackId);
                     sendTrack("TRACK_SET_BIT_CRUSHER", {{"rate", 0.5}, {"bits", 8.0}, {"mix", 1.0}}, {}, false);
                     sync();
                 });
    menu.addSubMenu("Beat FX", beat);
    menu.addSubMenu("Sound Color FX", color);
    menu.addSeparator();
    if (byMaker.empty()) menu.addItem("No plugins found yet (Plugins > Scan for plugins)", false, false, [] {});
    else menu.addSubMenu("Plugins", plugins);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&addEffect));
}

void TrackFxView::syncEffects(const juce::var& slots)
{
    // Rebuilt only when the chain itself changes (added, removed, moved).
    juce::String key = ctx.view.selectedTrackId;
    if (const auto* list = slots.getArray())
        for (const auto& s : *list) key << "|" << s.getProperty("slotId", "").toString();
    auto& engine = ctx.engine.host().engine();
    const auto trackId = ctx.view.selectedTrackId;
    if (key != effectsKey)
    {
        effectsKey = key;
        for (auto& e : effects) content.removeChildComponent(e.module.get());
        effects.clear();
        if (const auto* list = slots.getArray())
            for (const auto& s : *list)
            {
                Effect e;
                e.slotId = s.getProperty("slotId", "").toString();
                e.identifier = s.getProperty("identifier", "").toString();
                e.builtIn = s.getProperty("format", "").toString() == "Stemcel";
                e.beat = e.identifier.startsWith("djfx:beat:");
                e.module = std::make_unique<FxModule>(s.getProperty("name", "Effect").toString());
                auto& m = *e.module;
                const auto slotId = e.slotId;
                // Mix: the effect against the track's own sound (a plugin's latency allowed for).
                e.mix = &m.add(std::make_unique<FxKnob>(
                    "Mix", 0.0, 1.0, 0.01, 1.0,
                    [this, slotId](double v, const juce::String& g, bool end)
                    {
                        auto p = object({{"trackId", ctx.view.selectedTrackId}, {"slotId", slotId}, {"mix", v}});
                        if (g.isNotEmpty()) p.getDynamicObject()->setProperty("gestureId", g);
                        if (end) p.getDynamicObject()->setProperty("gestureEnd", true);
                        ctx.engine.send("TRACK_SET_PLUGIN_MIX", p);
                    },
                    percent));
                juce::StringArray lanes;
                if (e.builtIn)
                {
                    // Strength (and for Beat FX its beats and on/off): the effect's own parameters.
                    const auto setParam = [this, slotId](int index, double normalised)
                    { ctx.engine.host().engine().setTrackPluginParameterValue(ctx.view.selectedTrackId, slotId, index, static_cast<float>(normalised)); };
                    if (e.beat)
                    {
                        e.strength = &m.add(std::make_unique<FxKnob>("Strength", 0.0, 1.0, 0.01, 0.5,
                                                                      [setParam](double v, const juce::String&, bool) { setParam(0, v); }, percent));
                        const int last = static_cast<int>(djfx::kBeatSteps.size()) - 1;
                        e.beats = &m.add(std::make_unique<FxKnob>("Beats", 0.0, last, 1.0, djfx::kDefaultBeatStep,
                                                                   [setParam, last](double v, const juce::String&, bool) { setParam(1, v / last); },
                                                                   [](double v) { return djfx::beatStepLabel(juce::roundToInt(v)); }));
                        e.on = std::make_unique<juce::TextButton>("On");
                        e.on->setClickingTogglesState(true);
                        e.on->onClick = [setParam, b = e.on.get()] { setParam(2, b->getToggleState() ? 1.0 : 0.0); };
                        m.addExtra(*e.on, 44);
                        lanes.add(automation::pluginParamId(slotId, 2));
                    }
                    else
                        e.strength = &m.add(std::make_unique<FxKnob>("Strength", -1.0, 1.0, 0.01, 0.5,
                                                                      [setParam](double v, const juce::String&, bool) { setParam(0, (v + 1.0) * 0.5); },
                                                                      [](double v)
                                                                      {
                                                                          return std::abs(v) < 0.01 ? juce::String("Off")
                                                                                                    : (v > 0 ? "+" : "-") + percent(std::abs(v));
                                                                      }));
                    lanes.insert(0, automation::pluginParamId(slotId, 0));
                }
                else
                {
                    // A plugin: its mix here, everything else in its own window.
                    e.edit = std::make_unique<juce::TextButton>("Edit");
                    e.edit->setTooltip("Open the plugin's own window");
                    e.edit->onClick = [this, slotId]
                    { ctx.engine.send("TRACK_OPEN_PLUGIN_EDITOR", object({{"trackId", ctx.view.selectedTrackId}, {"slotId", slotId}})); };
                    m.addExtra(*e.edit, 52);
                }
                lanes.add("plugin:" + slotId + ":mix");
                // Names for the lanes' captions.
                const auto name = s.getProperty("name", "Effect").toString();
                automation::namePluginParam("plugin:" + slotId + ":mix", name + ": Mix");
                if (e.builtIn)
                {
                    automation::namePluginParam(automation::pluginParamId(slotId, 0), name + ": Strength");
                    if (e.beat) automation::namePluginParam(automation::pluginParamId(slotId, 2), name + ": On");
                }
                e.bypass = std::make_unique<juce::TextButton>("Off");
                e.bypass->setClickingTogglesState(true);
                e.bypass->setTooltip("Bypass");
                e.bypass->onClick = [this, slotId, b = e.bypass.get()]
                {
                    ctx.engine.send("TRACK_SET_PLUGIN_BYPASS",
                                    object({{"trackId", ctx.view.selectedTrackId}, {"slotId", slotId}, {"bypassed", b->getToggleState()}}));
                };
                e.remove = std::make_unique<juce::TextButton>("x");
                e.remove->setTooltip("Remove from the track");
                e.remove->onClick = [this, slotId]
                { ctx.engine.send("TRACK_REMOVE_PLUGIN", object({{"trackId", ctx.view.selectedTrackId}, {"slotId", slotId}})); };
                m.addTitleButton(*e.remove, 20);
                m.addTitleButton(*e.bypass, 32);
                auto* raw = e.module.get();
                m.setAutomation(lanes, [this, raw] { toggleLanes(*raw); });
                content.addAndMakeVisible(m);
                effects.push_back(std::move(e));
            }
        relayout();
    }
    // Their values.
    if (const auto* list = slots.getArray())
        for (const auto& s : *list)
            for (auto& e : effects)
            {
                if (e.slotId != s.getProperty("slotId", "").toString()) continue;
                e.mix->setValue(static_cast<double>(s.getProperty("mix", 1.0)));
                const bool bypassed = static_cast<bool>(s.getProperty("bypassed", false));
                e.bypass->setToggleState(bypassed, juce::dontSendNotification);
                e.module->setAlpha(bypassed ? 0.6F : 1.0F);
                if (e.strength != nullptr)
                    if (const auto v = engine.getTrackPluginParameterValue(trackId, e.slotId, 0))
                        e.strength->setValue(e.beat ? *v : *v * 2.0 - 1.0);
                if (e.beats != nullptr)
                    if (const auto v = engine.getTrackPluginParameterValue(trackId, e.slotId, 1))
                        e.beats->setValue(juce::roundToInt(*v * static_cast<float>(djfx::kBeatSteps.size() - 1)));
                if (e.on != nullptr)
                    if (const auto v = engine.getTrackPluginParameterValue(trackId, e.slotId, 2))
                        e.on->setToggleState(*v >= 0.5F, juce::dontSendNotification);
            }
}

void TrackFxView::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
}

void TrackFxView::resized()
{
    auto area = getLocalBounds();
    auto top = area.removeFromTop(24);
    addEffect.setBounds(top.removeFromRight(110).reduced(0, 1));
    heading.setBounds(top);
    viewport.setBounds(area);
    relayout();
}

void TrackFxView::relayout()
{
    // The built-in modules, then the track's effects in chain order.
    std::vector<std::unique_ptr<FxModule>> all;
    const int width = juce::jmax(100, viewport.getWidth() - viewport.getScrollBarThickness());
    auto place = [&](std::vector<FxModule*> list)
    {
        int x = 0, y = 0;
        for (auto* m : list)
        {
            const int w = m->preferredWidth();
            if (x + w > width && x > 0)
            {
                x = 0;
                y += kModuleHeight + 8;
            }
            m->setBounds(x, y, w, kModuleHeight);
            x += w + 8;
        }
        return y + kModuleHeight + 8;
    };
    std::vector<FxModule*> list;
    for (auto& m : modules)
        if (m->isVisible()) list.push_back(m.get());
    for (auto& e : effects) list.push_back(e.module.get());
    content.setSize(width, place(list));
}

// ── Project FX ─────────────────────────────────────────────────────────────
ProjectFxView::ProjectFxView(LibraryContext& context) : ctx(context)
{
    auto module = [this](const juce::String& title) -> FxModule&
    {
        modules.push_back(std::make_unique<FxModule>(title));
        addAndMakeVisible(*modules.back());
        return *modules.back();
    };
    auto send = [this](const juce::String& type, const char* field)
    {
        return [this, type, field = juce::String(field)](double v, const juce::String& g, bool end)
        {
            auto p = object({{field.toRawUTF8(), v}});
            if (g.isNotEmpty()) p.getDynamicObject()->setProperty("gestureId", g);
            if (end) p.getDynamicObject()->setProperty("gestureEnd", true);
            ctx.engine.send(type, p);
        };
    };
    auto& room = module("Room");
    for (const auto& [key, label, field] : std::vector<std::tuple<juce::String, juce::String, const char*>>{
             {"reverbSize", "Size", "size"}, {"reverbDecay", "Decay", "decay"}, {"reverbTone", "Tone", "tone"}, {"reverbMix", "Mix", "mix"}})
        knobs[key] = &room.add(std::make_unique<FxKnob>(label, 0.0, 1.0, 0.01, 0.0, send("PROJECT_SET_REVERB", field), percent));
    auto& echo = module("Echo");
    for (const auto& [key, label, field] : std::vector<std::tuple<juce::String, juce::String, const char*>>{
             {"delayFeedback", "Feedback", "feedback"}, {"delayTone", "Tone", "tone"}, {"delayMix", "Mix", "mix"}})
        knobs[key] = &echo.add(std::make_unique<FxKnob>(label, 0.0, 1.0, 0.01, 0.0, send("PROJECT_SET_DELAY", field), percent));
    for (const auto& note : kNoteValues)
    {
        auto* b = noteButtons.add(new juce::TextButton(note));
        b->setTooltip("Delay time: " + note + " note");
        b->onClick = [this, note] { ctx.engine.send("PROJECT_SET_DELAY", object({{"noteValue", note}})); };
    }
    // The note buttons stack in a small column beside the echo knobs.
    struct Column final : public juce::Component
    {
        void resized() override
        {
            int y = 0;
            for (auto* c : getChildren())
            {
                c->setBounds(0, y, getWidth(), 19);
                y += 20;
            }
        }
    };
    auto* column = new Column();
    for (auto* b : noteButtons) column->addAndMakeVisible(b);
    echo.addExtra(*column, 52);
    columnOwner.reset(column);
    auto& glue = module("Glue");
    knobs["mixGlueAmount"] = &glue.add(std::make_unique<FxKnob>("Amount", 0.0, 1.0, 0.01, 0.0, send("PROJECT_SET_MIX_GLUE", "amount"), percent));
    // Glue makes up what it takes off on its own; Makeup adds more.
    knobs["mixGlueMakeupDb"] = &glue.add(std::make_unique<FxKnob>("Makeup", 0.0, 12.0, 0.1, 0.0, send("PROJECT_SET_MIX_GLUE", "makeupDb"),
                                                                  [](double v) { return "+" + juce::String(v, 1) + " dB"; }));
    auto& master = module("Master");
    limiter.onClick = [this] { ctx.engine.send("PROJECT_SET_SAFETY_LIMITER", object({{"enabled", limiter.getToggleState()}})); };
    metronome.onClick = [this]
    {
        ctx.engine.send("PROJECT_SET_METRONOME", object({{"enabled", metronome.getToggleState()}}));
        // Not an edit (no undo, no dirty): a clean project writes it to its file straight away.
        const auto& m = ctx.mirror.model();
        if (!m.dirty && m.filePath.isNotEmpty())
            ctx.engine.send("PROJECT_SAVE_VIEW_STATE", object({{"filePath", m.filePath}}));
    };
    master.addExtra(limiter, 130);
    master.addExtra(metronome, 110);

    ctx.mirror.addChangeListener(this);
    sync();
}

ProjectFxView::~ProjectFxView()
{
    ctx.mirror.removeChangeListener(this);
}

void ProjectFxView::changeListenerCallback(juce::ChangeBroadcaster*)
{
    sync();
}

void ProjectFxView::sync()
{
    const auto& raw = ctx.mirror.model().raw;
    for (auto& [key, knob] : knobs) knob->setValue(num(raw, key.toRawUTF8(), 0.0));
    const auto note = raw.getProperty("delayNoteValue", "1/8").toString();
    for (auto* b : noteButtons)
        b->setColour(juce::TextButton::buttonColourId, b->getButtonText() == note ? colours::sky600 : colours::zinc700);
    limiter.setToggleState(raw.getProperty("safetyLimiterEnabled", true).isVoid()
                               || static_cast<bool>(raw.getProperty("safetyLimiterEnabled", true)),
                           juce::dontSendNotification);
    metronome.setToggleState(static_cast<bool>(raw.getProperty("metronomeEnabled", false)), juce::dontSendNotification);
}

void ProjectFxView::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
}

void ProjectFxView::resized()
{
    layoutModules(modules, getLocalBounds().reduced(0, 4));
}

// ── Plugins ────────────────────────────────────────────────────────────────
PluginsView::PluginsView(LibraryContext& context) : ctx(context), subscriptions(context.engine)
{
    heading.setFont(uiFont(13.0f, true));
    heading.setColour(juce::Label::textColourId, colours::text);
    addAndMakeVisible(heading);
    chain.setModel(this);
    chain.setRowHeight(28);
    chain.setColour(juce::ListBox::backgroundColourId, colours::zinc950);
    addAndMakeVisible(chain);
    addButton.onClick = [this] { showAddMenu(); };
    scanButton.onClick = [this] { ctx.engine.send("PLUGIN_SCAN", object({{"clearBlacklist", false}})); };
    addAndMakeVisible(addButton);
    addAndMakeVisible(scanButton);
    masterTarget.onClick = [this] { sync(); };
    addAndMakeVisible(masterTarget);
    scanStatus.setColour(juce::Label::textColourId, colours::textFaint);
    scanStatus.setFont(uiFont(12.0f));
    addAndMakeVisible(scanStatus);

    subscriptions.on("PLUGIN_LIST",
                     [this](const juce::var& p)
                     {
                         catalogue = p.getProperty("plugins", {});
                         const int count = catalogue.isArray() ? catalogue.size() : 0;
                         scanStatus.setText(static_cast<bool>(p.getProperty("scanning", false))
                                                ? juce::String("Scanning...")
                                                : juce::String(count) + " plugins available",
                                            juce::dontSendNotification);
                     });
    subscriptions.on("PLUGIN_SCAN_PROGRESS",
                     [this](const juce::var& p)
                     {
                         if (static_cast<bool>(p.getProperty("finished", false)))
                         {
                             ctx.engine.send("PLUGIN_LIST_REQUEST");
                             return;
                         }
                         scanStatus.setText("Scanning " + juce::String(static_cast<int>(p.getProperty("scanned", 0))) + "/"
                                                + juce::String(static_cast<int>(p.getProperty("total", 0))) + ": "
                                                + juce::File(p.getProperty("currentFile", {}).toString()).getFileName(),
                                            juce::dontSendNotification);
                     });
    subscriptions.on("PLUGIN_NOTICE",
                     [this](const juce::var& p)
                     {
                         if (ctx.notify) ctx.notify(p.getProperty("message", p.getProperty("error", {})).toString(), true);
                     });
    if (const auto last = ctx.engine.latest("PLUGIN_LIST"); !last.isVoid()) catalogue = last.getProperty("plugins", {});
    ctx.engine.send("PLUGIN_LIST_REQUEST");
    ctx.mirror.addChangeListener(this);
    ctx.view.addChangeListener(this);
    sync();
}

PluginsView::~PluginsView()
{
    ctx.mirror.removeChangeListener(this);
    ctx.view.removeChangeListener(this);
}

void PluginsView::changeListenerCallback(juce::ChangeBroadcaster*)
{
    sync();
}

juce::String PluginsView::targetId() const
{
    return masterTarget.getToggleState() ? juce::String("__master__") : ctx.view.selectedTrackId; // plugins::kMasterChainId
}

void PluginsView::sync()
{
    slots.clear();
    const bool master = masterTarget.getToggleState();
    const auto* track = master ? nullptr : ctx.mirror.model().findTrack(ctx.view.selectedTrackId);
    heading.setText(master ? juce::String("Plugins - master bus")
                           : track != nullptr ? "Plugins - " + track->name : juce::String("Select a track to add plugins to it."),
                    juce::dontSendNotification);
    addButton.setEnabled(master || track != nullptr);
    const auto& owner = master ? ctx.mirror.model().raw : track != nullptr ? track->raw : juce::var();
    if (master || track != nullptr)
        if (const auto* list = owner.getProperty(master ? "masterPlugins" : "plugins", {}).getArray())
            for (const auto& s : *list)
                slots.push_back({s.getProperty("slotId", {}).toString(), s.getProperty("name", {}).toString(),
                                 s.getProperty("manufacturer", {}).toString(),
                                 static_cast<bool>(s.getProperty("bypassed", false)),
                                 static_cast<bool>(s.getProperty("unresolved", false))});
    chain.updateContent();
    chain.repaint();
}

juce::var PluginsView::slotRef(const Slot& slot) const
{
    return object({{"trackId", targetId()}, {"slotId", slot.slotId}});
}

int PluginsView::getNumRows()
{
    return static_cast<int>(slots.size());
}

void PluginsView::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= static_cast<int>(slots.size())) return;
    const auto& s = slots[static_cast<size_t>(row)];
    if (selected) g.fillAll(colours::sky600.withAlpha(0.35f));
    g.setColour(s.unresolved ? colours::red500 : s.bypassed ? colours::textFaint : colours::text);
    g.setFont(uiFont(13.0f, true));
    g.drawText(juce::String(row + 1) + ".  " + s.name, 8, 0, width / 2, height, juce::Justification::centredLeft, true);
    g.setColour(colours::textDim);
    g.setFont(uiFont(12.0f));
    g.drawText(s.unresolved ? juce::String("not installed") : s.bypassed ? "bypassed - " + s.manufacturer : s.manufacturer,
               width / 2, 0, width / 2 - 8, height, juce::Justification::centredRight, true);
}

void PluginsView::listBoxItemClicked(int row, const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) slotMenu(row);
}

void PluginsView::listBoxItemDoubleClicked(int row, const juce::MouseEvent&)
{
    if (row >= 0 && row < static_cast<int>(slots.size()))
        ctx.engine.send("TRACK_OPEN_PLUGIN_EDITOR", slotRef(slots[static_cast<size_t>(row)]));
}

void PluginsView::slotMenu(int row)
{
    if (row < 0 || row >= static_cast<int>(slots.size())) return;
    const auto s = slots[static_cast<size_t>(row)];
    juce::PopupMenu menu;
    menu.addItem("Open editor", !s.unresolved, false, [this, s] { ctx.engine.send("TRACK_OPEN_PLUGIN_EDITOR", slotRef(s)); });
    menu.addItem(s.bypassed ? "Enable" : "Bypass", [this, s]
                 {
                     auto p = slotRef(s);
                     p.getDynamicObject()->setProperty("bypassed", !s.bypassed);
                     ctx.engine.send("TRACK_SET_PLUGIN_BYPASS", p);
                 });
    menu.addItem("Move up", row > 0, false, [this, s, row]
                 {
                     auto p = slotRef(s);
                     p.getDynamicObject()->setProperty("index", row - 1);
                     ctx.engine.send("TRACK_REORDER_PLUGIN", p);
                 });
    menu.addItem("Move down", row + 1 < static_cast<int>(slots.size()), false, [this, s, row]
                 {
                     auto p = slotRef(s);
                     p.getDynamicObject()->setProperty("index", row + 1);
                     ctx.engine.send("TRACK_REORDER_PLUGIN", p);
                 });
    menu.addSeparator();
    menu.addItem("Remove", [this, s] { ctx.engine.send("TRACK_REMOVE_PLUGIN", slotRef(s)); });
    menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
}

void PluginsView::showAddMenu()
{
    if (!catalogue.isArray() || catalogue.size() == 0)
    {
        if (ctx.notify) ctx.notify("No plugins found yet - use Scan for plugins.", false);
        return;
    }
    // Grouped by manufacturer; effects first (instruments are listed but rarely useful here).
    std::map<juce::String, juce::PopupMenu> byMaker;
    for (const auto& p : *catalogue.getArray())
    {
        const auto identifier = p.getProperty("identifier", {}).toString();
        const auto name = p.getProperty("name", {}).toString() + "  (" + p.getProperty("format", {}).toString() + ")";
        auto maker = p.getProperty("manufacturer", {}).toString();
        if (maker.isEmpty()) maker = "Other";
        byMaker[maker].addItem(name, [this, identifier]
                               {
                                   ctx.engine.send("TRACK_ADD_PLUGIN",
                                                   object({{"trackId", targetId()}, {"identifier", identifier}}));
                               });
    }
    juce::PopupMenu menu;
    for (auto& [maker, sub] : byMaker) menu.addSubMenu(maker, sub);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&addButton));
}

void PluginsView::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
}

void PluginsView::resized()
{
    auto area = getLocalBounds();
    auto top = area.removeFromTop(28);
    addButton.setBounds(top.removeFromRight(110).reduced(0, 2));
    top.removeFromRight(6);
    scanButton.setBounds(top.removeFromRight(130).reduced(0, 2));
    top.removeFromRight(8);
    scanStatus.setBounds(top.removeFromRight(260));
    masterTarget.setBounds(top.removeFromRight(110));
    heading.setBounds(top);
    chain.setBounds(area.reduced(0, 4));
}

} // namespace stemcel::native
