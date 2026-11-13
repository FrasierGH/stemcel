#pragma once

// A controller's performance mapping (resources/midi-mappings/performance/*.json): which MIDI
// message is which control, and which messages light which LEDs. One file per controller;
// the deck logic is the same for all of them.
//
// "midi" is two hex bytes: the status, then the note or CC number. In the status, "n" is the
// deck's channel (deckChannels), and "P" / "S" stand for the deck's pad channel without and
// with SHIFT (padChannels). A deck-scoped entry applies to every deck; "deck" pins it to one.

#include "PerfTypes.h"

#include <array>
#include <memory>
#include <vector>

namespace stemcel::perf
{

class Mapping
{
  public:
    enum class Encoding : std::uint8_t
    {
        button,    // note or CC, value > 0 = pressed
        absolute,  // 7-bit, or 14-bit with "lsb"
        rel64,     // relative around 0x40 (0x41 = +1, 0x3F = −1)
        relSigned  // relative, 1..63 = +, 127..65 = −
    };

    struct Led
    {
        int status = 0, data1 = 0;
    };

    // Loads every mapping in the folder; null when none matches `deviceName`.
    static std::shared_ptr<const Mapping> forDevice(const juce::String& deviceName);
    static std::shared_ptr<const Mapping> fromJson(const juce::var& json, juce::String& error);
    static std::shared_ptr<const Mapping> fromFile(const juce::File& file, juce::String& error);
    // Every model any installed mapping is for ("DDJ-400", ...).
    static juce::StringArray allModels();

    const juce::String& name() const noexcept { return mappingName; }
    const std::vector<std::vector<juce::uint8>>& initMessages() const noexcept { return init; }
    int jogTicksPerTurn() const noexcept { return ticksPerTurn; }

    // The LED messages for a state ("play", "pad.hotCue", ...) on a deck (0 for the global
    // ones), pad `index` for pad states: the plain address first, then the SHIFT one.
    std::vector<Led> leds(const juce::String& state, int deck, int index = 0) const;

    // Turns raw MIDI into control events. Stateful (a 14-bit control's MSB is held until its
    // LSB arrives), so one Translator per input, used on that input's MIDI thread.
    class Translator
    {
      public:
        explicit Translator(std::shared_ptr<const Mapping> mapping);
        // Up to two events (most messages give one); returns how many were written.
        int translate(const juce::uint8* bytes, int size, Event* out, int maxEvents);

      private:
        std::shared_ptr<const Mapping> mapping;
        std::array<std::array<int, 128>, 16> msb{}, lsb{};
    };

  private:
    struct Binding
    {
        Control control = Control::none;
        Encoding encoding = Encoding::button;
        int deck = 0;
        int index = 0;
        PadMode mode = PadMode::hotCue;
        bool isLsb = false;     // this entry is the LSB half of a 14-bit control
        int msbData1 = -1;      // for an LSB: where its MSB came
        bool fourteenBit = false;
        bool pressOnly = false; // only the press (value > 0) is an event
    };
    struct LedBinding
    {
        juce::String state;
        int deck = 0;
        int count = 1;
        std::vector<Led> addresses; // per deck already resolved; base addresses for counts
    };

    juce::String mappingName;
    std::vector<juce::String> models;
    std::vector<std::vector<juce::uint8>> init;
    int ticksPerTurn = 720;
    // [status byte][data1] → the bindings on it (several only for odd mappings).
    std::array<std::array<std::vector<Binding>, 128>, 256> table;
    std::vector<LedBinding> ledBindings;

    friend class Translator;
};

} // namespace stemcel::perf
