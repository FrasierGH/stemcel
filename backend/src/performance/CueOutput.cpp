#include "CueOutput.h"

#include "Log.h"

#include <algorithm>
#include <cmath>

namespace stemcel::perf
{

CueOutput::CueOutput() = default;

CueOutput::~CueOutput() { close(); }

bool CueOutput::open(const juce::String& typeName, const juce::String& name, double sourceRate, juce::String& error)
{
    close();
    for (auto* type : factory.getAvailableDeviceTypes())
    {
        if (type == nullptr || (typeName.isNotEmpty() && type->getTypeName() != typeName)) continue;
        type->scanForDevices();
        if (!type->getDeviceNames(false).contains(name)) continue;
        device.reset(type->createDevice(name, {}));
        if (device != nullptr) break;
    }
    if (device == nullptr)
    {
        error = "no output device named " + name;
        return false;
    }
    const int available = device->getOutputChannelNames().size();
    outputChannels = available >= 4 ? 4 : std::min(2, available);
    juce::BigInteger outputs;
    outputs.setRange(0, std::max(1, outputChannels), true);
    // Its own rate when it has one (a shared endpoint runs at its mix rate), else the engine's.
    const double rate = device->getCurrentSampleRate() > 0.0 ? device->getCurrentSampleRate() : sourceRate;
    error = device->open({}, outputs, rate, device->getDefaultBufferSize());
    if (error.isNotEmpty())
    {
        device.reset();
        return false;
    }
    const double actualRate = device->getCurrentSampleRate() > 0.0 ? device->getCurrentSampleRate() : rate;
    baseRatio = sourceRate > 0.0 ? sourceRate / actualRate : 1.0;
    // A little over two of either device's blocks in hand, at least ~20 ms.
    targetFill = std::max(static_cast<int>(sourceRate * 0.02), 3 * std::max(device->getCurrentBufferSizeSamples(), 512));
    written.store(0);
    readPos = 0.0;
    drift = 0.0;
    primed = false;
    std::fill(ring.begin(), ring.end(), 0.0F);
    running.store(true, std::memory_order_release);
    device->start(this);
    log::info("performance", "cue output on " + name + ": " + juce::String(outputChannels) + " outputs at "
                                 + juce::String(actualRate, 0) + " Hz (engine " + juce::String(sourceRate, 0) + " Hz)");
    return true;
}

void CueOutput::close()
{
    if (device == nullptr) return;
    running.store(false, std::memory_order_release);
    device->stop();
    device->close();
    device.reset();
    outputChannels = 0;
}

juce::String CueOutput::deviceName() const { return device != nullptr ? device->getName() : juce::String(); }

void CueOutput::push(const float* masterL, const float* masterR, const float* cueL, const float* cueR, int n) noexcept
{
    if (!running.load(std::memory_order_acquire)) return;
    auto at = written.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i, ++at)
    {
        auto* frame = &ring[static_cast<size_t>(at & (kRingFrames - 1)) * 4];
        frame[0] = masterL != nullptr ? masterL[i] : 0.0F;
        frame[1] = masterR != nullptr ? masterR[i] : 0.0F;
        frame[2] = cueL != nullptr ? cueL[i] : 0.0F;
        frame[3] = cueR != nullptr ? cueR[i] : 0.0F;
    }
    written.store(at, std::memory_order_release);
}

void CueOutput::audioDeviceIOCallbackWithContext(const float* const* /*inputs*/, int /*numInputs*/, float* const* outputs, int numOutputs, int n,
                                                 const juce::AudioIODeviceCallbackContext& /*context*/)
{
    for (int c = 0; c < numOutputs; ++c)
        if (outputs[c] != nullptr) std::fill(outputs[c], outputs[c] + n, 0.0F);
    const auto have = static_cast<double>(written.load(std::memory_order_acquire));
    if (!primed)
    {
        // Start a little behind the writer.
        if (have < targetFill) return;
        readPos = have - targetFill;
        primed = true;
    }
    double fill = have - readPos;
    if (fill < 2.0 || fill > kRingFrames - 2.0 * n)
    {
        // Lost (a device stalled): start again behind the writer.
        readPos = std::max(0.0, have - targetFill);
        fill = have - readPos;
    }
    // Hold the fill near its target by reading a hair faster or slower (±0.2%).
    drift += ((fill - targetFill) / targetFill * 0.002 - drift) * 0.01;
    const double step = baseRatio * (1.0 + std::clamp(drift, -0.002, 0.002));
    const bool fourOut = numOutputs >= 4;
    for (int i = 0; i < n; ++i)
    {
        const auto k = static_cast<juce::int64>(readPos);
        const auto f = static_cast<float>(readPos - static_cast<double>(k));
        if (static_cast<double>(k + 1) >= have) break; // ran dry: silence for the rest
        const auto* a = &ring[static_cast<size_t>(k & (kRingFrames - 1)) * 4];
        const auto* b = &ring[static_cast<size_t>((k + 1) & (kRingFrames - 1)) * 4];
        float v[4];
        for (int c = 0; c < 4; ++c) v[c] = a[c] + (b[c] - a[c]) * f;
        if (fourOut)
        {
            // Master on 1–2 (for the controller's headphone mix), cue on 3–4.
            for (int c = 0; c < 4; ++c)
                if (outputs[c] != nullptr) outputs[c][i] = v[c];
        }
        else
        {
            if (numOutputs > 0 && outputs[0] != nullptr) outputs[0][i] = v[2];
            if (numOutputs > 1 && outputs[1] != nullptr) outputs[1][i] = v[3];
        }
        readPos += step;
    }
}

} // namespace stemcel::perf
