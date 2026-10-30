#include "Theme.h"

#include "StemcelFonts.h"

namespace stemcel::native
{

namespace
{
struct RawEntry
{
    juce::uint32 swatch, fill, border, wave;
};

constexpr RawEntry kPalette[kPaletteSize] = {
    {0xff3b82f6, 0xff1e3a8a, 0xff3b82f6, 0xff93c5fd}, // blue
    {0xffef4444, 0xff7f1d1d, 0xffef4444, 0xfffca5a5}, // red
    {0xfff97316, 0xff7c2d12, 0xfff97316, 0xfffdba74}, // orange
    {0xfff59e0b, 0xff78350f, 0xfff59e0b, 0xfffcd34d}, // amber
    {0xffeab308, 0xff713f12, 0xffeab308, 0xfffde047}, // yellow
    {0xff84cc16, 0xff365314, 0xff84cc16, 0xffbef264}, // lime
    {0xff10b981, 0xff064e3b, 0xff10b981, 0xff6ee7b7}, // emerald
    {0xff14b8a6, 0xff134e4a, 0xff14b8a6, 0xff5eead4}, // teal
    {0xff06b6d4, 0xff164e63, 0xff06b6d4, 0xff67e8f9}, // cyan
    {0xff0ea5e9, 0xff0c4a6e, 0xff0ea5e9, 0xff7dd3fc}, // sky
    {0xff6366f1, 0xff312e81, 0xff6366f1, 0xffa5b4fc}, // indigo
    {0xff8b5cf6, 0xff4c1d95, 0xff8b5cf6, 0xffc4b5fd}, // violet
    {0xffd946ef, 0xff701a75, 0xffd946ef, 0xfff0abfc}, // fuchsia
    {0xffec4899, 0xff831843, 0xffec4899, 0xfff9a8d4}, // pink
    {0xfff43f5e, 0xff881337, 0xfff43f5e, 0xfffda4af}, // rose
    {0xffa1a1aa, 0xff3f3f46, 0xffa1a1aa, 0xffd4d4d8}, // zinc
};

// How far a silenced clip's colours fall back toward the canvas (renderer: SILENCED_MIX).
constexpr float kSilencedMix = 0.65f;

int wrapIndex(int colorIndex)
{
    return ((colorIndex % kPaletteSize) + kPaletteSize) % kPaletteSize;
}
} // namespace

PaletteEntry paletteEntry(int colorIndex, bool silenced)
{
    const auto& raw = kPalette[wrapIndex(colorIndex)];
    PaletteEntry e{juce::Colour(raw.fill), juce::Colour(raw.border), juce::Colour(raw.wave)};
    if (silenced)
    {
        e.fill = e.fill.interpolatedWith(colours::grey950, kSilencedMix);
        e.border = e.border.interpolatedWith(colours::grey950, kSilencedMix);
        e.wave = e.wave.interpolatedWith(colours::grey950, kSilencedMix);
    }
    return e;
}

juce::Colour paletteSwatch(int colorIndex)
{
    return juce::Colour(kPalette[wrapIndex(colorIndex)].swatch);
}

juce::Typeface::Ptr interfaceTypeface(bool bold)
{
    static const auto regular = juce::Typeface::createSystemTypefaceFor(StemcelFonts::JetBrainsMonoRegular_ttf,
                                                                        StemcelFonts::JetBrainsMonoRegular_ttfSize);
    static const auto heavy = juce::Typeface::createSystemTypefaceFor(StemcelFonts::JetBrainsMonoBold_ttf,
                                                                      StemcelFonts::JetBrainsMonoBold_ttfSize);
    return bold ? heavy : regular;
}

juce::Font uiFont(float height, bool bold)
{
    // A monospace face runs wider than the sans the layouts were drawn for: a touch smaller
    // keeps every label in its box.
    constexpr float kMonoScale = 0.9f;
    return juce::Font(juce::FontOptions(interfaceTypeface(bold)).withHeight(height * kMonoScale));
}

StemcelLookAndFeel::StemcelLookAndFeel()
{
    using namespace colours;
    auto scheme = juce::LookAndFeel_V4::getDarkColourScheme();
    scheme.setUIColour(ColourScheme::windowBackground, background);
    scheme.setUIColour(ColourScheme::widgetBackground, panel);
    scheme.setUIColour(ColourScheme::menuBackground, zinc900);
    scheme.setUIColour(ColourScheme::outline, border);
    scheme.setUIColour(ColourScheme::defaultText, text);
    scheme.setUIColour(ColourScheme::defaultFill, accent);
    scheme.setUIColour(ColourScheme::highlightedText, zinc100);
    scheme.setUIColour(ColourScheme::highlightedFill, zinc700);
    scheme.setUIColour(ColourScheme::menuText, text);
    setColourScheme(scheme);

    setColour(juce::ResizableWindow::backgroundColourId, background);
    setColour(juce::DocumentWindow::backgroundColourId, background);
    setColour(juce::TextButton::buttonColourId, zinc900);
    setColour(juce::TextButton::buttonOnColourId, sky600);
    setColour(juce::TextButton::textColourOffId, zinc200);
    setColour(juce::TextButton::textColourOnId, zinc950);
    setColour(juce::ComboBox::backgroundColourId, zinc900);
    setColour(juce::ComboBox::outlineColourId, zinc700);
    setColour(juce::ComboBox::textColourId, zinc200);
    setColour(juce::ComboBox::arrowColourId, zinc400);
    setColour(juce::PopupMenu::backgroundColourId, zinc900);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, zinc800);
    setColour(juce::PopupMenu::textColourId, zinc200);
    setColour(juce::PopupMenu::highlightedTextColourId, zinc100);
    setColour(juce::Label::textColourId, zinc200);
    setColour(juce::TextEditor::backgroundColourId, zinc950);
    setColour(juce::TextEditor::outlineColourId, zinc700);
    setColour(juce::TextEditor::focusedOutlineColourId, sky500);
    setColour(juce::TextEditor::textColourId, zinc100);
    setColour(juce::ToggleButton::textColourId, zinc200);
    setColour(juce::ToggleButton::tickColourId, sky500);
    setColour(juce::ToggleButton::tickDisabledColourId, zinc600);
    setColour(juce::Slider::thumbColourId, sky500);
    setColour(juce::Slider::trackColourId, sky500);
    setColour(juce::Slider::backgroundColourId, zinc700);
    setColour(juce::Slider::textBoxTextColourId, zinc200);
    setColour(juce::Slider::textBoxOutlineColourId, zinc700);
    setColour(juce::ListBox::backgroundColourId, zinc900);
    setColour(juce::ScrollBar::thumbColourId, zinc600);
    setColour(juce::TabbedButtonBar::tabTextColourId, zinc400);
    setColour(juce::TabbedButtonBar::frontTextColourId, zinc100);
    setColour(juce::TabbedComponent::backgroundColourId, zinc900);
    setColour(juce::TabbedComponent::outlineColourId, zinc700);
    setColour(juce::TreeView::backgroundColourId, zinc900);
    setColour(juce::TreeView::selectedItemBackgroundColourId, zinc800);
    setColour(juce::AlertWindow::backgroundColourId, zinc900);
    setColour(juce::AlertWindow::textColourId, zinc200);
    setColour(juce::AlertWindow::outlineColourId, zinc700);
    setColour(juce::CaretComponent::caretColourId, sky500);
    setColour(juce::DirectoryContentsDisplayComponent::highlightColourId, zinc800);
}

juce::Font StemcelLookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight)
{
    return uiFont(juce::jmin(13.0f, static_cast<float>(buttonHeight) * 0.6f));
}

void StemcelLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                                const juce::Colour& backgroundColour, bool isMouseOverButton,
                                                bool isButtonDown)
{
    // Terminal keys: square, a thin green rule, lit when hovered or held.
    auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
    auto base = backgroundColour;
    if (!button.isEnabled()) base = base.withMultipliedAlpha(0.5f);
    else if (isButtonDown) base = base.brighter(0.25f);
    else if (isMouseOverButton) base = base.brighter(0.12f);
    g.setColour(base);
    g.fillRect(bounds);
    g.setColour(isMouseOverButton && button.isEnabled() ? colours::zinc500 : colours::zinc700);
    g.drawRect(bounds, 1.0f);
}

void StemcelLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool, int, int, int, int,
                                        juce::ComboBox& box)
{
    const juce::Rectangle<float> bounds(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    g.setColour(box.findColour(juce::ComboBox::backgroundColourId));
    g.fillRect(bounds);
    g.setColour(box.hasKeyboardFocus(true) ? colours::zinc500 : box.findColour(juce::ComboBox::outlineColourId));
    g.drawRect(bounds.reduced(0.5f), 1.0f);
    // A terminal caret for the arrow.
    g.setColour(box.findColour(juce::ComboBox::arrowColourId).withAlpha(box.isEnabled() ? 1.0f : 0.3f));
    g.setFont(uiFont(12.0f, true));
    g.drawText("v", juce::Rectangle<int>(width - 18, 0, 14, height), juce::Justification::centred);
}

juce::Typeface::Ptr StemcelLookAndFeel::getTypefaceForFont(const juce::Font& font)
{
    return interfaceTypeface(font.isBold());
}

juce::Font StemcelLookAndFeel::getLabelFont(juce::Label& label)
{
    return label.getFont();
}

juce::Font StemcelLookAndFeel::getComboBoxFont(juce::ComboBox&)
{
    return uiFont(13.0f);
}

juce::Font StemcelLookAndFeel::getPopupMenuFont()
{
    return uiFont(13.0f);
}

} // namespace stemcel::native
