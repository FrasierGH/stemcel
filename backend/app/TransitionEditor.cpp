#include "TransitionEditor.h"

#include "Theme.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace stemcel::native
{
namespace
{
const juce::StringArray kCurveIds{"equalPower", "linear", "fast", "slow", "sCurve"};
const juce::StringArray kCurveNames{"Equal power", "Linear", "Fast start", "Slow start", "S-curve"};
const juce::StringArray kLevelIds{"fade", "edge", "cut", "none"};
const juce::StringArray kFilterIds{"none", "lowPass", "highPass"};
const juce::StringArray kFilterNames{"No filter", "Low-pass sweep", "High-pass sweep"};
const juce::StringArray kBandIds{"none", "swap", "sweep"};
const juce::StringArray kBandNames{"Untouched", "Swap at the swap point", "Sweep over the span"};
const juce::StringArray kEffectIds{"none", "echo", "reverb", "crush"};
const juce::StringArray kEffectNames{"No effect", "Echo throw", "Reverb wash", "Bitcrush"};
const juce::StringArray kExtraIds{"none", "brake", "backspin", "loopRoll"};
const juce::StringArray kExtraNames{"Nothing at the end", "Vinyl brake", "Backspin", "Loop roll"};

void fill(juce::ComboBox& box, const juce::StringArray& names)
{
    box.clear(juce::dontSendNotification);
    for (int i = 0; i < names.size(); ++i) box.addItem(names[i], i + 1);
}

void select(juce::ComboBox& box, const juce::StringArray& ids, const juce::String& id)
{
    box.setSelectedItemIndex(juce::jmax(0, ids.indexOf(id)), juce::dontSendNotification);
}

juce::String chosen(const juce::ComboBox& box, const juce::StringArray& ids)
{
    return ids[juce::jmax(0, box.getSelectedItemIndex())];
}

mix::Extra extraOf(const juce::String& id)
{
    if (id == "brake") return mix::Extra::brake;
    if (id == "backspin") return mix::Extra::backspin;
    if (id == "loopRoll") return mix::Extra::loopRoll;
    return mix::Extra::none;
}

juce::String extraId(mix::Extra e)
{
    switch (e)
    {
        case mix::Extra::brake: return "brake";
        case mix::Extra::backspin: return "backspin";
        case mix::Extra::loopRoll: return "loopRoll";
        case mix::Extra::tempoRamp:
        case mix::Extra::none: break;
    }
    return "none";
}

// A shape's value as 0 (gone) .. 1 (fully there) for the graph.
double presence(const juce::String& param, double value)
{
    if (param == "level") return value <= -59.0 ? 0.0 : std::pow(10.0, value / 20.0);
    if (param.startsWith("tone")) return 1.0 - juce::jlimit(0.0, 1.0, value / -40.0);
    if (param == "filter") return 1.0 - std::abs(value);
    if (param == "bitCrusherRate") return value;
    return 1.0 - value; // sends and crush mix: how much of the dry track is left
}

// And back: the value a point dragged to `p` (0..1) on the graph stands for. A filter keeps
// the direction its half sweeps in (low-pass is negative).
double fromPresence(const juce::String& param, double p, bool highPass)
{
    p = juce::jlimit(0.0, 1.0, p);
    if (param == "level") return p <= 0.001 ? -60.0 : juce::jmax(-60.0, 20.0 * std::log10(p));
    if (param.startsWith("tone")) return -40.0 * (1.0 - p);
    if (param == "filter") return (highPass ? 1.0 : -1.0) * (1.0 - p);
    if (param == "bitCrusherRate") return juce::jmax(0.01, p);
    return 1.0 - p;
}

juce::Colour colourFor(const juce::String& param)
{
    if (param == "level") return colours::zinc100;
    if (param == "toneBass") return colours::red500;
    if (param == "toneMid") return colours::amber400;
    if (param == "toneTreble") return colours::sky400;
    if (param == "filter") return colours::violet500;
    return colours::emerald500;
}

juce::String labelFor(const juce::String& param)
{
    if (param == "level") return "fader";
    if (param == "toneBass") return "low";
    if (param == "toneMid") return "mid";
    if (param == "toneTreble") return "high";
    if (param == "filter") return "filter";
    return "fx";
}
} // namespace

// ── One half ────────────────────────────────────────────────────────────────
class TransitionEditor::HalfEditor final : public juce::Component
{
  public:
    HalfEditor(bool isOut, std::function<double()> barsInWindow) : outgoing(isOut), bars(std::move(barsInWindow))
    {
        title.setText(outgoing ? "OUT: the outgoing track" : "IN: the incoming track", juce::dontSendNotification);
        title.setFont(uiFont(12.0f, true));
        title.setColour(juce::Label::textColourId, outgoing ? colours::amber400 : colours::sky400);
        addAndMakeVisible(title);

        kind.addItem("None (leave this track alone)", 1);
        for (const auto& k : mix::halfKinds())
            if (k.outgoing == outgoing)
            {
                kindIds.add(k.id);
                kind.addItem(k.name, kind.getNumItems() + 1);
            }
        kind.onChange = [this]
        {
            const int index = kind.getSelectedItemIndex() - 1;
            if (index < 0) value.reset();
            else if (const auto* k = mix::findHalf(kindIds[index])) value = k->defaults;
            load();
            notify();
        };
        addAndMakeVisible(kind);

        span.setSliderStyle(juce::Slider::TwoValueHorizontal);
        span.setRange(0.0, 1.0, 0.001);
        span.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        span.onValueChange = [this]
        {
            if (!value.has_value()) return;
            value->from = span.getMinValue();
            value->to = juce::jmax(span.getMinValue(), span.getMaxValue());
            describe();
            notify();
        };
        fill(curve, kCurveNames);
        fill(level, outgoing ? juce::StringArray{"Fade over the span", "Fade at the end of the span", "Cut at the end", "Leave the fader"}
                             : juce::StringArray{"Fade over the span", "Fade at the start of the span", "Cut in at the start", "Leave the fader"});
        fill(filter, kFilterNames);
        for (auto* band : {&low, &mid, &high}) fill(*band, kBandNames);
        fill(effect, kEffectNames);
        fill(extra, kExtraNames);
        for (auto* box : {&curve, &level, &filter, &low, &mid, &high, &effect, &extra}) box->onChange = [this] { read(); };
        depth.setRange(0.0, 1.0, 0.01);
        swap.setRange(0.0, 1.0, 0.001);
        kill.setRange(-40.0, -3.0, 0.5);
        kill.setTextValueSuffix(" dB");
        for (auto* s : {&depth, &swap, &kill})
        {
            s->setSliderStyle(juce::Slider::LinearHorizontal);
            s->setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 20);
            s->onValueChange = [this] { read(); };
        }
        swap.textFromValueFunction = [this](double v) { return "bar " + juce::String(1.0 + v * bars(), 1); };
        depth.textFromValueFunction = [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; };
        for (auto* l : {&spanLabel, &curveLabel, &levelLabel, &filterLabel, &lowLabel, &midLabel, &highLabel, &swapLabel, &killLabel,
                        &effectLabel})
        {
            l->setFont(uiFont(11.0f));
            l->setColour(juce::Label::textColourId, colours::textDim);
            addAndMakeVisible(l);
        }
        curveLabel.setText("Curve", juce::dontSendNotification);
        levelLabel.setText("Fader", juce::dontSendNotification);
        filterLabel.setText("Filter", juce::dontSendNotification);
        lowLabel.setText("Low", juce::dontSendNotification);
        midLabel.setText("Mid", juce::dontSendNotification);
        highLabel.setText("High", juce::dontSendNotification);
        swapLabel.setText("Swap point", juce::dontSendNotification);
        killLabel.setText("Kill depth", juce::dontSendNotification);
        effectLabel.setText(outgoing ? "Effect" : "", juce::dontSendNotification);
        for (juce::Component* c : std::initializer_list<juce::Component*>{&span, &curve, &level, &filter, &depth, &low, &mid, &high, &swap, &kill})
            addAndMakeVisible(c);
        if (outgoing)
        {
            addAndMakeVisible(effect);
            addAndMakeVisible(extra);
        }

        // A + on each option: its curve is drawn by hand on the graph instead.
        for (auto& [button, param] : manualButtons())
        {
            button->setClickingTogglesState(false);
            button->setTooltip("Draw this curve by hand on the graph (the option greys out). Click again to go back to the option.");
            button->onClick = [this, param = param] { toggleManual(param()); };
            if (outgoing || button != &manualFx) addAndMakeVisible(button);
        }
    }

    void set(const std::optional<mix::Half>& h)
    {
        value = h;
        kind.setSelectedItemIndex(h.has_value() ? kindIds.indexOf(h->kind) + 1 : 0, juce::dontSendNotification);
        load();
    }

    const std::optional<mix::Half>& get() const { return value; }
    std::function<void()> onChange;
    // A curve handed to the graph (or picked there): the editor makes it the one being drawn.
    std::function<void(const juce::String& param)> onActivate;

    // The graph's edits to a curve drawn by hand.
    void setManual(const juce::String& param, std::vector<automation::Point> points)
    {
        if (!value.has_value()) return;
        value->manual[param] = std::move(points);
        notify();
    }

    void toggleManual(const juce::String& param)
    {
        if (!value.has_value()) return;
        const bool on = value->manual.count(param) == 0;
        if (on)
        {
            // An effect drawn by hand still needs its send ready: an echo unless one is chosen.
            if (outgoing && (param == "delaySend" || param == "reverbSend" || param == "bitCrusherMix") && value->effect == "none")
            {
                value->effect = param == "reverbSend" ? "reverb" : param == "bitCrusherMix" ? "crush" : "echo";
                select(effect, kEffectIds, value->effect);
            }
            value->manual[param] = mix::manualSeed(*value, outgoing, param);
        }
        else
        {
            value->manual.erase(param);
        }
        refreshManual();
        notify();
        if (on && onActivate) onActivate(param);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        title.setBounds(area.removeFromTop(20));
        kind.setBounds(area.removeFromTop(24));
        area.removeFromTop(6);
        auto row = [&area](juce::Label& label, juce::Component& c, juce::Component* second = nullptr, juce::Component* plus = nullptr)
        {
            auto r = area.removeFromTop(24);
            label.setBounds(r.removeFromLeft(78));
            if (plus != nullptr)
            {
                plus->setBounds(r.removeFromRight(24).reduced(0, 1));
                r.removeFromRight(4);
            }
            if (second != nullptr)
            {
                c.setBounds(r.removeFromLeft(r.getWidth() / 2 - 2));
                r.removeFromLeft(4);
                second->setBounds(r);
            }
            else
                c.setBounds(r);
            area.removeFromTop(3);
        };
        spanLabel.setBounds(area.removeFromTop(16));
        span.setBounds(area.removeFromTop(22));
        area.removeFromTop(3);
        row(curveLabel, curve);
        row(levelLabel, level, nullptr, &manualLevel);
        row(filterLabel, filter, &depth, &manualFilter);
        row(lowLabel, low, nullptr, &manualLow);
        row(midLabel, mid, nullptr, &manualMid);
        row(highLabel, high, nullptr, &manualHigh);
        row(swapLabel, swap);
        row(killLabel, kill);
        if (outgoing) row(effectLabel, effect, &extra, &manualFx);
    }

    void describe()
    {
        if (!value.has_value())
        {
            spanLabel.setText("Span: -", juce::dontSendNotification);
            return;
        }
        const double total = bars();
        spanLabel.setText("Span: bar " + juce::String(1.0 + value->from * total, 1) + " to " + juce::String(1.0 + value->to * total, 1)
                              + " of the " + juce::String(total, 1) + "-bar window",
                          juce::dontSendNotification);
        swap.updateText();
    }

  private:
    std::vector<std::pair<juce::TextButton*, std::function<juce::String()>>> manualButtons()
    {
        return {{&manualLevel, [] { return juce::String("level"); }},
                {&manualFilter, [] { return juce::String("filter"); }},
                {&manualLow, [] { return juce::String("toneBass"); }},
                {&manualMid, [] { return juce::String("toneMid"); }},
                {&manualHigh, [] { return juce::String("toneTreble"); }},
                {&manualFx, [this] { return fxParam(); }}};
    }

    // The effect's own parameter: what its curve drawn by hand moves.
    juce::String fxParam() const
    {
        if (value.has_value())
        {
            if (value->effect == "reverb") return "reverbSend";
            if (value->effect == "crush") return "bitCrusherMix";
            for (const auto* p : {"delaySend", "reverbSend", "bitCrusherMix"})
                if (value->manual.count(p) != 0) return p;
        }
        return "delaySend";
    }

    // The + buttons lit for curves drawn by hand, and their options greyed out.
    void refreshManual()
    {
        const bool on = value.has_value();
        auto manual = [this](const juce::String& p) { return value.has_value() && value->manual.count(p) != 0; };
        for (auto& [button, param] : manualButtons())
        {
            const bool drawn = manual(param());
            button->setButtonText(drawn ? "M" : "+");
            button->setToggleState(drawn, juce::dontSendNotification);
            button->setColour(juce::TextButton::buttonColourId, drawn ? colours::sky600 : colours::zinc900);
            button->setEnabled(on);
        }
        level.setEnabled(on && !manual("level"));
        filter.setEnabled(on && !manual("filter"));
        depth.setEnabled(on && !manual("filter"));
        low.setEnabled(on && !manual("toneBass"));
        mid.setEnabled(on && !manual("toneMid"));
        high.setEnabled(on && !manual("toneTreble"));
        effect.setEnabled(on && !manual(fxParam()));
    }

    void load()
    {
        const bool on = value.has_value();
        for (juce::Component* c : std::initializer_list<juce::Component*>{&span, &curve, &level, &filter, &depth, &low, &mid, &high, &swap, &kill, &effect, &extra})
            c->setEnabled(on);
        if (on)
        {
            span.setMinAndMaxValues(value->from, value->to, juce::dontSendNotification);
            select(curve, kCurveIds, value->curve);
            select(level, kLevelIds, value->level);
            select(filter, kFilterIds, value->filter);
            depth.setValue(value->filterDepth, juce::dontSendNotification);
            select(low, kBandIds, value->low);
            select(mid, kBandIds, value->mid);
            select(high, kBandIds, value->high);
            swap.setValue(value->swapAt, juce::dontSendNotification);
            kill.setValue(value->killDb, juce::dontSendNotification);
            select(effect, kEffectIds, value->effect);
            select(extra, kExtraIds, extraId(value->extra));
        }
        refreshManual();
        describe();
    }

    void read()
    {
        if (!value.has_value()) return;
        value->curve = chosen(curve, kCurveIds);
        value->level = chosen(level, kLevelIds);
        value->filter = chosen(filter, kFilterIds);
        value->filterDepth = depth.getValue();
        value->low = chosen(low, kBandIds);
        value->mid = chosen(mid, kBandIds);
        value->high = chosen(high, kBandIds);
        value->swapAt = swap.getValue();
        value->killDb = kill.getValue();
        if (outgoing)
        {
            value->effect = chosen(effect, kEffectIds);
            value->extra = extraOf(chosen(extra, kExtraIds));
        }
        refreshManual();
        notify();
    }

    void notify()
    {
        if (onChange) onChange();
    }

    bool outgoing;
    std::function<double()> bars;
    std::optional<mix::Half> value;
    juce::StringArray kindIds;
    juce::Label title, spanLabel, curveLabel, levelLabel, filterLabel, lowLabel, midLabel, highLabel, swapLabel, killLabel, effectLabel;
    juce::ComboBox kind, curve, level, filter, low, mid, high, effect, extra;
    juce::Slider span, depth, swap, kill;
    juce::TextButton manualLevel{"+"}, manualFilter{"+"}, manualLow{"+"}, manualMid{"+"}, manualHigh{"+"}, manualFx{"+"};
};

// ── The editor ──────────────────────────────────────────────────────────────
TransitionEditor::TransitionEditor(ProjectMirror& projectMirror, TimelineActions& timelineActions, juce::String outId, juce::String inId)
    : mirror(projectMirror), actions(timelineActions), outClipId(std::move(outId)), inClipId(std::move(inId))
{
    const auto& m = mirror.model();
    const auto* record = mix::recordFor(m, outClipId, inClipId);
    join = record != nullptr ? mix::joinOf(*record) : mix::joinForPreset("smoothBlend");
    auto nameOf = [&m](const juce::String& id)
    {
        const auto* c = m.findClip(id);
        if (c == nullptr) return juce::String("?");
        if (c->name.isNotEmpty()) return c->name;
        const auto* item = m.findLibraryItem(c->libraryItemId);
        return item != nullptr ? item->displayName() : juce::String("clip");
    };
    heading.setText(nameOf(outClipId) + juce::String(juce::CharPointer_UTF8("  \xe2\x86\x92  ")) + nameOf(inClipId), juce::dontSendNotification);
    heading.setFont(uiFont(14.0f, true));
    addAndMakeVisible(heading);
    sub.setFont(uiFont(11.0f));
    sub.setColour(juce::Label::textColourId, colours::textDim);
    sub.setText("Changes apply as you make them (Undo steps back). The window is where the two clips overlap: move or trim them to "
                "lengthen it. + on an option draws its curve by hand on the graph.",
                juce::dontSendNotification);
    addAndMakeVisible(sub);
    hint.setFont(uiFont(10.0f));
    hint.setColour(juce::Label::textColourId, colours::textFaint);
    hint.setJustificationType(juce::Justification::centredRight);
    hint.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(hint);

    outEditor = std::make_unique<HalfEditor>(true, [this] { return windowBars(); });
    inEditor = std::make_unique<HalfEditor>(false, [this] { return windowBars(); });
    outEditor->set(join.out);
    inEditor->set(join.in);
    for (auto* e : {outEditor.get(), inEditor.get()})
    {
        e->onChange = [this] { changed(); };
        e->onActivate = [this, outgoing = e == outEditor.get()](const juce::String& param) { activate(outgoing, param); };
        addAndMakeVisible(e);
    }
    rampTempo.setToggleState(join.rampTempo, juce::dontSendNotification);
    rampTempo.onClick = [this] { changed(); };
    addAndMakeVisible(rampTempo);
    remove.onClick = [this]
    {
        stopTimer();
        mirror.refreshNow();
        if (const auto* r = mix::recordFor(mirror.model(), outClipId, inClipId))
            mix::remove(actions.engineClient(), actions, mirror.model(), r->getProperty("id", {}).toString());
        if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState(0);
    };
    close.onClick = [this]
    {
        if (isTimerRunning()) timerCallback();
        if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState(0);
    };
    addAndMakeVisible(remove);
    addAndMakeVisible(close);
    // The first curve drawn by hand, if the transition has one, is the one being edited.
    for (const bool outgoing : {true, false})
        if (const auto& half = outgoing ? join.out : join.in; half.has_value() && !half->manual.empty())
        {
            activate(outgoing, half->manual.begin()->first);
            break;
        }
    setSize(820, 720);
}

TransitionEditor::~TransitionEditor() = default;

void TransitionEditor::show(ProjectMirror& mirror, TimelineActions& actions, const juce::String& outClipId, const juce::String& inClipId)
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(new TransitionEditor(mirror, actions, outClipId, inClipId));
    options.dialogTitle = "Transition";
    options.dialogBackgroundColour = colours::background;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    if (auto* window = options.launchAsync())
        window->setResizeLimits(820, 720, 4000, 3000);
}

double TransitionEditor::windowBars() const
{
    const auto& m = mirror.model();
    const auto* out = m.findClip(outClipId);
    const auto* in = m.findClip(inClipId);
    if (out == nullptr || in == nullptr) return 0.0;
    const auto window = mix::windowOf(m, outClipId, inClipId);
    if (!window.has_value()) return 0.0;
    const auto curve = m.tempoCurve();
    return (curve.beatAt(window->second) - curve.beatAt(window->first)) / 4.0;
}

void TransitionEditor::changed()
{
    join.out = outEditor->get();
    join.in = inEditor->get();
    join.rampTempo = rampTempo.getToggleState();
    // A curve drawn by hand that has gone back to its option stops being edited.
    const auto& half = activeOut ? join.out : join.in;
    if (activeParam.isNotEmpty() && (!half.has_value() || half->manual.count(activeParam) == 0)) activate(activeOut, {});
    repaint(graphArea());
    startTimer(250);
}

void TransitionEditor::timerCallback()
{
    stopTimer();
    mirror.refreshNow();
    if (!join.out.has_value() && !join.in.has_value() && !join.rampTempo)
    {
        if (const auto* r = mix::recordFor(mirror.model(), outClipId, inClipId))
            mix::remove(actions.engineClient(), actions, mirror.model(), r->getProperty("id", {}).toString());
        return;
    }
    mix::applyJoin(actions.engineClient(), actions, mirror.model(), outClipId, inClipId, join);
}

juce::Rectangle<int> TransitionEditor::graphArea() const
{
    return getLocalBounds().reduced(14).withTrimmedTop(52).withHeight(graphHeight());
}

juce::Rectangle<float> TransitionEditor::band(bool outgoing) const
{
    auto top = graphArea().toFloat().reduced(4.0f).withTrimmedTop(14.0f);
    auto bottom = top.removeFromBottom(top.getHeight() / 2.0f);
    return (outgoing ? top : bottom).withTrimmedLeft(34.0f);
}

juce::Point<float> TransitionEditor::toGraph(bool outgoing, const juce::String& param, double at, double value) const
{
    const auto b = band(outgoing);
    return {b.getX() + static_cast<float>(juce::jlimit(0.0, 1.0, at)) * b.getWidth(),
            b.getBottom() - 3.0f - static_cast<float>(juce::jlimit(0.0, 1.0, presence(param, value))) * (b.getHeight() - 6.0f)};
}

TransitionEditor::HalfEditor& TransitionEditor::editorFor(bool outgoing) const
{
    return outgoing ? *outEditor : *inEditor;
}

std::vector<automation::Point> TransitionEditor::activePoints() const
{
    const auto& half = activeOut ? join.out : join.in;
    if (activeParam.isEmpty() || !half.has_value()) return {};
    const auto it = half->manual.find(activeParam);
    return it != half->manual.end() ? it->second : std::vector<automation::Point>{};
}

void TransitionEditor::setActivePoints(std::vector<automation::Point> points)
{
    std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.timeMs < b.timeMs; });
    editorFor(activeOut).setManual(activeParam, std::move(points));
}

void TransitionEditor::activate(bool outgoing, const juce::String& param)
{
    activeOut = outgoing;
    activeParam = param;
    hint.setText(param.isEmpty() ? juce::String("+ on an option draws its curve here")
                                 : juce::String(outgoing ? "OUT " : "IN ") + labelFor(param)
                                       + ": click to add a point, drag it, double-click to delete; right-click a segment for its shape, right-drag to bend",
                 juce::dontSendNotification);
    repaint(graphArea());
}

int TransitionEditor::pointAt(bool outgoing, const juce::String& param, juce::Point<float> p) const
{
    const auto& half = outgoing ? join.out : join.in;
    if (!half.has_value()) return -1;
    const auto it = half->manual.find(param);
    if (it == half->manual.end()) return -1;
    for (int i = 0; i < static_cast<int>(it->second.size()); ++i)
    {
        const auto& point = it->second[static_cast<size_t>(i)];
        if (toGraph(outgoing, param, point.timeMs, point.value).getDistanceFrom(p) <= 7.0f) return i;
    }
    return -1;
}

void TransitionEditor::drawByHandForTest(bool outgoing, const juce::String& param, const std::vector<automation::Point>& points)
{
    auto& editor = editorFor(outgoing);
    if (!editor.get().has_value() || editor.get()->manual.count(param) == 0) editor.toggleManual(param);
    activate(outgoing, param);
    setActivePoints(points);
    timerCallback();
}

void TransitionEditor::mouseDown(const juce::MouseEvent& e)
{
    dragIndex = -1;
    bendSegment = -1;
    bending = false;
    if (!graphArea().contains(e.getPosition())) return;
    const bool outgoing = band(true).contains(e.position) || !band(false).contains(e.position);
    // A point of any curve drawn by hand in this half picks that curve.
    if (const auto& half = outgoing ? join.out : join.in; half.has_value())
        for (const auto& [param, points] : half->manual)
            if (const int hit = pointAt(outgoing, param, e.position); hit >= 0)
            {
                if (param != activeParam || outgoing != activeOut) activate(outgoing, param);
                if (!e.mods.isPopupMenu()) dragIndex = hit;
                return;
            }
    if (activeParam.isEmpty() || outgoing != activeOut) return;
    auto points = activePoints();
    if (points.size() < 2) return;
    const auto b = band(outgoing);
    const double at = juce::jlimit(0.0, 1.0, static_cast<double>((e.position.x - b.getX()) / b.getWidth()));
    if (e.mods.isPopupMenu())
    {
        // The segment under the pointer: a click shows its shapes, a drag bends it.
        for (int i = 0; i + 1 < static_cast<int>(points.size()); ++i)
            if (at >= points[static_cast<size_t>(i)].timeMs && at <= points[static_cast<size_t>(i) + 1].timeMs) bendSegment = i;
        bendStartY = e.position.y;
        return;
    }
    // A new point where the click is, carried on by the drag.
    const auto& half = activeOut ? join.out : join.in;
    const bool highPass = half.has_value() && half->filter == "highPass";
    const double p = (b.getBottom() - 3.0f - e.position.y) / (b.getHeight() - 6.0f);
    automation::Point added{at, fromPresence(activeParam, p, highPass), "linear", 0.0};
    // The new point takes the shape of the segment it splits.
    for (size_t i = 0; i + 1 < points.size(); ++i)
        if (at > points[i].timeMs && at < points[i + 1].timeMs) added.curve = points[i].curve, added.tension = points[i].tension;
    points.push_back(added);
    std::sort(points.begin(), points.end(), [](const auto& a, const auto& b2) { return a.timeMs < b2.timeMs; });
    for (int i = 0; i < static_cast<int>(points.size()); ++i)
        if (std::abs(points[static_cast<size_t>(i)].timeMs - at) < 1.0e-9) dragIndex = i;
    setActivePoints(points);
}

void TransitionEditor::mouseDrag(const juce::MouseEvent& e)
{
    if (activeParam.isEmpty()) return;
    auto points = activePoints();
    const auto b = band(activeOut);
    if (dragIndex >= 0 && dragIndex < static_cast<int>(points.size()))
    {
        auto& point = points[static_cast<size_t>(dragIndex)];
        // Between its neighbours, so the curve keeps its order.
        const double lo = dragIndex > 0 ? points[static_cast<size_t>(dragIndex) - 1].timeMs + 0.001 : 0.0;
        const double hi = dragIndex + 1 < static_cast<int>(points.size()) ? points[static_cast<size_t>(dragIndex) + 1].timeMs - 0.001 : 1.0;
        point.timeMs = juce::jlimit(lo, juce::jmax(lo, hi), static_cast<double>((e.position.x - b.getX()) / b.getWidth()));
        const auto& half = activeOut ? join.out : join.in;
        point.value = fromPresence(activeParam, (b.getBottom() - 3.0f - e.position.y) / (b.getHeight() - 6.0f),
                                   half.has_value() && half->filter == "highPass");
        editorFor(activeOut).setManual(activeParam, points);
        return;
    }
    if (bendSegment >= 0 && bendSegment < static_cast<int>(points.size()))
    {
        // Up bends towards a fast start, down towards a slow one.
        const float travel = (bendStartY - e.position.y) / 60.0f;
        if (!bending && std::abs(bendStartY - e.position.y) < 3.0f) return;
        bending = true;
        auto& point = points[static_cast<size_t>(bendSegment)];
        const double amount = juce::jlimit(0.0, 1.0, static_cast<double>(std::abs(travel)));
        point.curve = amount < 0.05 ? "linear" : travel > 0.0f ? "fastStart" : "slowStart";
        point.tension = amount * 2.0 - 1.0;
        editorFor(activeOut).setManual(activeParam, points);
    }
}

void TransitionEditor::mouseUp(const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() && !bending && bendSegment >= 0)
    {
        const int segment = bendSegment;
        juce::PopupMenu menu;
        const auto points = activePoints();
        const auto current = segment < static_cast<int>(points.size()) ? points[static_cast<size_t>(segment)].curve : juce::String();
        const std::pair<const char*, const char*> shapes[] = {{"linear", "Linear"},
                                                              {"smooth", "Smooth (S-curve)"},
                                                              {"fastStart", "Fast start (in)"},
                                                              {"slowStart", "Slow start (out)"},
                                                              {"hold", "Hold, then step"}};
        for (const auto& [id, name] : shapes)
            menu.addItem(name, true, current == id, [this, segment, id = juce::String(id)]
                         {
                             auto next = activePoints();
                             if (segment >= static_cast<int>(next.size())) return;
                             next[static_cast<size_t>(segment)].curve = id;
                             next[static_cast<size_t>(segment)].tension = 0.0;
                             setActivePoints(next);
                         });
        menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
    }
    dragIndex = -1;
    bendSegment = -1;
    bending = false;
}

void TransitionEditor::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (activeParam.isEmpty()) return;
    const int hit = pointAt(activeOut, activeParam, e.position);
    auto points = activePoints();
    if (hit < 0 || points.size() <= 2) return;
    points.erase(points.begin() + hit);
    setActivePoints(points);
}

void TransitionEditor::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
    const auto area = graphArea().toFloat();
    g.setColour(colours::zinc950);
    g.fillRect(area);
    g.setColour(colours::zinc700);
    g.drawRect(area, 1.0f);
    // The window's bars and beats: beats faint, bars brighter and numbered along the top.
    const double bars = windowBars();
    const auto grid = band(true).withBottom(band(false).getBottom());
    if (bars > 0.0)
    {
        const int beats = static_cast<int>(std::ceil(bars * 4.0));
        g.setFont(uiFont(9.0f));
        for (int beat = 0; beat <= beats; ++beat)
        {
            const double at = beat / (bars * 4.0);
            if (at > 1.0) break;
            const float x = grid.getX() + static_cast<float>(at) * grid.getWidth();
            const bool bar = beat % 4 == 0;
            g.setColour(bar ? colours::zinc600.withAlpha(0.7f) : colours::zinc800);
            g.drawVerticalLine(juce::roundToInt(x), grid.getY(), grid.getBottom());
            if (bar && beat / 4 < static_cast<int>(std::ceil(bars)))
            {
                g.setColour(colours::textFaint);
                g.drawText(juce::String(beat / 4 + 1), juce::Rectangle<float>(x + 2.0f, area.getY() + 1.0f, 30.0f, 12.0f),
                           juce::Justification::centredLeft);
            }
        }
    }
    g.setColour(colours::zinc800);
    g.drawHorizontalLine(juce::roundToInt(band(false).getY() - 1.0f), area.getX(), area.getRight());
    // Each half's shapes: outgoing in the top half, incoming in the bottom half.
    auto draw = [&](const std::optional<mix::Half>& half, bool outgoing)
    {
        const auto b = band(outgoing);
        g.setColour(outgoing ? colours::amber400.withAlpha(0.6f) : colours::sky400.withAlpha(0.6f));
        g.setFont(uiFont(10.0f, true));
        g.drawText(outgoing ? "OUT" : "IN", juce::Rectangle<float>(area.getX() + 4.0f, b.getY(), 30.0f, b.getHeight()).toNearestInt(),
                   juce::Justification::centredLeft);
        if (!half.has_value()) return;
        float legendX = b.getX();
        for (const auto& shape : mix::shapesFor(*half, outgoing))
        {
            const bool drawn = !shape.shaped.empty();
            const bool active = drawn && outgoing == activeOut && shape.param == activeParam;
            juce::Path path;
            for (size_t i = 0; i < shape.points.size(); ++i)
            {
                const auto [t, v] = shape.points[i];
                const auto p = toGraph(outgoing, shape.param, t, v);
                if (i == 0) path.startNewSubPath(p);
                else path.lineTo(p);
            }
            const auto colour = colourFor(shape.param);
            g.setColour(active || !drawn ? colour : colour.withAlpha(0.75f));
            g.strokePath(path, juce::PathStrokeType(active ? 2.6f : shape.param == "level" ? 2.0f : 1.4f));
            if (drawn)
                for (const auto& point : shape.shaped)
                {
                    const auto p = toGraph(outgoing, shape.param, point.timeMs, point.value);
                    const float r = active ? 4.5f : 3.0f;
                    g.setColour(colours::zinc950);
                    g.fillEllipse(p.x - r, p.y - r, r * 2.0f, r * 2.0f);
                    g.setColour(colour);
                    g.drawEllipse(p.x - r, p.y - r, r * 2.0f, r * 2.0f, active ? 1.8f : 1.2f);
                }
            g.setColour(colour);
            g.setFont(uiFont(9.0f, drawn));
            g.drawText(labelFor(shape.param) + (drawn ? "*" : ""), juce::Rectangle<float>(legendX, b.getY() + 1.0f, 48.0f, 12.0f),
                       juce::Justification::centredLeft);
            legendX += 50.0f;
        }
    };
    draw(join.out, true);
    draw(join.in, false);
}

int TransitionEditor::graphHeight() const
{
    // The halves' rows keep their size; the graph gets the rest.
    return juce::jmax(200, getHeight() - 520);
}

void TransitionEditor::resized()
{
    auto area = getLocalBounds().reduced(14);
    heading.setBounds(area.removeFromTop(22));
    sub.setBounds(area.removeFromTop(28));
    area.removeFromTop(2);
    area.removeFromTop(graphHeight());
    hint.setBounds(area.removeFromTop(16));
    area.removeFromTop(6);
    auto buttons = area.removeFromBottom(28);
    close.setBounds(buttons.removeFromRight(90));
    buttons.removeFromRight(6);
    remove.setBounds(buttons.removeFromRight(90));
    area.removeFromBottom(6);
    rampTempo.setBounds(area.removeFromBottom(24));
    area.removeFromBottom(6);
    auto left = area.removeFromLeft(area.getWidth() / 2 - 8);
    area.removeFromLeft(16);
    outEditor->setBounds(left);
    inEditor->setBounds(area);
}

} // namespace stemcel::native
