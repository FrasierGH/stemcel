#pragma once

// Live performance (DJ) mode: the controls the decks, mixer and effects answer to, whether
// they come from a controller's mapping, the performance view or a test, and the events the
// audio thread takes them as.

#include <juce_core/juce_core.h>

#include <cstdint>

namespace stemcel::perf
{

constexpr int kDecks = 2;

// Every control, named as in the mapping files (controlName / controlFromName).
enum class Control : std::uint8_t
{
    none,
    // Deck transport
    play,         // PLAY/PAUSE
    playShift,    // SHIFT + PLAY/PAUSE (play/pause too, as in rekordbox)
    cue,          // CUE
    cueShift,     // SHIFT + CUE: back to the start
    sync,         // BEAT SYNC press
    syncLong,     // BEAT SYNC held: sync master
    syncShift,    // SHIFT + BEAT SYNC: tempo range
    tempo,        // TEMPO slider, 0 (−) .. 1 (+)
    keyLock,      // MASTER TEMPO (on screen): value 1 = on, 0 = off, -1 = toggle
    quantize,     // QUANTIZE (on screen): as keyLock
    // Loops
    loopIn,
    loopInLong,   // LOOP IN held: 4-beat loop
    loopOut,
    reloopExit,
    reloopShift,  // SHIFT + RELOOP/EXIT: active loop
    callPrev,     // CUE/LOOP CALL ◁ (halves a loop)
    callNext,     // CUE/LOOP CALL ▷ (doubles a loop)
    callPrevShift, // deletes the called memory cue
    callNextShift, // stores a memory cue
    // Jog
    jogTouch,     // value 1 touched, 0 released
    jogScratch,   // relative ticks, top of the platter in vinyl mode
    jogBend,      // relative ticks, the side (or the top with vinyl mode off)
    jogSearch,    // relative ticks, SHIFT + jog
    // Pads (index 0..7; mode in `mode`)
    padMode,      // a pad mode button (mode in `mode`)
    pad,
    padShift,
    // Channel
    trim,
    eqHi,
    eqMid,
    eqLow,
    filter,       // the FILTER / Sound Color FX knob
    fader,
    hpCue,        // headphone CUE
    hpCueShift,   // SHIFT + CUE: tap BPM
    faderStartPlay,
    faderStartCue,
    load,         // LOAD (the browser picks the item; handled by the app)
    loadShift,
    // Programmatic deck actions (the view, tests)
    beatJump,     // value = beats (signed)
    beatLoop,     // value = beats
    keyShift,     // value = semitones (absolute)
    hotCue,       // index 0..7, value pressed
    hotCueDelete, // index 0..7
    seek,         // value = seconds
    // Mixer and browser
    crossfader,
    masterLevel,
    masterCue,
    masterCueShift,
    hpMix,
    hpLevel,
    browse,       // relative steps
    browseShift,
    browsePress,
    browsePressShift,
    // Beat FX
    fxBeatPrev,
    fxBeatNext,
    fxBpmAuto,
    fxTap,
    fxSelect,
    fxSelectPrev,
    fxChannel,    // index 0 = CH1, 1 = CH2, 2 = master
    fxLevel,
    fxOn,
    fxRelease,
    // On screen (rekordbox's panels): the Sound Color FX and Release FX types, the Beat FX type
    // directly, the sampler's bank and volume, and SHIFT + a sampler pad on an empty slot.
    colorFxSelect, // value = ColorFx index
    releaseFxSelect, // value = 0 Vinyl Brake, 1 Echo, 2 Spin Back
    fxTypeSelect,  // value = BeatFx index
    samplerBank,   // value = 0..3
    samplerVolume, // value 0..1
    samplerLoad,   // index = slot (the app loads the browser's item)
    count
};

enum class PadMode : std::uint8_t
{
    hotCue,
    padFx1,
    beatJump,
    sampler,
    keyboard,
    padFx2,
    beatLoop,
    keyShift,
    count
};

const char* controlName(Control control) noexcept;
Control controlFromName(const juce::String& name) noexcept;
const char* padModeName(PadMode mode) noexcept;
PadMode padModeFromName(const juce::String& name) noexcept; // PadMode::count when unknown

// Controls the app answers (the browser, LOAD, view panels), not the audio thread.
bool isAppControl(Control control) noexcept;

struct DeckTrack; // PerformanceEngine.h

// One control change on its way to the audio thread.
struct Event
{
    Control control = Control::none;
    std::int16_t deck = 0;  // 1..kDecks, 0 = global
    std::int16_t index = 0; // pad, fx channel, sampler slot
    PadMode mode = PadMode::hotCue;
    float value = 0.0F;     // 1/0 for buttons, 0..1 for knobs, ticks for relative controls
};

} // namespace stemcel::perf
