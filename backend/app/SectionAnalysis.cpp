#include "SectionAnalysis.h"

#include "AppPaths.h"
#include "Theme.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace stemcel::native::sections
{
namespace
{
constexpr int kPhraseBars = 8;
constexpr int kCacheVersion = 1;

double percentile(std::vector<double> values, double fraction)
{
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const auto index = static_cast<size_t>(juce::jlimit(0.0, 1.0, fraction) * static_cast<double>(values.size() - 1));
    return values[index];
}

// Each feature scaled to 0..1 between the track's 10th and 95th percentiles.
void normalise(std::vector<Bar>& bars)
{
    auto scale = [&bars](double Bar::*field)
    {
        std::vector<double> values;
        for (const auto& b : bars)
            if (!std::isnan(b.*field)) values.push_back(b.*field);
        if (values.empty()) return;
        const double lo = percentile(values, 0.10), hi = percentile(values, 0.95);
        for (auto& b : bars)
            if (!std::isnan(b.*field)) b.*field = hi > lo + 1.0e-9 ? juce::jlimit(0.0, 1.0, (b.*field - lo) / (hi - lo)) : 0.5;
    };
    scale(&Bar::low);
    scale(&Bar::mid);
    scale(&Bar::high);
    // Vocal levels are absolute already (see analyseFile): a voice that runs all through stays one.
}

double energyOf(const Bar& b)
{
    return 0.5 * b.low + 0.3 * b.mid + 0.2 * b.high;
}

juce::String kindOfRekordboxLabel(const juce::String& label, const juce::String& mood)
{
    const auto l = label.toUpperCase();
    if (l.startsWith("INTRO")) return "intro";
    if (l.startsWith("OUTRO")) return "outro";
    if (l.startsWith("UP")) return "build";
    if (l.startsWith("DOWN")) return "breakdown";
    if (l.startsWith("CHORUS")) return mood == "high" ? "drop" : "chorus";
    if (l.startsWith("VERSE")) return "verse";
    if (l.startsWith("BRIDGE")) return "bridge";
    return "groove";
}

juce::String titleCase(const juce::String& label)
{
    juce::String out;
    for (const auto& word : juce::StringArray::fromTokens(label.toLowerCase(), " ", ""))
        out << (out.isEmpty() ? "" : " ") << word.substring(0, 1).toUpperCase() << word.substring(1);
    return out;
}

// RMS in dB of a band over each bar of a mono signal, with the bar grid.
std::vector<double> barLevels(const std::vector<float>& signal, double sampleRate, double barSec, double firstBarSec, int bars)
{
    std::vector<double> out(static_cast<size_t>(bars), -100.0);
    for (int i = 0; i < bars; ++i)
    {
        const auto from = static_cast<size_t>(juce::jmax(0.0, (firstBarSec + i * barSec) * sampleRate));
        const auto to = juce::jmin(signal.size(), static_cast<size_t>((firstBarSec + (i + 1) * barSec) * sampleRate));
        if (to <= from) continue;
        double sum = 0.0;
        for (auto s = from; s < to; ++s) sum += static_cast<double>(signal[s]) * signal[s];
        out[static_cast<size_t>(i)] = 10.0 * std::log10(sum / static_cast<double>(to - from) + 1.0e-12);
    }
    return out;
}

std::optional<std::vector<float>> decodeMono(const juce::File& file, double& sampleRate)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0) return std::nullopt;
    sampleRate = reader->sampleRate;
    std::vector<float> mono(static_cast<size_t>(reader->lengthInSamples), 0.0f);
    constexpr int kBlock = 1 << 16;
    juce::AudioBuffer<float> buffer(static_cast<int>(juce::jmax(1u, reader->numChannels)), kBlock);
    for (juce::int64 at = 0; at < reader->lengthInSamples; at += kBlock)
    {
        const int n = static_cast<int>(juce::jmin<juce::int64>(kBlock, reader->lengthInSamples - at));
        reader->read(&buffer, 0, n, at, true, true);
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        {
            const auto* data = buffer.getReadPointer(ch);
            for (int i = 0; i < n; ++i) mono[static_cast<size_t>(at + i)] += data[i] / static_cast<float>(buffer.getNumChannels());
        }
    }
    return mono;
}

std::vector<float> filtered(const std::vector<float>& input, double sampleRate, const std::vector<juce::dsp::IIR::Coefficients<float>::Ptr>& chain)
{
    std::vector<float> out(input);
    for (const auto& coefficients : chain)
    {
        juce::dsp::IIR::Filter<float> filter(coefficients);
        filter.reset();
        for (auto& s : out) s = filter.processSample(s);
    }
    juce::ignoreUnused(sampleRate);
    return out;
}
} // namespace

std::pair<double, double> barGrid(double bpm, double beatAnchorSec)
{
    if (bpm < 40.0 || bpm > 300.0) return {4.0, 0.0};
    const double barSec = 240.0 / bpm;
    // Bar 0 starts on the grid at or before the start of the file.
    double first = std::fmod(beatAnchorSec, barSec);
    if (first > 0.0) first -= barSec;
    return {barSec, first};
}

std::vector<Section> segment(const std::vector<Bar>& input, double barSec, double firstBarSec, double durationSec, bool vocalFromStem)
{
    std::vector<Section> out;
    if (input.empty() || barSec <= 0.0) return out;
    auto bars = input;
    normalise(bars);
    const int phrase = bars.size() < 32 ? kPhraseBars / 2 : kPhraseBars;

    // Phrases' averages.
    struct Block
    {
        int first = 0, count = 0;
        Bar mean;
    };
    std::vector<Block> blocks;
    for (int i = 0; i < static_cast<int>(bars.size()); i += phrase)
    {
        Block b{i, juce::jmin(phrase, static_cast<int>(bars.size()) - i), {}};
        b.mean.vocal = 0.0;
        bool vocalKnown = false;
        for (int k = b.first; k < b.first + b.count; ++k)
        {
            const auto& bar = bars[static_cast<size_t>(k)];
            b.mean.low += bar.low / b.count;
            b.mean.mid += bar.mid / b.count;
            b.mean.high += bar.high / b.count;
            if (!std::isnan(bar.vocal))
            {
                b.mean.vocal += bar.vocal / b.count;
                vocalKnown = true;
            }
        }
        if (!vocalKnown) b.mean.vocal = std::numeric_limits<double>::quiet_NaN();
        blocks.push_back(b);
    }

    // Neighbouring phrases of the same character make one section.
    std::vector<std::vector<Block>> groups;
    for (const auto& b : blocks)
    {
        if (!groups.empty())
        {
            const auto& last = groups.back().back();
            const bool same = std::abs(energyOf(b.mean) - energyOf(last.mean)) < 0.12 && std::abs(b.mean.low - last.mean.low) < 0.2
                              && (std::isnan(b.mean.vocal) || std::isnan(last.mean.vocal) || (b.mean.vocal > 0.35) == (last.mean.vocal > 0.35));
            if (same)
            {
                groups.back().push_back(b);
                continue;
            }
        }
        groups.push_back({b});
    }

    std::vector<double> energies;
    for (const auto& b : blocks) energies.push_back(energyOf(b.mean));
    const double median = percentile(energies, 0.5);
    for (size_t g = 0; g < groups.size(); ++g)
    {
        const auto& group = groups[g];
        Bar mean;
        mean.vocal = 0.0;
        int count = 0, vocalCount = 0;
        for (const auto& b : group)
        {
            mean.low += b.mean.low * b.count;
            mean.mid += b.mean.mid * b.count;
            mean.high += b.mean.high * b.count;
            if (!std::isnan(b.mean.vocal))
            {
                mean.vocal += b.mean.vocal * b.count;
                vocalCount += b.count;
            }
            count += b.count;
        }
        mean.low /= count;
        mean.mid /= count;
        mean.high /= count;
        mean.vocal = vocalCount > 0 ? mean.vocal / vocalCount : std::numeric_limits<double>::quiet_NaN();

        Section s;
        s.startSec = juce::jmax(0.0, firstBarSec + group.front().first * barSec);
        s.endSec = juce::jmin(durationSec, firstBarSec + (group.back().first + group.back().count) * barSec);
        if (g + 1 == groups.size()) s.endSec = durationSec;
        s.energy = energyOf(mean);
        s.kick = mean.low > 0.45;
        s.vocal = !std::isnan(mean.vocal) && mean.vocal > 0.35;
        s.vocalEstimated = s.vocal && !vocalFromStem;
        s.source = "stemcel";
        const bool first = g == 0, last = g + 1 == groups.size();
        const bool nextIsBig = g + 1 < groups.size()
                               && energyOf(groups[g + 1].front().mean) > s.energy + 0.1 && groups[g + 1].front().mean.low > 0.45;
        const bool rising = energyOf(group.back().mean) > energyOf(group.front().mean) + 0.08;
        if (first && s.energy < median) s.kind = "intro", s.label = "Intro";
        else if (last && s.energy < median) s.kind = "outro", s.label = "Outro";
        else if (s.kick && s.energy >= median + 0.05) s.kind = "drop", s.label = "Drop";
        // No kick between the intro and the outro: a breakdown, or a build when it rises into a drop.
        else if (!s.kick) (nextIsBig && rising) ? (s.kind = "build", s.label = "Build") : (s.kind = "breakdown", s.label = "Breakdown");
        else if (nextIsBig || rising) s.kind = "build", s.label = "Build";
        else s.kind = "groove", s.label = "Groove";
        if (s.endSec > s.startSec) out.push_back(s);
    }
    return out;
}

std::vector<Section> fromRekordbox(const RekordboxTrackDetail& detail, double bpm, double beatAnchorSec, double durationSec)
{
    // Per-second band levels, to give each section its energy and kick.
    auto bandMean = [&detail](double from, double to)
    {
        std::array<double, 3> sum{};
        int n = 0;
        const int a = juce::jmax(0, static_cast<int>(from * detail.bandsPerSecond));
        const int b = juce::jmin(static_cast<int>(detail.bands.size()), static_cast<int>(to * detail.bandsPerSecond));
        for (int i = a; i < b; ++i, ++n)
            for (int k = 0; k < 3; ++k) sum[static_cast<size_t>(k)] += detail.bands[static_cast<size_t>(i)][static_cast<size_t>(k)];
        if (n > 0)
            for (auto& v : sum) v /= n;
        return std::make_pair(sum, n);
    };
    std::vector<Section> out;
    if (!detail.phrases.empty())
    {
        std::vector<double> lows;
        for (const auto& band : detail.bands) lows.push_back(band[0]);
        const double kickLevel = percentile(lows, 0.6);
        for (const auto& p : detail.phrases)
        {
            if (p.startMs < 0.0 || p.endMs <= p.startMs) continue;
            Section s;
            s.startSec = p.startMs / 1000.0;
            s.endSec = p.endMs / 1000.0;
            s.label = titleCase(p.label);
            s.kind = kindOfRekordboxLabel(p.label, detail.mood);
            s.source = "rekordbox";
            if (const auto [mean, n] = bandMean(s.startSec, s.endSec); n > 0)
            {
                s.energy = juce::jlimit(0.0, 1.0, 0.5 * mean[0] + 0.3 * mean[1] + 0.2 * mean[2]);
                s.kick = mean[0] >= kickLevel && kickLevel > 0.05;
            }
            out.push_back(s);
        }
        return out;
    }
    if (detail.bands.empty()) return out;
    // No phrases: the 3-band waveform, segmented as Stemcel's own analysis would.
    const auto [barSec, first] = barGrid(bpm, beatAnchorSec);
    std::vector<Bar> bars;
    for (double t = first; t < durationSec; t += barSec)
    {
        const auto [mean, n] = bandMean(t, t + barSec);
        if (n == 0) continue;
        bars.push_back({mean[0], mean[1], mean[2], std::numeric_limits<double>::quiet_NaN()});
    }
    auto list = segment(bars, barSec, first, durationSec, false);
    for (auto& s : list) s.source = "rekordbox";
    return list;
}

std::optional<std::vector<Section>> analyseFile(const juce::File& audio, double bpm, double beatAnchorSec, const juce::File& vocalStem)
{
    double rate = 0.0;
    const auto mono = decodeMono(audio, rate);
    if (!mono.has_value() || rate <= 0.0) return std::nullopt;
    const double durationSec = static_cast<double>(mono->size()) / rate;
    const auto [barSec, first] = barGrid(bpm, beatAnchorSec);
    const int bars = juce::jmax(1, static_cast<int>(std::ceil((durationSec - first) / barSec)));
    using C = juce::dsp::IIR::Coefficients<float>;
    const auto fr = static_cast<float>(rate);
    // Low: the kick and bass. Mid: where voices and leads sit. High: hats and air.
    const auto low = barLevels(filtered(*mono, rate, {C::makeLowPass(rate, 120.0f), C::makeLowPass(rate, 120.0f)}), rate, barSec, first, bars);
    const auto mid = barLevels(filtered(*mono, rate, {C::makeHighPass(rate, 300.0f), C::makeLowPass(rate, juce::jmin(3000.0f, fr * 0.45f))}),
                               rate, barSec, first, bars);
    const auto high = barLevels(filtered(*mono, rate, {C::makeHighPass(rate, juce::jmin(5000.0f, fr * 0.45f))}), rate, barSec, first, bars);
    std::vector<double> vocal(static_cast<size_t>(bars), std::numeric_limits<double>::quiet_NaN());
    bool fromStem = false;
    if (vocalStem.existsAsFile())
    {
        double stemRate = 0.0;
        if (const auto stem = decodeMono(vocalStem, stemRate); stem.has_value() && stemRate > 0.0)
        {
            vocal = barLevels(*stem, stemRate, barSec, first, bars);
            // The stem's level, -45 dBFS (no voice) to -30 dBFS and up (a voice), as 0..1.
            for (auto& v : vocal) v = juce::jlimit(0.0, 1.0, (v + 45.0) / 15.0);
            fromStem = true;
        }
    }
    std::vector<Bar> list;
    for (int i = 0; i < bars; ++i)
    {
        const auto at = static_cast<size_t>(i);
        // Vocals only from a Vocals stem: guessing them from the mix's mid band called pads
        // and synths voices.
        list.push_back({low[at], mid[at], high[at], vocal[at]});
    }
    return segment(list, barSec, first, durationSec, fromStem);
}

juce::var toVar(const std::vector<Section>& list)
{
    juce::Array<juce::var> out;
    for (const auto& s : list)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("startSec", s.startSec);
        o->setProperty("endSec", s.endSec);
        o->setProperty("label", s.label);
        o->setProperty("kind", s.kind);
        o->setProperty("energy", s.energy);
        o->setProperty("kick", s.kick);
        o->setProperty("vocal", s.vocal);
        if (s.vocalEstimated) o->setProperty("vocalEstimated", true);
        o->setProperty("source", s.source);
        out.add(juce::var(o));
    }
    return out;
}

std::vector<Section> fromVar(const juce::var& value)
{
    std::vector<Section> out;
    if (const auto* list = value.getArray())
        for (const auto& v : *list)
        {
            Section s;
            s.startSec = v.getProperty("startSec", 0.0);
            s.endSec = v.getProperty("endSec", 0.0);
            s.label = v.getProperty("label", {}).toString();
            s.kind = v.getProperty("kind", {}).toString();
            s.energy = v.getProperty("energy", 0.0);
            s.kick = v.getProperty("kick", false);
            s.vocal = v.getProperty("vocal", false);
            s.vocalEstimated = v.getProperty("vocalEstimated", false);
            s.source = v.getProperty("source", {}).toString();
            if (s.endSec > s.startSec) out.push_back(s);
        }
    return out;
}

std::vector<Section> of(const LibraryItemModel& item)
{
    return fromVar(item.raw.getProperty("sections", {}));
}

juce::String caption(const Section& s)
{
    return s.label + (s.vocal ? (s.vocalEstimated ? " + vocal?" : " + vocal") : "");
}

juce::Colour colourFor(const juce::String& kind)
{
    if (kind == "drop" || kind == "chorus") return juce::Colour(0xffef4444);
    if (kind == "build") return juce::Colour(0xfff59e0b);
    if (kind == "breakdown" || kind == "bridge") return juce::Colour(0xff6366f1);
    if (kind == "intro" || kind == "outro") return juce::Colour(0xff64748b);
    if (kind == "verse") return juce::Colour(0xff14b8a6);
    return juce::Colour(0xff10b981);
}

juce::File cacheFileFor(const juce::File& audio, double bpm, double beatAnchorSec, bool withVocalStem)
{
    juce::MemoryBlock head;
    if (juce::FileInputStream in(audio); in.openedOk()) in.readIntoMemoryBlock(head, 64 * 1024);
    juce::MemoryOutputStream key;
    key << audio.getFileName() << "|" << audio.getSize() << "|" << audio.getLastModificationTime().toMilliseconds() << "|"
        << juce::String(bpm, 3) << "|" << juce::String(beatAnchorSec, 3) << "|" << (withVocalStem ? "stem" : "mix") << "|v"
        << kCacheVersion << "|";
    key.write(head.getData(), head.getSize());
    const auto hash = juce::SHA256(key.getData(), key.getDataSize()).toHexString();
    return juce::File::getSpecialLocation(juce::File::userHomeDirectory)
        .getChildFile(appFolderName())
        .getChildFile("Analysis")
        .getChildFile(hash.substring(0, 32) + ".json");
}
} // namespace stemcel::native::sections
