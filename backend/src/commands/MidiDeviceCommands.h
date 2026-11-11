#pragma once

#include <juce_core/juce_core.h>

namespace stemcel
{

class BridgeServer;
class AudioEngine;

// Live performance mode: while a sink is set, input from the devices it accepts goes to it on
// the MIDI thread (straight to the decks) instead of the timeline controls.
class MidiPerformanceSink
{
  public:
    virtual ~MidiPerformanceSink() = default;
    virtual bool acceptsDevice(const juce::String& deviceName) const = 0;
    // Message thread: an accepted device is open (or was handed over); send it its init.
    virtual void deviceOpened(const juce::String& identifier, const juce::String& deviceName) = 0;
    // MIDI thread.
    virtual void handleMidi(const juce::String& identifier, const juce::uint8* data, int size) = 0;
};
void setMidiPerformanceSink(MidiPerformanceSink* sink);
// Raw bytes to an enabled device's output (message thread); false when it has none.
bool sendMidiToDevice(const juce::String& identifier, const juce::uint8* data, int size);

// Builds the MIDI_DEVICES_LIST envelope from the current MIDI input devices.
juce::var buildMidiDevicesListEnvelope();

// Enumerates connected MIDI input devices and broadcasts MIDI_DEVICES_LIST.
void handleMidiDevicesRequest(BridgeServer& bridge);

// Opens exactly the requested connected MIDI inputs, then broadcasts their state.
void handleMidiInputsSet(const juce::var& payload, BridgeServer& bridge);
void handleMidiDeckSelectionSet(const juce::var& payload, BridgeServer& bridge);
void handleMidiScratchSettingsSet(const juce::var& payload);
void setMidiScratchEngine(AudioEngine& engine);

// Mirrors the selected track peak to supported enabled controller channel meters.
void sendMidiSelectedTrackMeter(float peakL, float peakR, bool playing);
void sendMidiTransportPlaying(bool playing);
void sendMidiMarkerLights(bool cueActive, int markerCount);

} // namespace stemcel
