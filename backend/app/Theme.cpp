#include "Theme.h"

#include "StemcelFonts.h"

namespace stemcel::native
{

namespace
{
ThemeId theme = ThemeId::terminal;

bool fusion()
{
    return theme == ThemeId::fusionDark;
}

// The interface font Qt's Fusion style takes from the system.
juce::String systemInterfaceFamily()
{
#if JUCE_WINDOWS
    return "Segoe UI";
#elif JUCE_MAC
    return "Helvetica Neue";
#else
    return juce::Font::getDefaultSansSerifFontName();
#endif
}

juce::String systemMonospaceFamily()
{
#if JUCE_WINDOWS
    return "Consolas";
#elif JUCE_MAC
    return "Menlo";
#else
    return juce::Font::getDefaultMonospacedFontName();
#endif
}

// Fusion's dark palette, after the one most Qt applications set for it.
namespace fusionPalette
{
const juce::Colour window{0xff353535};
const juce::Colour base{0xff2a2a2a};
const juce::Colour alternateBase{0xff424242};
const juce::Colour dark{0xff232323};
const juce::Colour shadow{0xff141414};
const juce::Colour outline{0xff1f1f1f}; // window.darker(140%), round every widget
const juce::Colour text{0xffffffff};
const juce::Colour disabledText{0xff7f7f7f};
const juce::Colour highlight{0xff2a82da};
} // namespace fusionPalette

// A Fusion bevel: the face's vertical gradient, a light line inside the top edge, and the
// dark outline, on a rounded rectangle.
void fusionBevel(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour face, bool down, bool hover, bool enabled,
                 juce::Colour outline = fusionPalette::outline, float radius = 2.0f)
{
    if (!enabled) face = face.withMultipliedSaturation(0.5f).darker(0.1f);
    auto top = down ? face.darker(0.12f) : face.brighter(hover ? 0.12f : 0.07f);
    auto bottom = down ? face.darker(0.04f) : face.darker(hover ? 0.0f : 0.06f);
    g.setGradientFill(juce::ColourGradient::vertical(top, r.getY(), bottom, r.getBottom()));
    g.fillRoundedRectangle(r, radius);
    if (!down && enabled)
    {
        g.setColour(juce::Colours::white.withAlpha(0.07f));
        g.drawHorizontalLine(juce::roundToInt(r.getY() + 1.0f), r.getX() + radius, r.getRight() - radius);
    }
    g.setColour(outline);
    g.drawRoundedRectangle(r, radius, 1.0f);
}

void fusionArrowDown(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
{
    const auto c = area.getCentre();
    juce::Path arrow;
    arrow.addTriangle(c.x - 3.5f, c.y - 1.5f, c.x + 3.5f, c.y - 1.5f, c.x, c.y + 2.5f);
    g.setColour(colour);
    g.fillPath(arrow);
}
} // namespace

ThemeId currentTheme()
{
    return theme;
}

juce::String themeName(ThemeId id)
{
    return id == ThemeId::fusionDark ? "fusionDark" : "terminal";
}

ThemeId themeFromName(const juce::String& name)
{
    return name == "fusionDark" ? ThemeId::fusionDark : ThemeId::terminal;
}

void applyTheme(ThemeId id)
{
    theme = id;
    using namespace colours;
    if (id == ThemeId::fusionDark)
    {
        namespace f = fusionPalette;
        zinc950 = f::dark.darker(0.15f);
        zinc900 = f::window;
        zinc800 = f::alternateBase;
        zinc700 = f::outline;
        zinc600 = juce::Colour(0xff5c5c5c);
        zinc500 = f::disabledText;
        zinc400 = juce::Colour(0xffb4b4b4);
        zinc300 = juce::Colour(0xffd2d2d2);
        zinc200 = f::text;
        zinc100 = f::text;
        sky400 = f::highlight.brighter(0.25f);
        sky500 = f::highlight;
        sky600 = f::highlight.darker(0.15f);
        grey950 = juce::Colour(0xff262626);
        grey900 = juce::Colour(0xff2c2c2c);
        grey800 = juce::Colour(0xff333333);
        grey700 = juce::Colour(0xff3c3c3c);
        grey600 = juce::Colour(0xff474747);
        grey500 = juce::Colour(0xff5e5e5e);
    }
    else
    {
        zinc950 = juce::Colour(0xff000000);
        zinc900 = juce::Colour(0xff030a05);
        zinc800 = juce::Colour(0xff081a0d);
        zinc700 = juce::Colour(0xff0f3a1b);
        zinc600 = juce::Colour(0xff1a6330);
        zinc500 = juce::Colour(0xff2c8f48);
        zinc400 = juce::Colour(0xff4dc46b);
        zinc300 = juce::Colour(0xff7fe396);
        zinc200 = juce::Colour(0xffa8f5b9);
        zinc100 = juce::Colour(0xffd6ffe0);
        sky400 = juce::Colour(0xff6dff96);
        sky500 = juce::Colour(0xff33ff6e);
        sky600 = juce::Colour(0xff16c24e);
        grey950 = juce::Colour(0xff050505);
        grey900 = juce::Colour(0xff101010);
        grey800 = juce::Colour(0xff1b1b1b);
        grey700 = juce::Colour(0xff262626);
        grey600 = juce::Colour(0xff353535);
        grey500 = juce::Colour(0xff4d4d4d);
    }
    background = zinc900;
    panel = zinc800;
    border = zinc700;
    text = zinc200;
    textDim = zinc400;
    textFaint = zinc500;
    accent = sky500;
}

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
    if (fusion())
        return juce::Font(juce::FontOptions(systemInterfaceFamily(), height, bold ? juce::Font::bold : juce::Font::plain));
    // A monospace face runs wider than the sans the layouts were drawn for: a touch smaller
    // keeps every label in its box.
    constexpr float kMonoScale = 0.9f;
    return juce::Font(juce::FontOptions(interfaceTypeface(bold)).withHeight(height * kMonoScale));
}

StemcelLookAndFeel::StemcelLookAndFeel()
{
    applyColours();
}

void StemcelLookAndFeel::applyColours()
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

    if (!fusion()) return;
    // Fusion Dark: lists and editors on the darker base, the window grey for buttons and
    // menus, the blue highlight with white text for anything selected.
    namespace f = fusionPalette;
    scheme.setUIColour(ColourScheme::windowBackground, f::window);
    scheme.setUIColour(ColourScheme::widgetBackground, f::window);
    scheme.setUIColour(ColourScheme::menuBackground, f::window);
    scheme.setUIColour(ColourScheme::outline, f::outline);
    scheme.setUIColour(ColourScheme::defaultFill, f::highlight);
    scheme.setUIColour(ColourScheme::highlightedFill, f::highlight);
    scheme.setUIColour(ColourScheme::highlightedText, f::text);
    setColourScheme(scheme);
    setColour(juce::ResizableWindow::backgroundColourId, f::window);
    setColour(juce::DocumentWindow::backgroundColourId, f::window);
    setColour(juce::TextButton::buttonColourId, f::window);
    setColour(juce::TextButton::buttonOnColourId, f::highlight);
    setColour(juce::TextButton::textColourOffId, f::text);
    setColour(juce::TextButton::textColourOnId, f::text);
    setColour(juce::ComboBox::backgroundColourId, f::window);
    setColour(juce::ComboBox::outlineColourId, f::outline);
    setColour(juce::ComboBox::textColourId, f::text);
    setColour(juce::ComboBox::arrowColourId, f::text);
    setColour(juce::PopupMenu::backgroundColourId, f::window);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, f::highlight);
    setColour(juce::PopupMenu::textColourId, f::text);
    setColour(juce::PopupMenu::highlightedTextColourId, f::text);
    setColour(juce::Label::textColourId, f::text);
    setColour(juce::TextEditor::backgroundColourId, f::base);
    setColour(juce::TextEditor::outlineColourId, f::outline);
    setColour(juce::TextEditor::focusedOutlineColourId, f::highlight);
    setColour(juce::TextEditor::textColourId, f::text);
    setColour(juce::TextEditor::highlightColourId, f::highlight);
    setColour(juce::TextEditor::highlightedTextColourId, f::text);
    setColour(juce::ToggleButton::textColourId, f::text);
    setColour(juce::ToggleButton::tickColourId, f::text);
    setColour(juce::ToggleButton::tickDisabledColourId, f::disabledText);
    setColour(juce::Slider::thumbColourId, f::window);
    setColour(juce::Slider::trackColourId, f::highlight);
    setColour(juce::Slider::backgroundColourId, f::base);
    setColour(juce::Slider::rotarySliderFillColourId, f::highlight);
    setColour(juce::Slider::rotarySliderOutlineColourId, f::base);
    setColour(juce::Slider::textBoxTextColourId, f::text);
    setColour(juce::Slider::textBoxBackgroundColourId, f::base);
    setColour(juce::Slider::textBoxOutlineColourId, f::outline);
    setColour(juce::ListBox::backgroundColourId, f::base);
    setColour(juce::ListBox::outlineColourId, f::outline);
    setColour(juce::ScrollBar::thumbColourId, f::window.brighter(0.1f));
    setColour(juce::TabbedButtonBar::tabTextColourId, f::text.withAlpha(0.7f));
    setColour(juce::TabbedButtonBar::frontTextColourId, f::text);
    setColour(juce::TabbedComponent::backgroundColourId, f::window);
    setColour(juce::TabbedComponent::outlineColourId, f::outline);
    setColour(juce::TreeView::backgroundColourId, f::base);
    setColour(juce::TreeView::selectedItemBackgroundColourId, f::highlight);
    setColour(juce::TableHeaderComponent::backgroundColourId, f::window);
    setColour(juce::TableHeaderComponent::textColourId, f::text);
    setColour(juce::TableHeaderComponent::outlineColourId, f::outline);
    setColour(juce::AlertWindow::backgroundColourId, f::window);
    setColour(juce::AlertWindow::textColourId, f::text);
    setColour(juce::AlertWindow::outlineColourId, f::outline);
    setColour(juce::ProgressBar::backgroundColourId, f::base);
    setColour(juce::ProgressBar::foregroundColourId, f::highlight);
    setColour(juce::TooltipWindow::backgroundColourId, f::base);
    setColour(juce::TooltipWindow::textColourId, f::text);
    setColour(juce::TooltipWindow::outlineColourId, f::outline);
    setColour(juce::CaretComponent::caretColourId, f::text);
    setColour(juce::DirectoryContentsDisplayComponent::highlightColourId, f::highlight);
    setColour(juce::DirectoryContentsDisplayComponent::highlightedTextColourId, f::text);
}

juce::Font StemcelLookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight)
{
    return uiFont(juce::jmin(13.0f, static_cast<float>(buttonHeight) * 0.6f));
}

void StemcelLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                                const juce::Colour& backgroundColour, bool isMouseOverButton,
                                                bool isButtonDown)
{
    if (fusion())
    {
        // Fusion push buttons: a bevel; a highlight-tinted outline when on or focused.
        const bool on = button.getToggleState() && button.getClickingTogglesState();
        const auto outline = on || button.hasKeyboardFocus(false) ? fusionPalette::highlight.darker(0.45f) : fusionPalette::outline;
        fusionBevel(g, button.getLocalBounds().toFloat().reduced(0.5f), backgroundColour, isButtonDown, isMouseOverButton,
                    button.isEnabled(), outline);
        return;
    }
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
    if (fusion())
    {
        // Fusion's combo box is a push button with a down arrow at its right.
        const auto outline = box.hasKeyboardFocus(true) ? fusionPalette::highlight.darker(0.3f) : fusionPalette::outline;
        fusionBevel(g, bounds.reduced(0.5f), box.findColour(juce::ComboBox::backgroundColourId), box.isPopupActive(),
                    box.isMouseOver(true), box.isEnabled(), outline);
        fusionArrowDown(g, juce::Rectangle<float>(static_cast<float>(width) - 18.0f, 0.0f, 14.0f, static_cast<float>(height)),
                        box.findColour(juce::ComboBox::arrowColourId).withAlpha(box.isEnabled() ? 0.9f : 0.3f));
        return;
    }
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
    if (fusion())
    {
        // JUCE's own menus and dialogs ask for the default sans: the system interface font;
        // and the clock asks for the default monospace: the system's coding font, not Courier.
        const auto name = font.getTypefaceName();
        const auto family = name == juce::Font::getDefaultSansSerifFontName() ? systemInterfaceFamily()
                            : name == juce::Font::getDefaultMonospacedFontName() ? systemMonospaceFamily()
                                                                                  : juce::String();
        if (family.isEmpty()) return juce::Font::getDefaultTypefaceForFont(font);
        return juce::Font::getDefaultTypefaceForFont(juce::Font(juce::FontOptions(family, font.getHeight(), font.getStyleFlags())));
    }
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

void StemcelLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                         float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle style,
                                         juce::Slider& slider)
{
    const bool plain = style == juce::Slider::LinearHorizontal || style == juce::Slider::LinearVertical;
    if (!fusion() || !plain)
    {
        LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }
    // Fusion: a sunken four-pixel groove, filled with the highlight up to the handle, and a
    // bevelled handle.
    namespace f = fusionPalette;
    const bool horizontal = slider.isHorizontal();
    const auto area = juce::Rectangle<int>(x, y, width, height).toFloat();
    constexpr float groove = 4.0f;
    const auto track = horizontal ? juce::Rectangle<float>(area.getX(), area.getCentreY() - groove / 2, area.getWidth(), groove)
                                  : juce::Rectangle<float>(area.getCentreX() - groove / 2, area.getY(), groove, area.getHeight());
    g.setColour(f::base.darker(0.2f));
    g.fillRoundedRectangle(track, 2.0f);
    const auto filled = horizontal ? track.withRight(sliderPos) : track.withTop(sliderPos);
    g.setGradientFill(horizontal ? juce::ColourGradient::vertical(f::highlight.brighter(0.15f), filled.getY(), f::highlight.darker(0.1f), filled.getBottom())
                                 : juce::ColourGradient::horizontal(f::highlight.brighter(0.15f), filled.getX(), f::highlight.darker(0.1f), filled.getRight()));
    if (slider.isEnabled()) g.fillRoundedRectangle(filled, 2.0f);
    g.setColour(f::outline);
    g.drawRoundedRectangle(track, 2.0f, 1.0f);

    const float along = juce::jmin(16.0f, horizontal ? area.getHeight() : area.getWidth());
    const auto handle = horizontal ? juce::Rectangle<float>(11.0f, along).withCentre({sliderPos, area.getCentreY()})
                                   : juce::Rectangle<float>(along, 11.0f).withCentre({area.getCentreX(), sliderPos});
    fusionBevel(g, handle.reduced(0.5f), f::window, slider.isMouseButtonDown(), slider.isMouseOverOrDragging(), slider.isEnabled());
}

void StemcelLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                         float rotaryStartAngle, float rotaryEndAngle, juce::Slider& slider)
{
    if (!fusion())
    {
        LookAndFeel_V4::drawRotarySlider(g, x, y, width, height, sliderPos, rotaryStartAngle, rotaryEndAngle, slider);
        return;
    }
    // Fusion's dial: a bevelled disc in a groove ring that the highlight fills, and a notch.
    namespace f = fusionPalette;
    const auto area = juce::Rectangle<int>(x, y, width, height).toFloat().reduced(2.0f);
    const float radius = juce::jmin(area.getWidth(), area.getHeight()) / 2.0f;
    const auto centre = area.getCentre();
    const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
    const float ring = juce::jmax(2.0f, radius * 0.16f);

    juce::Path groove;
    groove.addCentredArc(centre.x, centre.y, radius - ring / 2, radius - ring / 2, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
    g.setColour(f::base.darker(0.2f));
    g.strokePath(groove, juce::PathStrokeType(ring, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    if (slider.isEnabled())
    {
        juce::Path value;
        value.addCentredArc(centre.x, centre.y, radius - ring / 2, radius - ring / 2, 0.0f, rotaryStartAngle, angle, true);
        g.setColour(f::highlight);
        g.strokePath(value, juce::PathStrokeType(ring, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    const float knob = radius - ring - 2.0f;
    const auto disc = juce::Rectangle<float>(knob * 2, knob * 2).withCentre(centre);
    auto face = f::window;
    if (slider.isMouseOverOrDragging()) face = face.brighter(0.08f);
    g.setGradientFill(juce::ColourGradient::vertical(face.brighter(0.12f), disc.getY(), face.darker(0.1f), disc.getBottom()));
    g.fillEllipse(disc);
    g.setColour(f::outline);
    g.drawEllipse(disc, 1.0f);
    const auto tip = centre.getPointOnCircumference(knob * 0.8f, angle);
    const auto from = centre.getPointOnCircumference(knob * 0.3f, angle);
    g.setColour(slider.isEnabled() ? f::text : f::disabledText);
    g.drawLine({from, tip}, 2.0f);
}

void StemcelLookAndFeel::drawTickBox(juce::Graphics& g, juce::Component& component, float x, float y, float w, float h,
                                    bool ticked, bool isEnabled, bool highlighted, bool down)
{
    if (!fusion())
    {
        LookAndFeel_V4::drawTickBox(g, component, x, y, w, h, ticked, isEnabled, highlighted, down);
        return;
    }
    // Fusion's check box: a sunken base-coloured square and a light tick.
    namespace f = fusionPalette;
    const float side = juce::jmin(w, h, 14.0f);
    const auto box = juce::Rectangle<float>(side, side).withCentre({x + w / 2, y + h / 2}).reduced(0.5f);
    g.setGradientFill(juce::ColourGradient::vertical(f::base.darker(0.15f), box.getY(), f::base.brighter(down ? 0.0f : 0.06f), box.getBottom()));
    g.fillRoundedRectangle(box, 2.0f);
    g.setColour(highlighted && isEnabled ? f::highlight.darker(0.2f) : f::outline);
    g.drawRoundedRectangle(box, 2.0f, 1.0f);
    if (ticked)
    {
        juce::Path tick;
        const auto t = box.reduced(side * 0.22f);
        tick.startNewSubPath(t.getX(), t.getCentreY());
        tick.lineTo(t.getX() + t.getWidth() * 0.38f, t.getBottom());
        tick.lineTo(t.getRight(), t.getY());
        g.setColour(isEnabled ? f::text : f::disabledText);
        g.strokePath(tick, juce::PathStrokeType(1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

void StemcelLookAndFeel::drawScrollbar(juce::Graphics& g, juce::ScrollBar& bar, int x, int y, int width, int height,
                                      bool vertical, int thumbStart, int thumbSize, bool isMouseOver, bool isMouseDown)
{
    if (!fusion())
    {
        LookAndFeel_V4::drawScrollbar(g, bar, x, y, width, height, vertical, thumbStart, thumbSize, isMouseOver, isMouseDown);
        return;
    }
    // Fusion: a recessed groove and a bevelled handle.
    namespace f = fusionPalette;
    const auto area = juce::Rectangle<int>(x, y, width, height).toFloat();
    g.setColour(f::window.darker(0.15f));
    g.fillRect(area);
    if (thumbSize <= 0) return;
    const auto thumb = vertical ? juce::Rectangle<float>(area.getX() + 1, static_cast<float>(thumbStart), area.getWidth() - 2, static_cast<float>(thumbSize))
                                : juce::Rectangle<float>(static_cast<float>(thumbStart), area.getY() + 1, static_cast<float>(thumbSize), area.getHeight() - 2);
    fusionBevel(g, thumb.reduced(0.5f), f::window.brighter(0.1f), isMouseDown, isMouseOver, true);
}

void StemcelLookAndFeel::drawTabButton(juce::TabBarButton& button, juce::Graphics& g, bool isMouseOver, bool isMouseDown)
{
    if (!fusion())
    {
        LookAndFeel_V4::drawTabButton(button, g, isMouseOver, isMouseDown);
        return;
    }
    // Fusion tabs: rounded on top; the front one is the pane's colour and joins it, the
    // others sit a little darker and lower.
    namespace f = fusionPalette;
    const bool front = button.isFrontTab();
    auto r = button.getActiveArea().toFloat();
    if (!front) r.removeFromTop(2.0f);
    juce::Path shape;
    shape.addRoundedRectangle(r.getX() + 0.5f, r.getY() + 0.5f, r.getWidth() - 1.0f, r.getHeight(), 3.0f, 3.0f, true, true, false, false);
    const auto face = front ? f::window : f::window.darker(isMouseOver ? 0.08f : 0.18f);
    g.setGradientFill(juce::ColourGradient::vertical(face.brighter(0.06f), r.getY(), face, r.getBottom()));
    g.fillPath(shape);
    g.setColour(f::outline);
    g.strokePath(shape, juce::PathStrokeType(1.0f));
    if (front)
    {
        g.setColour(f::highlight);
        g.fillRect(r.getX() + 2.0f, r.getY() + 0.5f, r.getWidth() - 4.0f, 2.0f);
    }
    g.setColour(front ? f::text : f::text.withAlpha(0.7f));
    g.setFont(uiFont(13.0f));
    g.drawText(button.getButtonText().trim(), r.toNearestInt(), juce::Justification::centred, true);
}

void StemcelLookAndFeel::drawTableHeaderBackground(juce::Graphics& g, juce::TableHeaderComponent& header)
{
    if (!fusion())
    {
        LookAndFeel_V4::drawTableHeaderBackground(g, header);
        return;
    }
    // Fusion's header sections are buttons: a bevel per column, separated by the outline.
    namespace f = fusionPalette;
    const auto r = header.getLocalBounds().toFloat();
    g.setGradientFill(juce::ColourGradient::vertical(f::window.brighter(0.08f), r.getY(), f::window.darker(0.06f), r.getBottom()));
    g.fillRect(r);
    g.setColour(f::outline);
    g.drawHorizontalLine(header.getHeight() - 1, 0.0f, r.getRight());
    for (int i = header.getNumColumns(true); --i >= 0;)
        g.fillRect(header.getColumnPosition(i).removeFromRight(1).reduced(0, 3));
}

void StemcelLookAndFeel::drawTableHeaderColumn(juce::Graphics& g, juce::TableHeaderComponent& header,
                                              const juce::String& columnName, int columnId, int width, int height,
                                              bool isMouseOver, bool isMouseDown, int columnFlags)
{
    if (!fusion())
    {
        LookAndFeel_V4::drawTableHeaderColumn(g, header, columnName, columnId, width, height, isMouseOver, isMouseDown, columnFlags);
        return;
    }
    namespace f = fusionPalette;
    if (isMouseDown) g.fillAll(juce::Colours::black.withAlpha(0.15f));
    else if (isMouseOver) g.fillAll(juce::Colours::white.withAlpha(0.04f));
    auto area = juce::Rectangle<int>(width, height).reduced(6, 0);
    if ((columnFlags & (juce::TableHeaderComponent::sortedForwards | juce::TableHeaderComponent::sortedBackwards)) != 0)
    {
        juce::Path arrow;
        const auto a = area.removeFromRight(10).toFloat().withSizeKeepingCentre(7.0f, 4.0f);
        if ((columnFlags & juce::TableHeaderComponent::sortedForwards) != 0)
            arrow.addTriangle(a.getX(), a.getBottom(), a.getRight(), a.getBottom(), a.getCentreX(), a.getY());
        else
            arrow.addTriangle(a.getX(), a.getY(), a.getRight(), a.getY(), a.getCentreX(), a.getBottom());
        g.setColour(f::text.withAlpha(0.7f));
        g.fillPath(arrow);
    }
    g.setColour(f::text);
    g.setFont(uiFont(13.0f));
    g.drawFittedText(columnName, area, juce::Justification::centredLeft, 1);
}

void StemcelLookAndFeel::drawProgressBar(juce::Graphics& g, juce::ProgressBar& bar, int width, int height,
                                        double progress, const juce::String& textToShow)
{
    if (!fusion() || progress < 0.0 || progress > 1.0)
    {
        LookAndFeel_V4::drawProgressBar(g, bar, width, height, progress, textToShow);
        return;
    }
    // Fusion: a sunken base trough with a highlight bar.
    namespace f = fusionPalette;
    const auto r = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)).reduced(0.5f);
    g.setColour(f::base);
    g.fillRoundedRectangle(r, 2.0f);
    const auto done = r.withWidth(r.getWidth() * static_cast<float>(progress)).reduced(1.0f);
    g.setGradientFill(juce::ColourGradient::vertical(f::highlight.brighter(0.15f), done.getY(), f::highlight.darker(0.1f), done.getBottom()));
    g.fillRoundedRectangle(done, 1.5f);
    g.setColour(f::outline);
    g.drawRoundedRectangle(r, 2.0f, 1.0f);
    if (textToShow.isNotEmpty())
    {
        g.setColour(f::text);
        g.setFont(uiFont(12.0f));
        g.drawText(textToShow, 0, 0, width, height, juce::Justification::centred, false);
    }
}

void StemcelLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height)
{
    if (!fusion())
    {
        LookAndFeel_V4::drawPopupMenuBackground(g, width, height);
        return;
    }
    namespace f = fusionPalette;
    g.fillAll(f::window);
    g.setColour(f::outline.brighter(0.2f));
    g.drawRect(0, 0, width, height, 1);
}


} // namespace stemcel::native
