#include "KeyDetection.h"

#include <array>
#include <cmath>
#include <numeric>

namespace stemcel::native
{
namespace
{
const std::array<const char*, 12> kNoteNames{"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
constexpr std::array<double, 12> kMajorProfile{6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
constexpr std::array<double, 12> kMinorProfile{6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};
constexpr double kMinConfidenceGap = 0.002;
constexpr double kTargetSampleRate = 11025.0;
constexpr double kMaxSeconds = 120.0;

double goertzelMagnitude(const float* samples, int length, double sampleRate, double frequency)
{
    const double omega = 2.0 * juce::MathConstants<double>::pi * frequency / sampleRate;
    const double coeff = 2.0 * std::cos(omega);
    double s1 = 0.0;
    double s2 = 0.0;
    for (int i = 0; i < length; ++i)
    {
        const double window = 0.5 - 0.5 * std::cos(2.0 * juce::MathConstants<double>::pi * i / (length - 1));
        const double s0 = samples[i] * window + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - coeff * s1 * s2));
}

double correlation(const std::array<double, 12>& a, const std::array<double, 12>& b)
{
    const double meanA = std::accumulate(a.begin(), a.end(), 0.0) / 12.0;
    const double meanB = std::accumulate(b.begin(), b.end(), 0.0) / 12.0;
    double num = 0.0, denA = 0.0, denB = 0.0;
    for (size_t i = 0; i < 12; ++i)
    {
        const double da = a[i] - meanA;
        const double db = b[i] - meanB;
        num += da * db;
        denA += da * da;
        denB += db * db;
    }
    const double den = std::sqrt(denA * denB);
    return den > 0.0 ? num / den : 0.0;
}

std::array<double, 12> rotated(const std::array<double, 12>& profile, int root)
{
    std::array<double, 12> out{};
    for (int i = 0; i < 12; ++i) out[static_cast<size_t>((i + root) % 12)] = profile[static_cast<size_t>(i)];
    return out;
}

// Mono samples already at the detector's rate.
juce::String detectFromMono(const std::vector<float>& mono, double sampleRate)
{
    if (mono.size() < 2048 || sampleRate <= 0.0) return {};
    std::array<double, 12> chroma{};
    constexpr int windowSize = 4096;
    for (size_t start = 0; start + windowSize <= mono.size(); start += windowSize)
        for (int midi = 36; midi <= 83; ++midi)
        {
            const double freq = 440.0 * std::pow(2.0, (midi - 69) / 12.0);
            chroma[static_cast<size_t>(midi % 12)] += goertzelMagnitude(mono.data() + start, windowSize, sampleRate, freq);
        }
    const double total = std::accumulate(chroma.begin(), chroma.end(), 0.0);
    if (total <= 0.0) return {};
    for (auto& c : chroma) c /= total;

    double best = -2.0, second = -2.0;
    juce::String bestKey;
    for (int root = 0; root < 12; ++root)
    {
        for (int minor = 0; minor < 2; ++minor)
        {
            const double score = correlation(chroma, rotated(minor ? kMinorProfile : kMajorProfile, root));
            if (score > best)
            {
                second = best;
                best = score;
                bestKey = juce::String(kNoteNames[static_cast<size_t>(root)]) + (minor ? " minor" : " major");
            }
            else if (score > second)
            {
                second = score;
            }
        }
    }
    return best - second < kMinConfidenceGap ? juce::String() : bestKey;
}

int noteIndex(juce::String name)
{
    name = name.trim();
    if (name.isEmpty()) return -1;
    static const juce::String letters = "C D EF G A B";
    const int base = letters.indexOfChar(juce::CharacterFunctions::toUpperCase(name[0]));
    if (base < 0) return -1;
    int index = base;
    for (int i = 1; i < name.length(); ++i)
    {
        const auto ch = name[i];
        if (ch == '#' || ch == 0x266F) ++index;
        else if (ch == 'b' || ch == 0x266D) --index;
        else break;
    }
    return (index + 12) % 12;
}
} // namespace

juce::String detectMusicalKey(const juce::AudioBuffer<float>& audio, double sampleRate)
{
    if (audio.getNumChannels() == 0 || sampleRate <= 0.0) return {};
    const int stride = juce::jmax(1, static_cast<int>(std::floor(sampleRate / kTargetSampleRate)));
    const double outRate = sampleRate / stride;
    const int count = juce::jmin(audio.getNumSamples() / stride, static_cast<int>(outRate * kMaxSeconds));
    std::vector<float> mono(static_cast<size_t>(juce::jmax(0, count)));
    for (int i = 0; i < count; ++i)
    {
        float sum = 0.0f;
        for (int ch = 0; ch < audio.getNumChannels(); ++ch) sum += audio.getSample(ch, i * stride);
        mono[static_cast<size_t>(i)] = sum / static_cast<float>(audio.getNumChannels());
    }
    return detectFromMono(mono, outRate);
}

juce::String detectMusicalKey(juce::AudioFormatReader& reader)
{
    const double rate = reader.sampleRate;
    const int channels = static_cast<int>(reader.numChannels);
    if (rate <= 0.0 || channels <= 0) return {};
    const int stride = juce::jmax(1, static_cast<int>(std::floor(rate / kTargetSampleRate)));
    const double outRate = rate / stride;
    const auto wanted = juce::jmin<juce::int64>(reader.lengthInSamples, static_cast<juce::int64>(rate * kMaxSeconds));

    std::vector<float> mono;
    mono.reserve(static_cast<size_t>(wanted / stride + 1));
    constexpr int block = 65536;
    juce::AudioBuffer<float> buffer(channels, block);
    juce::int64 phase = 0;
    for (juce::int64 pos = 0; pos < wanted; pos += block)
    {
        const int n = static_cast<int>(juce::jmin<juce::int64>(block, wanted - pos));
        if (!reader.read(&buffer, 0, n, pos, true, true)) break;
        for (int i = static_cast<int>(phase); i < n; i += stride)
        {
            float sum = 0.0f;
            for (int ch = 0; ch < channels; ++ch) sum += buffer.getSample(ch, i);
            mono.push_back(sum / static_cast<float>(channels));
        }
        // Next block's first decimated sample keeps the global stride grid.
        phase = (stride - ((pos + n) % stride)) % stride;
    }
    return detectFromMono(mono, outRate);
}

juce::String normaliseKeyName(const juce::String& raw)
{
    auto key = raw.trim();
    if (key.isEmpty()) return {};

    // Camelot (1A–12B) and Open Key (1m–12d).
    if (juce::CharacterFunctions::isDigit(key[0]))
    {
        const int number = key.getIntValue();
        const auto letter = juce::CharacterFunctions::toUpperCase(key.getLastCharacter());
        if (number >= 1 && number <= 12)
        {
            // Camelot 8B = C major, 8A = A minor; each step is a fifth.
            const bool camelotMinor = letter == 'A';
            const bool camelotMajor = letter == 'B';
            const bool openMinor = letter == 'M';
            const bool openMajor = letter == 'D';
            if (camelotMinor || camelotMajor)
            {
                const int majorRoot = ((number - 8) * 7 % 12 + 12) % 12;
                return camelotMajor ? juce::String(kNoteNames[static_cast<size_t>(majorRoot)]) + " major"
                                    : juce::String(kNoteNames[static_cast<size_t>((majorRoot + 9) % 12)]) + " minor";
            }
            if (openMinor || openMajor)
            {
                // Open Key 1d = C major, 1m = A minor.
                const int majorRoot = ((number - 1) * 7) % 12;
                return openMajor ? juce::String(kNoteNames[static_cast<size_t>(majorRoot)]) + " major"
                                 : juce::String(kNoteNames[static_cast<size_t>((majorRoot + 9) % 12)]) + " minor";
            }
        }
        return {};
    }

    const int root = noteIndex(key);
    if (root < 0) return {};
    // Skip the note name and accidentals; the rest says major or minor.
    int i = 1;
    while (i < key.length() && (key[i] == '#' || key[i] == 'b' || key[i] == 0x266F || key[i] == 0x266D)) ++i;
    const auto rest = key.substring(i).trim().toLowerCase();
    const bool minor = rest.startsWith("m") && !rest.startsWith("maj");
    return juce::String(kNoteNames[static_cast<size_t>(root)]) + (minor ? " minor" : " major");
}

} // namespace stemcel::native
