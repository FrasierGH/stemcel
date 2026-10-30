#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace stemcel::native
{

// The themes. Terminal: phosphor green on black, the timeline in black and dark grey. Fusion
// Dark: Qt's Fusion style with its common dark palette (window #353535, highlight #2a82da,
// white text), drawn the way Fusion draws its widgets.
enum class ThemeId
{
    terminal,
    fusionDark,
};

// Sets every colour token below for `theme`, and the interface font. Called once at
// startup, before any component is made (components take their colours when they are
// built), so a change of theme applies the next time the app starts.
void applyTheme(ThemeId theme);
ThemeId currentTheme();
// The name stored in the preferences ("terminal", "fusionDark"), and back; unknown names
// are the terminal theme.
juce::String themeName(ThemeId theme);
ThemeId themeFromName(const juce::String& name);

// The colour tokens. Their names are the Tailwind ones the Electron UI used (zinc for the
// neutral scale, sky for the accent), so the code reads the same; their values are the
// theme's, set by applyTheme (the terminal's until then). In the terminal theme zinc runs
// from the black canvas (950) through dark greens (borders, dim lines) to bright phosphor
// text (100); in Fusion Dark from the darkest grey through the window grey to white, with
// 700 the dark outline Fusion draws around its widgets.
namespace colours
{
inline juce::Colour zinc950{0xff000000};
inline juce::Colour zinc900{0xff030a05};
inline juce::Colour zinc800{0xff081a0d};
inline juce::Colour zinc700{0xff0f3a1b};
inline juce::Colour zinc600{0xff1a6330};
inline juce::Colour zinc500{0xff2c8f48};
inline juce::Colour zinc400{0xff4dc46b};
inline juce::Colour zinc300{0xff7fe396};
inline juce::Colour zinc200{0xffa8f5b9};
inline juce::Colour zinc100{0xffd6ffe0};
// The accent: a brighter terminal green.
inline juce::Colour sky400{0xff6dff96};
inline juce::Colour sky500{0xff33ff6e};
inline juce::Colour sky600{0xff16c24e};
// The timeline's neutrals, black to dark grey: canvas, rows, ruler and grid.
inline juce::Colour grey950{0xff050505};
inline juce::Colour grey900{0xff101010};
inline juce::Colour grey800{0xff1b1b1b};
inline juce::Colour grey700{0xff262626};
inline juce::Colour grey600{0xff353535};
inline juce::Colour grey500{0xff4d4d4d};
inline juce::Colour red500{0xffef4444};
inline juce::Colour red600{0xffdc2626};
inline juce::Colour amber400{0xfffbbf24};
inline juce::Colour amber500{0xfff59e0b};
inline juce::Colour emerald500{0xff10b981};
inline juce::Colour violet500{0xff8b5cf6};

inline juce::Colour background = zinc900;
inline juce::Colour panel = zinc800;
inline juce::Colour border = zinc700;
inline juce::Colour text = zinc200;
inline juce::Colour textDim = zinc400;
inline juce::Colour textFaint = zinc500;
inline juce::Colour accent = sky500;
} // namespace colours

// The renderer's TRACK_PALETTE: one hue per entry, in three shades (clip body, outline,
// waveform). A track's colorIndex picks the entry; a clip may override it.
struct PaletteEntry
{
    juce::Colour fill;
    juce::Colour border;
    juce::Colour wave;
};

inline constexpr int kPaletteSize = 16;

// Entry for a colour index (wrapped), optionally sunk toward the timeline background for a
// track that cannot be heard (muted, or another track soloed).
PaletteEntry paletteEntry(int colorIndex, bool silenced = false);
// The swatch shown in pickers.
juce::Colour paletteSwatch(int colorIndex);

class StemcelLookAndFeel final : public juce::LookAndFeel_V4
{
  public:
    StemcelLookAndFeel();

    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                              bool isMouseOverButton, bool isButtonDown) override;
    juce::Font getLabelFont(juce::Label&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    juce::Font getPopupMenuFont() override;
    // Every font in the app (JUCE's own menus and dialogs too) is the interface typeface.
    juce::Typeface::Ptr getTypefaceForFont(const juce::Font& font) override;
    void drawComboBox(juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY, int buttonW,
                      int buttonH, juce::ComboBox&) override;

    // Fusion Dark draws these as Qt's Fusion style does; the terminal theme keeps JUCE's.
    void drawLinearSlider(juce::Graphics&, int x, int y, int width, int height, float sliderPos, float minSliderPos,
                          float maxSliderPos, juce::Slider::SliderStyle, juce::Slider&) override;
    void drawRotarySlider(juce::Graphics&, int x, int y, int width, int height, float sliderPosProportional,
                          float rotaryStartAngle, float rotaryEndAngle, juce::Slider&) override;
    void drawTickBox(juce::Graphics&, juce::Component&, float x, float y, float w, float h, bool ticked, bool isEnabled,
                     bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    void drawScrollbar(juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height, bool isScrollbarVertical,
                       int thumbStartPosition, int thumbSize, bool isMouseOver, bool isMouseDown) override;
    void drawTabButton(juce::TabBarButton&, juce::Graphics&, bool isMouseOver, bool isMouseDown) override;
    void drawTableHeaderBackground(juce::Graphics&, juce::TableHeaderComponent&) override;
    void drawTableHeaderColumn(juce::Graphics&, juce::TableHeaderComponent&, const juce::String& columnName, int columnId,
                               int width, int height, bool isMouseOver, bool isMouseDown, int columnFlags) override;
    void drawProgressBar(juce::Graphics&, juce::ProgressBar&, int width, int height, double progress,
                         const juce::String& textToShow) override;
    void drawPopupMenuBackground(juce::Graphics&, int width, int height) override;

    // The widget colours for the current theme (applyTheme first). The constructor calls it.
    void applyColours();
};

// The terminal theme's typeface: JetBrains Mono (SIL Open Font License 1.1,
// app/fonts/OFL.txt), built into the app so it looks the same on every machine. Fusion Dark
// uses the system's interface font (Segoe UI on Windows, Helvetica Neue on macOS), as Qt does.
juce::Typeface::Ptr interfaceTypeface(bool bold);

// Shared font helper so sizes stay consistent across the app.
juce::Font uiFont(float height, bool bold = false);

} // namespace stemcel::native
