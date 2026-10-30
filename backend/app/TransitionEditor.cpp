#include "TransitionEditor.h"

#include "Theme.h"

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
    }

    void set(const std::optional<mix::Half>& h)
    {
        value = h;
        kind.setSelectedItemIndex(h.has_value() ? kindIds.indexOf(h->kind) + 1 : 0, juce::dontSendNotification);
        load();
    }

    const std::optional<mix::Half>& get() const { return value; }
    std::function<void()> onChange;

    void resized() override
    {
        auto area = getLocalBounds();
        title.setBounds(area.removeFromTop(20));
        kind.setBounds(area.removeFromTop(24));
        area.removeFromTop(6);
        auto row = [&area](juce::Label& label, juce::Component& c, juce::Component* second = nullptr)
        {
            auto r = area.removeFromTop(24);
            label.setBounds(r.removeFromLeft(78));
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
        row(levelLabel, level);
        row(filterLabel, filter, &depth);
        row(lowLabel, low);
        row(midLabel, mid);
        row(highLabel, high);
        row(swapLabel, swap);
        row(killLabel, kill);
        if (outgoing) row(effectLabel, effect, &extra);
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
                "lengthen it.",
                juce::dontSendNotification);
    addAndMakeVisible(sub);

    outEditor = std::make_unique<HalfEditor>(true, [this] { return windowBars(); });
    inEditor = std::make_unique<HalfEditor>(false, [this] { return windowBars(); });
    outEditor->set(join.out);
    inEditor->set(join.in);
    for (auto* e : {outEditor.get(), inEditor.get()})
    {
        e->onChange = [this] { changed(); };
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
    setSize(820, 620);
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
    options.resizable = false;
    options.launchAsync();
}

double TransitionEditor::windowBars() const
{
    const auto& m = mirror.model();
    const auto* out = m.findClip(outClipId);
    const auto* in = m.findClip(inClipId);
    if (out == nullptr || in == nullptr) return 0.0;
    const auto window = mix::overlap(*out, *in);
    if (!window.has_value()) return 0.0;
    const auto curve = m.tempoCurve();
    return (curve.beatAt(window->second) - curve.beatAt(window->first)) / 4.0;
}

void TransitionEditor::changed()
{
    join.out = outEditor->get();
    join.in = inEditor->get();
    join.rampTempo = rampTempo.getToggleState();
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
    return getLocalBounds().reduced(14).withTrimmedTop(52).withHeight(120);
}

void TransitionEditor::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
    const auto area = graphArea().toFloat();
    g.setColour(colours::zinc950);
    g.fillRect(area);
    g.setColour(colours::zinc700);
    g.drawRect(area, 1.0f);
    // Bar lines across the window.
    const double bars = windowBars();
    if (bars > 0.0)
        for (int b = 1; b < static_cast<int>(std::ceil(bars)); ++b)
        {
            const auto x = area.getX() + static_cast<float>(b / bars) * area.getWidth();
            g.setColour(colours::zinc800);
            g.drawVerticalLine(juce::roundToInt(x), area.getY(), area.getBottom());
        }
    // Each half's shapes: outgoing in the top half, incoming in the bottom half.
    auto draw = [&](const std::optional<mix::Half>& half, bool outgoing, juce::Rectangle<float> band)
    {
        g.setColour(outgoing ? colours::amber400.withAlpha(0.6f) : colours::sky400.withAlpha(0.6f));
        g.setFont(uiFont(10.0f, true));
        g.drawText(outgoing ? "OUT" : "IN", band.removeFromLeft(34.0f).toNearestInt(), juce::Justification::centredLeft);
        if (!half.has_value()) return;
        float legendX = band.getX();
        for (const auto& shape : mix::shapesFor(*half, outgoing))
        {
            juce::Path path;
            for (size_t i = 0; i < shape.points.size(); ++i)
            {
                const auto& [t, v] = shape.points[i];
                const float x = band.getX() + static_cast<float>(t) * band.getWidth();
                const float y = band.getBottom() - 3.0f - static_cast<float>(presence(shape.param, v)) * (band.getHeight() - 6.0f);
                if (i == 0) path.startNewSubPath(x, y);
                else path.lineTo(x, y);
            }
            g.setColour(colourFor(shape.param));
            g.strokePath(path, juce::PathStrokeType(shape.param == "level" ? 2.0f : 1.4f));
            g.setFont(uiFont(9.0f));
            g.drawText(labelFor(shape.param), juce::Rectangle<float>(legendX, band.getY() + 1.0f, 44.0f, 12.0f), juce::Justification::centredLeft);
            legendX += 46.0f;
        }
    };
    auto top = area.reduced(4.0f);
    auto bottom = top.removeFromBottom(top.getHeight() / 2.0f);
    draw(join.out, true, top);
    draw(join.in, false, bottom);
}

void TransitionEditor::resized()
{
    auto area = getLocalBounds().reduced(14);
    heading.setBounds(area.removeFromTop(22));
    sub.setBounds(area.removeFromTop(28));
    area.removeFromTop(2);
    area.removeFromTop(120); // the graph
    area.removeFromTop(10);
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
