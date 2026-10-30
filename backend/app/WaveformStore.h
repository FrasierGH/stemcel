#pragma once

#include "Waveform.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_graphics/juce_graphics.h>
#include <juce_events/juce_events.h>

#include <map>
#include <memory>
#include <mutex>

namespace stemcel
{
class PeaksCache;
}

namespace stemcel::native
{

// Waveform peaks for the timeline and library, keyed by audio file. Served from the engine's
// on-disk peaks cache when present, otherwise computed on a worker thread (and written to
// that cache, so the Electron app reuses them too). Broadcasts a change when peaks arrive.
class WaveformStore final : public juce::ChangeBroadcaster
{
  public:
    explicit WaveformStore(const PeaksCache& cache);
    ~WaveformStore() override;

    // The peaks for `file`, or null while they load (a load is started on first request).
    std::shared_ptr<const waveform::PeaksResult> get(const juce::String& filePath);

  private:
    void loaded(const juce::String& filePath, std::shared_ptr<const waveform::PeaksResult> peaks);

    const PeaksCache& cache;
    juce::ThreadPool pool{2};
    std::mutex formatMutex;
    juce::AudioFormatManager formats;
    std::map<juce::String, std::shared_ptr<const waveform::PeaksResult>> ready;
    std::map<juce::String, bool> pending;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
};

// Draws one lane of peaks (lane 0 = mono summary) for the source window
// [sourceStartMs, sourceStartMs + sourceLengthMs) into `area`.
// Lane 0 is the summary; stereo sources add 1 (left) and 2 (right).
void drawPeaks(juce::Graphics& g, const waveform::PeaksResult& peaks, juce::Rectangle<float> area,
               double sourceStartMs, double sourceLengthMs, bool reversed, juce::Colour colour, int lane = 0);

} // namespace stemcel::native
