#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace stemcel::native
{

// The terminal theme: phosphor green on black, the timeline in black and dark grey. The token names are the
// Tailwind ones the Electron UI used (zinc for the neutral scale, sky for the accent), so the
// code reads the same; their values are the terminal's. zinc runs from the black canvas
// (950) through dark greens (borders, dim lines) to bright phosphor text (100).
namespace colours
{
inline const juce::Colour zinc950{0xff000000};
inline const juce::Colour zinc900{0xff030a05};
inline const juce::Colour zinc800{0xff081a0d};
inline const juce::Colour zinc700{0xff0f3a1b};
inline const juce::Colour zinc600{0xff1a6330};
inline const juce::Colour zinc500{0xff2c8f48};
inline const juce::Colour zinc400{0xff4dc46b};
inline const juce::Colour zinc300{0xff7fe396};
inline const juce::Colour zinc200{0xffa8f5b9};
inline const juce::Colour zinc100{0xffd6ffe0};
// The accent: a brighter terminal green.
inline const juce::Colour sky400{0xff6dff96};
inline const juce::Colour sky500{0xff33ff6e};
inline const juce::Colour sky600{0xff16c24e};
// The timeline's neutrals, black to dark grey: canvas, rows, ruler and grid.
inline const juce::Colour grey950{0xff050505};
inline const juce::Colour grey900{0xff101010};
inline const juce::Colour grey800{0xff1b1b1b};
inline const juce::Colour grey700{0xff262626};
inline const juce::Colour grey600{0xff353535};
inline const juce::Colour grey500{0xff4d4d4d};
inline const juce::Colour red500{0xffef4444};
inline const juce::Colour red600{0xffdc2626};
inline const juce::Colour amber400{0xfffbbf24};
inline const juce::Colour amber500{0xfff59e0b};
inline const juce::Colour emerald500{0xff10b981};
inline const juce::Colour violet500{0xff8b5cf6};

inline const juce::Colour background = zinc900;
inline const juce::Colour panel = zinc800;
inline const juce::Colour border = zinc700;
inline const juce::Colour text = zinc200;
inline const juce::Colour textDim = zinc400;
inline const juce::Colour textFaint = zinc500;
inline const juce::Colour accent = sky500;
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
};

// The interface typeface: JetBrains Mono (SIL Open Font License 1.1, app/fonts/OFL.txt),
// built into the app so it looks the same on every machine.
juce::Typeface::Ptr interfaceTypeface(bool bold);

// Shared font helper so sizes stay consistent across the app.
juce::Font uiFont(float height, bool bold = false);

} // namespace stemcel::native
