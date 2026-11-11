#pragma once

// The performance view's mouse controls: knobs, faders and buttons that send PERF_CONTROL and
// follow the engine's state (the controller may move the same control).

#include "EngineClient.h"
#include "Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace stemcel::native::perf
{

inline void sendControl(EngineClient& engine, const juce::String& control, int deck, double value, int index = 0,
                        const juce::String& mode = {})
{
    auto* o = new juce::DynamicObject();
    o->setProperty("control", control);
    o->setProperty("deck", deck);
    o->setProperty("value", value);
    if (index != 0) o->setProperty("index", index);
    if (mode.isNotEmpty()) o->setProperty("mode", mode);
    engine.send("PERF_CONTROL", juce::var(o));
}

// A knob or fader bound to one control. Shows the engine's value unless the mouse holds it.
class ControlSlider final : public juce::Slider
{
  public:
    ControlSlider(EngineClient& e, juce::String controlName, int deckNumber, juce::Slider::SliderStyle style,
                  juce::String caption = {})
        : juce::Slider(style, juce::Slider::NoTextBox), engine(e), control(std::move(controlName)), deck(deckNumber),
          label(std::move(caption))
    {
        setRange(0.0, 1.0);
        setValue(0.5, juce::dontSendNotification);
        setDoubleClickReturnValue(true, control == "fader" ? 1.0 : 0.5);
        setTooltip(label);
        onValueChange = [this]
        {
            if (!updating) sendControl(engine, control, deck, inverted ? 1.0 - getValue() : getValue());
        };
    }
    // Shown upside down: the control's 0 at the top.
    void setInverted(bool on) { inverted = on; }
    void follow(double v)
    {
        if (isMouseButtonDown()) return;
        updating = true;
        setValue(inverted ? 1.0 - v : v, juce::dontSendNotification);
        updating = false;
    }
    const juce::String& caption() const { return label; }

  private:
    EngineClient& engine;
    juce::String control;
    int deck;
    juce::String label;
    bool updating = false;
    bool inverted = false;
};

// A button that sends 1 on press and 0 on release (CUE, PLAY, pads), lit by the state.
class HoldButton final : public juce::Component
{
  public:
    std::function<void(bool pressed)> onPress;

    explicit HoldButton(juce::String text, juce::Colour litColour = colours::accent)
        : label(std::move(text)), lit(litColour)
    {
        setRepaintsOnMouseActivity(true);
    }
    void setText(const juce::String& t)
    {
        if (t != label)
        {
            label = t;
            repaint();
        }
    }
    void setLit(bool on, juce::Colour c)
    {
        if (on != isLit || c != lit)
        {
            isLit = on;
            lit = c;
            repaint();
        }
    }
    void setLit(bool on) { setLit(on, lit); }
    void paint(juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced(1.5F);
        const bool down = isMouseButtonDown();
        g.setColour(isLit ? lit.withAlpha(down ? 0.95F : 0.8F) : (down ? colours::zinc700 : colours::panel));
        g.fillRoundedRectangle(r, 4.0F);
        g.setColour(isLit ? lit.brighter(0.3F) : (isMouseOver() ? colours::zinc500 : colours::border));
        g.drawRoundedRectangle(r, 4.0F, 1.0F);
        g.setColour(isLit ? colours::onAccent : colours::text);
        g.setFont(uiFont(juce::jlimit(9.0F, 13.0F, r.getHeight() * 0.42F), true));
        g.drawFittedText(label, r.toNearestInt().reduced(2), juce::Justification::centred, 2);
    }
    void mouseDown(const juce::MouseEvent&) override
    {
        if (onPress) onPress(true);
        repaint();
    }
    void mouseUp(const juce::MouseEvent&) override
    {
        if (onPress) onPress(false);
        repaint();
    }

  private:
    juce::String label;
    juce::Colour lit;
    bool isLit = false;
};

} // namespace stemcel::native::perf
