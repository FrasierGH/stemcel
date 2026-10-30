#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

namespace stemcel::plugins
{

// Every plugin format this build hosts (ADR 0025): VST3; VST 2 (through FST's headers, see
// CMakeLists' STEMCEL_HOST_VST2); Audio Units on macOS. The catalogue, the scan worker and
// an export's own catalogue all register the same list, so a plugin found by one loads in all.
inline void addHostedFormats(juce::AudioPluginFormatManager& manager)
{
    manager.addFormat(std::make_unique<juce::VST3PluginFormat>());
#if JUCE_PLUGINHOST_VST
    manager.addFormat(std::make_unique<juce::VSTPluginFormat>());
#endif
#if JUCE_PLUGINHOST_AU && JUCE_MAC
    manager.addFormat(std::make_unique<juce::AudioUnitPluginFormat>());
#endif
}

} // namespace stemcel::plugins
