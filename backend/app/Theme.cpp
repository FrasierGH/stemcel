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
    if (id == ThemeId::fusionDark)
    {
        well = juce::Colour(0xff1f1f1f);
        card = juce::Colour(0xff2d2d2d);
        rule = juce::Colour(0xff262626);
        onAccent = juce::Colours::white;
    }
    else
    {
        well = juce::Colour(0xff000000);
        card = juce::Colour(0xff04110a);
        rule = zinc700;
        onAccent = juce::Colour(0xff000000);
    }
}

bool isLitColour(juce::Colour c)
{
    return c.getSaturation() > 0.35f && c.getBrightness() > 0.45f;
}

void drawWell(juce::Graphics& g, juce::Rectangle<float> area)
{
    if (fusion())
    {
        // Sunken: a darker face, a shadow along the top inside edge, the outline round it
        // and a faint light line below (the bevel catching the light).
        namespace f = fusionPalette;
        g.setGradientFill(juce::ColourGradient::vertical(colours::well.darker(0.15f), area.getY(), colours::well.brighter(0.04f),
                                                         area.getBottom()));
        g.fillRoundedRectangle(area, 2.5f);
        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.drawHorizontalLine(juce::roundToInt(area.getY() + 1.0f), area.getX() + 2.0f, area.getRight() - 2.0f);
        g.setColour(f::outline);
        g.drawRoundedRectangle(area.reduced(0.5f), 2.5f, 1.0f);
        g.setColour(juce::Colours::white.withAlpha(0.05f));
        g.drawHorizontalLine(juce::roundToInt(area.getBottom()), area.getX() + 2.0f, area.getRight() - 2.0f);
        return;
    }
    // Black glass: a faint phosphor bloom from the top, a dim rim, brighter corners.
    g.setColour(colours::well);
    g.fillRect(area);
    g.setGradientFill(juce::ColourGradient::vertical(colours::sky500.withAlpha(0.06f), area.getY(), colours::sky500.withAlpha(0.0f),
                                                     area.getCentreY()));
    g.fillRect(area);
    g.setColour(colours::zinc700);
    g.drawRect(area, 1.0f);
    g.setColour(colours::zinc500);
    constexpr float tick = 4.0f;
    for (const auto corner : {area.getTopLeft(), area.getTopRight().translated(-1.0f, 0.0f), area.getBottomLeft().translated(0.0f, -1.0f),
                              area.getBottomRight().translated(-1.0f, -1.0f)})
    {
        const float dx = corner.x < area.getCentreX() ? 1.0f : -1.0f;
        const float dy = corner.y < area.getCentreY() ? 1.0f : -1.0f;
        g.fillRect(juce::Rectangle<float>(corner.x, corner.y, 1.0f, 1.0f).withWidth(tick).translated(dx < 0 ? -(tick - 1.0f) : 0.0f, 0.0f));
        g.fillRect(juce::Rectangle<float>(corner.x, corner.y, 1.0f, 1.0f).withHeight(tick).translated(0.0f, dy < 0 ? -(tick - 1.0f) : 0.0f));
    }
}

void drawGhostDigits(juce::Graphics& g, const juce::String& text, const juce::Font& font, juce::Rectangle<int> area,
                     juce::Justification justification)
{
    if (fusion() || text.isEmpty()) return;
    juce::String ghost;
    for (const auto c : text) ghost << (juce::CharacterFunctions::isDigit(c) ? juce::juce_wchar('8') : c);
    g.setColour(colours::sky500.withAlpha(0.1f));
    g.setFont(font);
    g.drawText(ghost, area, justification, false);
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
    const auto swatch = juce::Colour(kPalette[wrapIndex(colorIndex)].swatch);
    const float h = swatch.getHue();
    const float s = swatch.getSaturation();
    PaletteEntry e;
    if (fusion())
    {
        // Muted mid-tones that sit in Fusion's greys, the waveform drawn dark into them (as
        // most DAWs on a grey desk do), the name strip a shade lighter with dark text.
        e.fill = juce::Colour::fromHSV(h, s * 0.46f, 0.58f, 1.0f);
        e.border = juce::Colour::fromHSV(h, s * 0.55f, 0.36f, 1.0f);
        e.wave = juce::Colour::fromHSV(h, s * 0.65f, 0.17f, 1.0f);
        e.header = juce::Colour::fromHSV(h, s * 0.38f, 0.82f, 1.0f);
        e.headerText = juce::Colour(0xff141414);
    }
    else
    {
        // Dark glass tinted with the hue and a bright trace of the waveform, like a scope.
        e.fill = juce::Colour::fromHSV(h, s * 0.85f, 0.16f, 1.0f);
        e.border = juce::Colour::fromHSV(h, s * 0.75f, 0.62f, 1.0f);
        e.wave = juce::Colour::fromHSV(h, s * 0.42f, 1.0f, 1.0f);
        e.header = juce::Colour::fromHSV(h, s * 0.8f, 0.30f, 1.0f);
        e.headerText = juce::Colour::fromHSV(h, s * 0.25f, 1.0f, 1.0f);
    }
    if (silenced)
        for (auto* c : {&e.fill, &e.border, &e.wave, &e.header, &e.headerText})
            *c = c->interpolatedWith(colours::grey950, kSilencedMix);
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
    setColour(juce::TextButton::textColourOnId, sky400);
    setColour(juce::ComboBox::backgroundColourId, zinc950);
    setColour(juce::ComboBox::outlineColourId, zinc700);
    setColour(juce::ComboBox::textColourId, zinc200);
    setColour(juce::ComboBox::arrowColourId, zinc400);
    // Menus in inverse video, as a terminal highlights a choice.
    setColour(juce::PopupMenu::backgroundColourId, zinc950);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, sky600);
    setColour(juce::PopupMenu::textColourId, zinc200);
    setColour(juce::PopupMenu::highlightedTextColourId, zinc950);
    setColour(juce::PopupMenu::headerTextColourId, zinc400);
    setColour(juce::TooltipWindow::backgroundColourId, zinc950);
    setColour(juce::TooltipWindow::textColourId, zinc200);
    setColour(juce::TooltipWindow::outlineColourId, zinc600);
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
    setColour(juce::PopupMenu::headerTextColourId, f::text.withAlpha(0.6f));
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
        if (button.getProperties()["tab"])
        {
            // A Fusion tab: rounded on top; the front one is the pane's grey with the highlight
            // along its top, the others a little darker and lower.
            namespace f = fusionPalette;
            const bool front = button.getToggleState();
            auto r = button.getLocalBounds().toFloat();
            if (!front) r.removeFromTop(2.0f);
            juce::Path shape;
            shape.addRoundedRectangle(r.getX() + 0.5f, r.getY() + 0.5f, r.getWidth() - 1.0f, r.getHeight() - 0.5f, 3.0f, 3.0f, true, true,
                                      false, false);
            const auto face = front ? f::window.brighter(0.05f) : f::window.darker(isMouseOverButton ? 0.06f : 0.16f);
            g.setGradientFill(juce::ColourGradient::vertical(face.brighter(0.06f), r.getY(), face, r.getBottom()));
            g.fillPath(shape);
            g.setColour(f::outline);
            g.strokePath(shape, juce::PathStrokeType(1.0f));
            if (front)
            {
                g.setColour(f::highlight);
                g.fillRect(r.getX() + 2.0f, r.getY() + 1.0f, r.getWidth() - 4.0f, 2.0f);
            }
            return;
        }
        // Fusion push buttons: a bevel; a highlight-tinted outline when on or focused.
        const bool on = button.getToggleState() && button.getClickingTogglesState();
        const auto outline = on || button.hasKeyboardFocus(false) || isLitColour(backgroundColour) ? fusionPalette::highlight.darker(0.45f)
                                                                                                   : fusionPalette::outline;
        fusionBevel(g, button.getLocalBounds().toFloat().reduced(0.5f), backgroundColour, isButtonDown, isMouseOverButton,
                    button.isEnabled(), outline);
        return;
    }
    auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
    if (button.getProperties()["tab"])
    {
        // A terminal tab: no face, the one in front underlined in the accent.
        if (button.getToggleState())
        {
            g.setColour(colours::sky500.withAlpha(0.07f));
            g.fillRect(bounds);
            g.setColour(colours::sky500);
            g.fillRect(bounds.removeFromBottom(2.0f));
        }
        else if (isMouseOverButton)
        {
            g.setColour(colours::zinc600);
            g.fillRect(bounds.removeFromBottom(1.0f));
        }
        return;
    }
    if (isLitColour(backgroundColour) && button.isEnabled())
    {
        // Lit: dark glass tinted with the colour, a bright rim and a faint inner one.
        g.setColour(colours::zinc950);
        g.fillRect(bounds);
        g.setColour(backgroundColour.withAlpha(isButtonDown ? 0.34f : isMouseOverButton ? 0.26f : 0.18f));
        g.fillRect(bounds);
        g.setColour(backgroundColour);
        g.drawRect(bounds, 1.0f);
        g.setColour(backgroundColour.withAlpha(0.22f));
        g.drawRect(bounds.reduced(2.0f), 1.0f);
        return;
    }
    // Terminal keys: square, a thin green rule, brighter when hovered or held.
    auto base = backgroundColour;
    if (!button.isEnabled()) base = base.withMultipliedAlpha(0.5f);
    else if (isButtonDown) base = base.brighter(0.25f);
    else if (isMouseOverButton) base = base.brighter(0.12f);
    g.setColour(base);
    g.fillRect(bounds);
    g.setColour(isMouseOverButton && button.isEnabled() ? colours::zinc500 : colours::zinc700);
    g.drawRect(bounds, 1.0f);
    // A light line inside the top edge, as a key cap catches it.
    g.setColour(colours::zinc100.withAlpha(isButtonDown ? 0.0f : 0.05f));
    g.drawHorizontalLine(juce::roundToInt(bounds.getY() + 1.0f), bounds.getX() + 1.0f, bounds.getRight() - 1.0f);
}

void StemcelLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button, bool isMouseOverButton, bool)
{
    const auto font = getTextButtonFont(button, button.getHeight());
    g.setFont(font);
    const bool tab = button.getProperties()["tab"];
    const auto face = button.findColour(button.getToggleState() ? juce::TextButton::buttonOnColourId : juce::TextButton::buttonColourId);
    auto colour = button.findColour(button.getToggleState() ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId);
    if (tab)
        colour = button.getToggleState() ? (fusion() ? fusionPalette::text : colours::sky400)
                                         : (fusion() ? fusionPalette::text.withAlpha(isMouseOverButton ? 0.9f : 0.65f)
                                                     : (isMouseOverButton ? colours::zinc300 : colours::zinc500));
    else if (!fusion() && isLitColour(face))
        colour = face.brighter(0.5f); // the terminal's lit keys glow in their own colour
    g.setColour(colour.withMultipliedAlpha(button.isEnabled() ? 1.0f : 0.5f));
    const int yIndent = juce::jmin(4, button.proportionOfHeight(0.3f));
    const int cornerSize = juce::jmin(button.getHeight(), button.getWidth()) / 2;
    const int fontHeight = juce::roundToInt(font.getHeight() * 0.6f);
    const int leftIndent = juce::jmin(fontHeight, 2 + cornerSize / (button.isConnectedOnLeft() ? 4 : 2));
    const int rightIndent = juce::jmin(fontHeight, 2 + cornerSize / (button.isConnectedOnRight() ? 4 : 2));
    const int textWidth = button.getWidth() - leftIndent - rightIndent;
    if (textWidth > 0)
        g.drawFittedText(button.getButtonText(), leftIndent, yIndent, textWidth, button.getHeight() - yIndent * 2,
                         juce::Justification::centred, 2);
}

void StemcelLookAndFeel::fillTextEditorBackground(juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    const auto r = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    g.setColour(editor.findColour(juce::TextEditor::backgroundColourId));
    if (fusion()) g.fillRoundedRectangle(r, 2.0f);
    else g.fillRect(r);
}

void StemcelLookAndFeel::drawTextEditorOutline(juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    if (!editor.isEnabled() || editor.findColour(juce::TextEditor::outlineColourId).isTransparent()) return;
    const auto r = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)).reduced(0.5f);
    const bool focused = editor.hasKeyboardFocus(true) && !editor.isReadOnly();
    if (fusion())
    {
        // Qt's line edit: sunken, a shadow inside the top edge, the highlight round it when focused.
        g.setColour(juce::Colours::black.withAlpha(0.22f));
        g.drawHorizontalLine(1, 2.0f, static_cast<float>(width) - 2.0f);
        g.setColour(focused ? fusionPalette::highlight : editor.findColour(juce::TextEditor::outlineColourId));
        g.drawRoundedRectangle(r, 2.0f, 1.0f);
        return;
    }
    g.setColour(focused ? colours::sky500 : editor.findColour(juce::TextEditor::outlineColourId));
    g.drawRect(r, 1.0f);
    if (focused)
    {
        g.setColour(colours::sky500.withAlpha(0.2f));
        g.drawRect(r.reduced(1.0f), 1.0f);
    }
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
    g.setColour(box.isPopupActive() ? colours::sky500 : box.hasKeyboardFocus(true) || box.isMouseOver(true) ? colours::zinc500
                                                                                                          : box.findColour(juce::ComboBox::outlineColourId));
    g.drawRect(bounds.reduced(0.5f), 1.0f);
    // A divider and a chevron for the arrow.
    g.setColour(colours::zinc700);
    g.drawVerticalLine(width - 20, 4.0f, static_cast<float>(height - 4));
    const auto c = juce::Rectangle<float>(static_cast<float>(width) - 20.0f, 0.0f, 20.0f, static_cast<float>(height)).getCentre();
    juce::Path chevron;
    chevron.startNewSubPath(c.x - 3.5f, c.y - 1.5f);
    chevron.lineTo(c.x, c.y + 2.0f);
    chevron.lineTo(c.x + 3.5f, c.y - 1.5f);
    g.setColour(box.findColour(juce::ComboBox::arrowColourId).withAlpha(box.isEnabled() ? 1.0f : 0.3f));
    g.strokePath(chevron, juce::PathStrokeType(1.5f, juce::PathStrokeType::mitered, juce::PathStrokeType::square));
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
    if (!plain)
    {
        LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }
    if (!fusion())
    {
        // Terminal: a hairline groove with scale ticks, lit up to the handle, and a block handle
        // (a fader cap when vertical).
        const bool horizontal = slider.isHorizontal();
        const auto area = juce::Rectangle<int>(x, y, width, height).toFloat();
        const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
        const float zero = bipolar ? static_cast<float>(slider.getPositionOfValue(0.0)) : (horizontal ? area.getX() : area.getBottom());
        const auto track = horizontal ? juce::Rectangle<float>(area.getX(), area.getCentreY() - 1.0f, area.getWidth(), 2.0f)
                                      : juce::Rectangle<float>(area.getCentreX() - 1.0f, area.getY(), 2.0f, area.getHeight());
        g.setColour(colours::zinc700);
        g.fillRect(track);
        g.setColour(colours::zinc700.withAlpha(0.8f));
        for (int i = 0; i <= 10; ++i)
        {
            const float t = static_cast<float>(i) / 10.0f;
            const float len = i % 5 == 0 ? 3.0f : 1.5f;
            if (horizontal)
                g.fillRect(area.getX() + t * (area.getWidth() - 1.0f), track.getBottom() + 2.0f, 1.0f, len);
            else
                g.fillRect(track.getRight() + 3.0f, area.getY() + t * (area.getHeight() - 1.0f), len, 1.0f);
        }
        const float a = juce::jmin(zero, sliderPos), b = juce::jmax(zero, sliderPos);
        const auto lit = horizontal ? track.withX(a).withWidth(b - a) : track.withY(a).withHeight(b - a);
        if (slider.isEnabled())
        {
            g.setColour(colours::sky500.withAlpha(0.25f));
            g.fillRect(lit.expanded(horizontal ? 0.0f : 1.0f, horizontal ? 1.0f : 0.0f));
            g.setColour(colours::sky500);
            g.fillRect(lit);
        }
        const bool hot = slider.isMouseOverOrDragging() && slider.isEnabled();
        const auto handle = horizontal ? juce::Rectangle<float>(6.0f, juce::jmin(14.0f, area.getHeight())).withCentre({sliderPos, area.getCentreY()})
                                       : juce::Rectangle<float>(juce::jmin(22.0f, area.getWidth()), 9.0f).withCentre({area.getCentreX(), sliderPos});
        g.setColour(colours::zinc950);
        g.fillRect(handle);
        g.setColour(slider.isEnabled() ? (hot ? colours::sky400 : colours::sky500) : colours::zinc600);
        g.drawRect(handle, 1.0f);
        if (horizontal) g.fillRect(handle.reduced(2.0f, 3.0f).withSizeKeepingCentre(2.0f, handle.getHeight() - 6.0f));
        else g.fillRect(handle.withSizeKeepingCentre(handle.getWidth() - 6.0f, 1.0f));
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
    const auto area = juce::Rectangle<int>(x, y, width, height).toFloat().reduced(2.0f);
    const float radius = juce::jmin(area.getWidth(), area.getHeight()) / 2.0f;
    const auto centre = area.getCentre();
    const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
    // A dial with a centre (pan) lights from the centre out.
    const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
    const float zeroAngle = bipolar ? rotaryStartAngle
                                          + static_cast<float>(slider.valueToProportionOfLength(0.0)) * (rotaryEndAngle - rotaryStartAngle)
                                    : rotaryStartAngle;
    if (!fusion())
    {
        // Terminal: a ring of segments lit from zero to the value, round a black knob with a
        // bright pointer.
        constexpr int kSegments = 21;
        const float outer = radius, inner = radius * 0.72f;
        const float lo = juce::jmin(zeroAngle, angle), hi = juce::jmax(zeroAngle, angle);
        for (int i = 0; i < kSegments; ++i)
        {
            const float a = rotaryStartAngle + (rotaryEndAngle - rotaryStartAngle) * static_cast<float>(i) / static_cast<float>(kSegments - 1);
            const bool on = slider.isEnabled() && a >= lo - 0.01f && a <= hi + 0.01f;
            g.setColour(on ? colours::sky500 : colours::zinc700);
            g.drawLine(juce::Line<float>(centre.getPointOnCircumference(inner, a), centre.getPointOnCircumference(outer, a)), on ? 2.0f : 1.5f);
        }
        const float knob = inner - 3.0f;
        const auto disc = juce::Rectangle<float>(knob * 2.0f, knob * 2.0f).withCentre(centre);
        g.setColour(colours::zinc950);
        g.fillEllipse(disc);
        g.setColour(slider.isMouseOverOrDragging() ? colours::zinc500 : colours::zinc600);
        g.drawEllipse(disc, 1.0f);
        g.setColour(slider.isEnabled() ? colours::sky400 : colours::zinc600);
        g.drawLine(juce::Line<float>(centre.getPointOnCircumference(knob * 0.2f, angle), centre.getPointOnCircumference(knob * 0.9f, angle)), 2.0f);
        return;
    }
    // Fusion's dial: a bevelled disc in a groove ring that the highlight fills, and a notch.
    namespace f = fusionPalette;
    const float ring = juce::jmax(2.0f, radius * 0.16f);

    juce::Path groove;
    groove.addCentredArc(centre.x, centre.y, radius - ring / 2, radius - ring / 2, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
    // The groove reads on any panel: the shadow colour, a hairline of light beneath it.
    g.setColour(f::shadow.withAlpha(0.85f));
    g.strokePath(groove, juce::PathStrokeType(ring, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    if (slider.isEnabled())
    {
        juce::Path value;
        value.addCentredArc(centre.x, centre.y, radius - ring / 2, radius - ring / 2, 0.0f, juce::jmin(zeroAngle, angle),
                            juce::jmax(zeroAngle, angle), true);
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
        // Terminal: a square with a lit block inside when ticked.
        const float side = juce::jmin(w, h, 13.0f);
        const auto box = juce::Rectangle<float>(side, side).withCentre({x + w / 2, y + h / 2}).reduced(0.5f);
        g.setColour(colours::zinc950);
        g.fillRect(box);
        g.setColour(highlighted && isEnabled ? colours::zinc400 : colours::zinc600);
        g.drawRect(box, 1.0f);
        if (ticked)
        {
            g.setColour(isEnabled ? (down ? colours::sky400 : colours::sky500) : colours::zinc600);
            g.fillRect(box.reduced(3.0f));
        }
        juce::ignoreUnused(component);
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
        // Terminal: a slim square thumb, lit while held.
        juce::ignoreUnused(bar);
        const auto area = juce::Rectangle<int>(x, y, width, height).toFloat();
        if (thumbSize <= 0) return;
        const float thick = isMouseOver || isMouseDown ? 6.0f : 4.0f;
        const auto thumb = vertical ? juce::Rectangle<float>(area.getCentreX() - thick / 2, static_cast<float>(thumbStart) + 1.0f, thick,
                                                             static_cast<float>(thumbSize) - 2.0f)
                                    : juce::Rectangle<float>(static_cast<float>(thumbStart) + 1.0f, area.getCentreY() - thick / 2,
                                                             static_cast<float>(thumbSize) - 2.0f, thick);
        g.setColour(isMouseDown ? colours::sky600 : isMouseOver ? colours::zinc500 : colours::zinc600);
        g.fillRect(thumb);
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
        g.fillAll(colours::zinc950);
        g.setColour(colours::zinc600);
        g.drawRect(0, 0, width, height, 1);
        return;
    }
    namespace f = fusionPalette;
    g.setGradientFill(juce::ColourGradient::vertical(f::window.brighter(0.04f), 0.0f, f::window, static_cast<float>(height)));
    g.fillAll();
    g.setColour(f::outline.brighter(0.2f));
    g.drawRect(0, 0, width, height, 1);
}


} // namespace stemcel::native
