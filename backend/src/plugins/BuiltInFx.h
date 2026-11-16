#pragma once

// Stemcel's own effects as plugins: performance mode's Beat FX and Sound Color FX, offered in
// a track's FX beside VST3/AU plugins. Hosting them as plugins gives them everything a plugin
// slot has (its place in the chain, bypass, the slot's mix, automation of their parameters,
// saving with the project and the same sound in an export) with nothing new to keep in step.
//
// Parameters (the automation lanes' "plugin:<slot>:<index>"):
//   Beat FX:        0 Strength (LEVEL/DEPTH), 1 Beats (BEAT ◁/▷'s steps), 2 On
//   Sound Color FX: 0 Strength (the FILTER knob: centre off, one character each way)
// Their tempo is the timeline's, from the host's play head.

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

namespace stemcel::plugins
{

inline const juce::String kBuiltInFormatName{"Stemcel"};

// Every built-in effect, Beat FX then Sound Color FX ("djfx:beat:<type>", "djfx:color:<type>").
juce::Array<juce::PluginDescription> builtInDescriptions();
bool isBuiltIn(const juce::PluginDescription& description);
bool isBuiltInIdentifier(const juce::String& identifier);

// Null for an identifier that isn't one of them.
std::unique_ptr<juce::AudioPluginInstance> createBuiltIn(const juce::PluginDescription& description, double sampleRate,
                                                         int blockSize);

} // namespace stemcel::plugins
