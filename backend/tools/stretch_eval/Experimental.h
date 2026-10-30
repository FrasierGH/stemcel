#pragma once

// Experimental offline stretches, for listening against élastique before anything goes into
// the engine (see StretchEval.cpp, --experimental):
//   pro-long  Signalsmith Stretch with a 200 ms window (the default is 120 ms): finer
//             frequency resolution, so close partials beat less (less "metallic").
//   hybrid    Harmonic/percussive split (Driedger & Müller, "Improving Time-Scale
//             Modification of Music Signals Using Harmonic-Percussive Separation", 2014):
//             the sustained part through the long-window stretch, the percussive part
//             through plain overlap-add with a short window, so tones stay smooth and hits
//             stay sharp, the trade-off one window size cannot make.

#include <signalsmith-stretch/signalsmith-stretch.h>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace experimental
{

// Signalsmith Stretch over a whole buffer with the given window and hop (seconds).
inline juce::AudioBuffer<float> signalsmith(const juce::AudioBuffer<float>& in, double sampleRate, double speed,
                                            double blockSeconds, double intervalSeconds)
{
    const int channels = in.getNumChannels();
    const int outLength = static_cast<int>(std::ceil(in.getNumSamples() / speed));
    juce::AudioBuffer<float> out(channels, outLength);
    signalsmith::stretch::SignalsmithStretch<float> stretch;
    stretch.configure(channels, static_cast<int>(blockSeconds * sampleRate), static_cast<int>(intervalSeconds * sampleRate));
    std::vector<const float*> ins(static_cast<size_t>(channels));
    std::vector<float*> outs(static_cast<size_t>(channels));
    for (int c = 0; c < channels; ++c)
    {
        ins[static_cast<size_t>(c)] = in.getReadPointer(c);
        outs[static_cast<size_t>(c)] = out.getWritePointer(c);
    }
    stretch.exact(ins.data(), in.getNumSamples(), outs.data(), outLength);
    return out;
}

// Splits `in` into its harmonic and percussive parts (median-filtered spectrogram, soft
// masks from the channels' summed magnitude, so the stereo image stays together).
inline void splitHarmonicPercussive(const juce::AudioBuffer<float>& in, juce::AudioBuffer<float>& harmonic,
                                    juce::AudioBuffer<float>& percussive)
{
    constexpr int kOrder = 11, kN = 1 << kOrder, kHop = kN / 4, kBins = kN / 2 + 1;
    constexpr int kTimeMedian = 17, kFreqMedian = 17;
    const int channels = in.getNumChannels();
    const int length = in.getNumSamples();
    const int frames = (length + kN) / kHop + 1;
    juce::dsp::FFT fft(kOrder);
    std::vector<float> window(kN);
    for (int i = 0; i < kN; ++i) window[static_cast<size_t>(i)] = 0.5f - 0.5f * std::cos(2.0f * juce::MathConstants<float>::pi * i / kN);

    // STFT per channel (frames start kN before the signal so the edges are covered).
    std::vector<std::vector<std::vector<std::complex<float>>>> spec(
        static_cast<size_t>(channels), std::vector<std::vector<std::complex<float>>>(static_cast<size_t>(frames), std::vector<std::complex<float>>(kBins)));
    std::vector<std::complex<float>> buf(kN), res(kN);
    for (int c = 0; c < channels; ++c)
        for (int f = 0; f < frames; ++f)
        {
            const int start = f * kHop - kN;
            for (int i = 0; i < kN; ++i)
            {
                const int at = start + i;
                buf[static_cast<size_t>(i)] = at >= 0 && at < length ? in.getSample(c, at) * window[static_cast<size_t>(i)] : 0.0f;
            }
            fft.perform(buf.data(), res.data(), false);
            std::copy(res.begin(), res.begin() + kBins, spec[static_cast<size_t>(c)][static_cast<size_t>(f)].begin());
        }

    std::vector<std::vector<float>> mag(static_cast<size_t>(frames), std::vector<float>(kBins, 0.0f));
    for (int c = 0; c < channels; ++c)
        for (int f = 0; f < frames; ++f)
            for (int k = 0; k < kBins; ++k) mag[static_cast<size_t>(f)][static_cast<size_t>(k)] += std::abs(spec[static_cast<size_t>(c)][static_cast<size_t>(f)][static_cast<size_t>(k)]);

    std::vector<float> tmp;
    auto median = [&tmp](std::vector<float>& v) {
        tmp = v;
        std::nth_element(tmp.begin(), tmp.begin() + static_cast<long>(tmp.size() / 2), tmp.end());
        return tmp[tmp.size() / 2];
    };
    std::vector<std::vector<float>> maskH(static_cast<size_t>(frames), std::vector<float>(kBins));
    std::vector<float> window1;
    for (int f = 0; f < frames; ++f)
        for (int k = 0; k < kBins; ++k)
        {
            window1.clear();
            for (int d = -kTimeMedian / 2; d <= kTimeMedian / 2; ++d)
                window1.push_back(mag[static_cast<size_t>(juce::jlimit(0, frames - 1, f + d))][static_cast<size_t>(k)]);
            const float h = median(window1);
            window1.clear();
            for (int d = -kFreqMedian / 2; d <= kFreqMedian / 2; ++d)
                window1.push_back(mag[static_cast<size_t>(f)][static_cast<size_t>(juce::jlimit(0, kBins - 1, k + d))]);
            const float p = median(window1);
            maskH[static_cast<size_t>(f)][static_cast<size_t>(k)] = (h * h) / (h * h + p * p + 1.0e-12f);
        }

    // Masked inverse STFTs, overlap-added (Hann analysis and synthesis at 75%: sum of w^2 = 1.5).
    harmonic.setSize(channels, length);
    percussive.setSize(channels, length);
    harmonic.clear();
    percussive.clear();
    for (int c = 0; c < channels; ++c)
        for (int part = 0; part < 2; ++part)
        {
            auto& dest = part == 0 ? harmonic : percussive;
            for (int f = 0; f < frames; ++f)
            {
                for (int k = 0; k < kBins; ++k)
                {
                    const float m = part == 0 ? maskH[static_cast<size_t>(f)][static_cast<size_t>(k)] : 1.0f - maskH[static_cast<size_t>(f)][static_cast<size_t>(k)];
                    buf[static_cast<size_t>(k)] = spec[static_cast<size_t>(c)][static_cast<size_t>(f)][static_cast<size_t>(k)] * m;
                }
                for (int k = 1; k < kN / 2; ++k) buf[static_cast<size_t>(kN - k)] = std::conj(buf[static_cast<size_t>(k)]);
                fft.perform(buf.data(), res.data(), true);
                const int start = f * kHop - kN;
                for (int i = 0; i < kN; ++i)
                {
                    const int at = start + i;
                    if (at >= 0 && at < length)
                        dest.addSample(c, at, res[static_cast<size_t>(i)].real() * window[static_cast<size_t>(i)] / 1.5f);
                }
            }
        }
}

// Plain overlap-add with a short Hann window: no phase changes at all, so a percussive hit
// keeps its shape (its own short decay simply repeats or drops a little).
inline juce::AudioBuffer<float> overlapAdd(const juce::AudioBuffer<float>& in, double speed)
{
    constexpr int kN = 512, kHop = kN / 2;
    const int channels = in.getNumChannels();
    const int outLength = static_cast<int>(std::ceil(in.getNumSamples() / speed));
    juce::AudioBuffer<float> out(channels, outLength);
    out.clear();
    for (int m = 0; m * kHop < outLength + kN; ++m)
    {
        const int from = static_cast<int>(std::lround(m * kHop * speed)) - kN / 2;
        const int to = m * kHop - kN / 2;
        for (int i = 0; i < kN; ++i)
        {
            const float w = 0.5f - 0.5f * std::cos(2.0f * juce::MathConstants<float>::pi * i / kN); // sums to 1 at 50%
            const int src = from + i, dst = to + i;
            if (src < 0 || src >= in.getNumSamples() || dst < 0 || dst >= outLength) continue;
            for (int c = 0; c < channels; ++c) out.addSample(c, dst, in.getSample(c, src) * w);
        }
    }
    return out;
}

inline juce::AudioBuffer<float> hybrid(const juce::AudioBuffer<float>& in, double sampleRate, double speed)
{
    juce::AudioBuffer<float> harmonic, percussive;
    splitHarmonicPercussive(in, harmonic, percussive);
    auto out = signalsmith(harmonic, sampleRate, speed, 0.2, 0.025);
    const auto perc = overlapAdd(percussive, speed);
    for (int c = 0; c < out.getNumChannels(); ++c)
        out.addFrom(c, 0, perc, c, 0, juce::jmin(out.getNumSamples(), perc.getNumSamples()));
    return out;
}

} // namespace experimental
