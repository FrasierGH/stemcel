#include "FxPanels.h"

#include "Theme.h"

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
FxModule::FxModule(const juce::String& moduleTitle) : title(moduleTitle) {}

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
    g.drawText(title, 8, 4, getWidth() - 16, 16, juce::Justification::centredLeft);
}

void FxModule::resized()
{
    auto area = getLocalBounds().reduced(8, 4);
    area.removeFromTop(18);
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

    auto& tone = module("Tone");
    for (const auto& [key, label, field] : std::vector<std::tuple<juce::String, juce::String, const char*>>{
             {"toneBassDb", "Bass", "bassDb"}, {"toneMidDb", "Mid", "midDb"}, {"toneTrebleDb", "Treble", "trebleDb"}})
        knob(tone, key, label, -15.0, 15.0, 0.5, 0.0,
             [this, field = juce::String(field)](double v, const juce::String& g, bool end)
             { sendTrack("TRACK_SET_TONE", {{field.toRawUTF8(), v}}, g, end); },
             db);
    auto& filter = module("Filter");
    knob(filter, "toneFilter", "LP / HP", -1.0, 1.0, 0.01, 0.0,
         [this](double v, const juce::String& g, bool end) { sendTrack("TRACK_SET_TONE", {{"filter", v}}, g, end); },
         [](double v) { return std::abs(v) < 0.005 ? juce::String("Off") : (v < 0 ? "LP " : "HP ") + percent(std::abs(v)); });
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
    auto& leveler = module("Leveler");
    knob(leveler, "levelerAmount", "Amount", 0.0, 1.0, 0.01, 0.0,
         [this](double v, const juce::String& g, bool end) { sendTrack("TRACK_SET_LEVELER", {{"amount", v}}, g, end); }, percent);
    auto& punch = module("Punch");
    knob(punch, "punchAmount", "Amount", 0.0, 1.0, 0.01, 0.0,
         [this](double v, const juce::String& g, bool end) { sendTrack("TRACK_SET_PUNCH", {{"amount", v}}, g, end); }, percent);
    auto& saturation = module("Saturation");
    knob(saturation, "saturationDrive", "Drive", 0.0, 1.0, 0.01, 0.0,
         [this](double v, const juce::String& g, bool end) { sendTrack("TRACK_SET_SATURATION", {{"drive", v}}, g, end); }, percent);
    knob(saturation, "saturationMix", "Mix", 0.0, 1.0, 0.01, 1.0,
         [this](double v, const juce::String& g, bool end) { sendTrack("TRACK_SET_SATURATION", {{"mix", v}}, g, end); }, percent);
    auto& crusher = module("Bit crusher");
    knob(crusher, "bitCrusherRate", "Rate", 0.01, 1.0, 0.01, 1.0,
         [this](double v, const juce::String& g, bool end) { sendTrack("TRACK_SET_BIT_CRUSHER", {{"rate", v}}, g, end); }, percent);
    knob(crusher, "bitCrusherBits", "Bits", 1.0, 16.0, 1.0, 16.0,
         [this](double v, const juce::String& g, bool end) { sendTrack("TRACK_SET_BIT_CRUSHER", {{"bits", v}}, g, end); },
         [](double v) { return juce::String(juce::roundToInt(v)); });
    knob(crusher, "bitCrusherBoost", "Boost", 0.0, 1.0, 0.01, 0.0,
         [this](double v, const juce::String& g, bool end) { sendTrack("TRACK_SET_BIT_CRUSHER", {{"boost", v}}, g, end); }, percent);
    knob(crusher, "bitCrusherMix", "Mix", 0.0, 1.0, 0.01, 0.0,
         [this](double v, const juce::String& g, bool end) { sendTrack("TRACK_SET_BIT_CRUSHER", {{"mix", v}}, g, end); }, percent);

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
    if (track == nullptr) return;
    static const std::map<juce::String, double> defaults{{"saturationMix", 1.0}, {"bitCrusherRate", 1.0}, {"bitCrusherBits", 16.0}};
    for (auto& [key, knob] : knobs)
    {
        const auto it = defaults.find(key);
        knob->setValue(num(track->raw, key.toRawUTF8(), it != defaults.end() ? it->second : 0.0));
    }
}

void TrackFxView::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
}

void TrackFxView::resized()
{
    auto area = getLocalBounds();
    heading.setBounds(area.removeFromTop(24));
    viewport.setBounds(area);
    const int width = area.getWidth() - viewport.getScrollBarThickness();
    content.setSize(width, modulesHeight(modules, width));
    layoutModules(modules, content.getLocalBounds());
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
