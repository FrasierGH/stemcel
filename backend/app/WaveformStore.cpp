#include "WaveformStore.h"

#include "PeaksCache.h"

#include <juce_graphics/juce_graphics.h>

namespace stemcel::native
{

WaveformStore::WaveformStore(const PeaksCache& peaksCache) : cache(peaksCache)
{
    formats.registerBasicFormats();
}

WaveformStore::~WaveformStore()
{
    alive->store(false);
    pool.removeAllJobs(true, 5000);
}

std::shared_ptr<const waveform::PeaksResult> WaveformStore::get(const juce::String& filePath)
{
    if (filePath.isEmpty()) return nullptr;
    if (const auto it = ready.find(filePath); it != ready.end()) return it->second;
    if (pending.count(filePath) != 0) return nullptr;
    pending[filePath] = true;

    pool.addJob(
        [this, filePath, flag = alive]
        {
            const juce::File file(filePath);
            auto result = std::make_shared<waveform::PeaksResult>(
                cache.tryLoad(file, waveform::kDefaultPeaksPerSecond));
            if (result->peaks.empty() && file.existsAsFile())
            {
                {
                    const std::scoped_lock lock(formatMutex);
                    *result = waveform::computePeaks(file, formats, waveform::kDefaultPeaksPerSecond);
                }
                if (!result->peaks.empty()) cache.store(file, *result);
            }
            juce::MessageManager::callAsync(
                [this, flag, filePath, result]
                {
                    if (flag->load()) loaded(filePath, result);
                });
        });
    return nullptr;
}

void WaveformStore::loaded(const juce::String& filePath, std::shared_ptr<const waveform::PeaksResult> peaks)
{
    pending.erase(filePath);
    ready[filePath] = std::move(peaks);
    sendChangeMessage();
}

void drawPeaks(juce::Graphics& g, const waveform::PeaksResult& peaks, juce::Rectangle<float> area,
               double sourceStartMs, double sourceLengthMs, bool reversed, juce::Colour colour, int lane)
{
    const int buckets = peaks.bucketsPerLane();
    if (lane < 0 || lane >= peaks.laneCount) lane = 0;
    const size_t base = static_cast<size_t>(lane) * static_cast<size_t>(buckets) * 2U;
    if (buckets <= 0 || area.getWidth() < 1.0f || sourceLengthMs <= 0.0) return;

    const double bucketsPerMs = static_cast<double>(peaks.peaksPerSecond) / 1000.0;
    const float midY = area.getCentreY();
    const float halfH = area.getHeight() * 0.5f;
    const int width = static_cast<int>(area.getWidth());
    const double msPerPixel = sourceLengthMs / static_cast<double>(width);

    juce::Path path;
    std::vector<float> tops(static_cast<size_t>(width));
    std::vector<float> bottoms(static_cast<size_t>(width));
    for (int x = 0; x < width; ++x)
    {
        const int px = reversed ? (width - 1 - x) : x;
        const double fromMs = sourceStartMs + msPerPixel * px;
        const double toMs = fromMs + msPerPixel;
        int b0 = juce::jlimit(0, buckets - 1, static_cast<int>(fromMs * bucketsPerMs));
        int b1 = juce::jlimit(0, buckets - 1, static_cast<int>(toMs * bucketsPerMs));
        if (b1 < b0) std::swap(b0, b1);
        float lo = 0.0f;
        float hi = 0.0f;
        for (int b = b0; b <= b1; ++b)
        {
            lo = juce::jmin(lo, peaks.peaks[base + static_cast<size_t>(b) * 2U]);
            hi = juce::jmax(hi, peaks.peaks[base + static_cast<size_t>(b) * 2U + 1U]);
        }
        tops[static_cast<size_t>(x)] = midY - juce::jlimit(0.0f, 1.0f, hi) * halfH;
        bottoms[static_cast<size_t>(x)] = midY - juce::jlimit(-1.0f, 0.0f, lo) * halfH;
    }

    path.startNewSubPath(area.getX(), tops[0]);
    for (int x = 1; x < width; ++x) path.lineTo(area.getX() + static_cast<float>(x), tops[static_cast<size_t>(x)]);
    for (int x = width - 1; x >= 0; --x)
        path.lineTo(area.getX() + static_cast<float>(x), bottoms[static_cast<size_t>(x)]);
    path.closeSubPath();
    g.setColour(colour);
    g.fillPath(path);
}

} // namespace stemcel::native
