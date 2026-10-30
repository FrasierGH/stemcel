#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

namespace stemcel::native
{

// Approximate musical key for library metadata (Krumhansl profiles over a Goertzel
// chroma), ported from the renderer's `detectMusicalKey` so both apps label a file the
// same way: "A minor", "Eb major". Empty when too short, silent or ambiguous.
juce::String detectMusicalKey(const juce::AudioBuffer<float>& audio, double sampleRate);

// Reads up to the first two minutes of `reader`, downmixed and decimated to ~11 kHz the
// way the renderer feeds its detector, and returns the key (or empty).
juce::String detectMusicalKey(juce::AudioFormatReader& reader);

// A key as rekordbox, tags or Camelot/Open Key notation spell it ("Am", "F#m", "8A",
// "10d", "Abmaj", "G minor") in Stemcel's form ("A minor"), or empty when unparseable.
juce::String normaliseKeyName(const juce::String& key);

} // namespace stemcel::native
