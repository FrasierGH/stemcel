#include "TempoDrift.h"

#include "BpmAnalysisHelpers.h"
#include "BpmAudioLoader.h"
#include "BpmDetector.h"
#include "PercussiveEmphasis.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace stemcel
{
namespace
{
constexpr double kAnalysisRate = 44100.0;
constexpr int kOdfHop = 256;                  // ~5.8 ms, as the detector's refinement
constexpr double kOdfGroupDelayFrames = 0.53; // the onset function's measured lag (BpmDetector)
constexpr double kMaxSeconds = 1800.0;        // a half-hour mix at most
constexpr double kTightness = 100.0;          // how strongly a beat keeps to the main period
constexpr double kMinStep = 0.8, kMaxStep = 1.25; // a beat may come 0.8..1.25 periods after the last

double percentile(std::vector<double> values, double p)
{
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const auto i = static_cast<size_t>(juce::jlimit(0.0, 1.0, p) * static_cast<double>(values.size() - 1) + 0.5);
    return values[std::min(i, values.size() - 1)];
}
} // namespace

TempoDriftResult analyseTempoDriftSamples(const std::vector<float>& mono, double sampleRate, double bpmHint)
{
    TempoDriftResult result;
    if (mono.empty() || !(bpmHint > 0.0) || !(sampleRate > 0.0)) return result;

    const auto emphasised = emphasisePercussiveContent(mono, sampleRate);
    auto odf = bpm_detail::computeOdf(emphasised, kOdfHop);
    const double envRate = sampleRate / static_cast<double>(kOdfHop);
    const double period = 60.0 / bpmHint * envRate; // frames per beat at the main tempo
    odf = subtractMovingMedianFloor(odf, envRate, 60.0 / bpmHint);
    const int n = static_cast<int>(odf.size());
    if (n < static_cast<int>(period * 8.0)) return result;

    // Scale the onset strength so the tempo penalty means the same on quiet and loud tracks.
    double mean = 0.0, sq = 0.0;
    for (const double v : odf)
    {
        mean += v;
        sq += v * v;
    }
    mean /= n;
    const double sd = std::sqrt(juce::jmax(1.0e-12, sq / n - mean * mean));
    for (auto& v : odf) v = v / sd;

    // Dynamic programming (Ellis 2007): each frame's best score is its onset strength plus
    // the best earlier beat 0.8..1.25 periods back, less a cost for straying from the period.
    const int minBack = juce::jmax(1, static_cast<int>(std::floor(period * kMinStep)));
    const int maxBack = static_cast<int>(std::ceil(period * kMaxStep));
    std::vector<double> score(static_cast<size_t>(n), 0.0);
    std::vector<int> back(static_cast<size_t>(n), -1);
    for (int t = 0; t < n; ++t)
    {
        double best = 0.0;
        int from = -1;
        for (int d = minBack; d <= maxBack && t - d >= 0; ++d)
        {
            const double r = std::log(static_cast<double>(d) / period);
            const double s = score[static_cast<size_t>(t - d)] - kTightness * r * r;
            if (from < 0 || s > best)
            {
                best = s;
                from = t - d;
            }
        }
        // Always linked back (through quiet stretches too), so the chain spans the recording.
        score[static_cast<size_t>(t)] = odf[static_cast<size_t>(t)] + (from >= 0 ? best : 0.0);
        back[static_cast<size_t>(t)] = from;
    }
    // The chain ends at the best-scoring frame in the last period, and is read back from there.
    int t = n - 1;
    for (int i = juce::jmax(0, n - static_cast<int>(period)); i < n; ++i)
        if (score[static_cast<size_t>(i)] > score[static_cast<size_t>(t)]) t = i;
    std::vector<int> frames;
    while (t >= 0)
    {
        frames.push_back(t);
        t = back[static_cast<size_t>(t)];
    }
    std::reverse(frames.begin(), frames.end());
    // The chain runs on through silence before the music starts and after it stops; those
    // beats are placeholders, not the recording's, so they go.
    {
        std::vector<double> strengths;
        for (const int f : frames) strengths.push_back(odf[static_cast<size_t>(f)]);
        const double floor = 0.5 * percentile(strengths, 0.5);
        const auto strong = [&](int f) { return odf[static_cast<size_t>(f)] >= floor; };
        const auto first = std::find_if(frames.begin(), frames.end(), strong);
        const auto last = std::find_if(frames.rbegin(), frames.rend(), strong).base();
        frames = first < last ? std::vector<int>(first, last) : std::vector<int>{};
    }
    if (frames.size() < 16) return result;
    for (const int f : frames) result.beatsSec.push_back((static_cast<double>(f) - kOdfGroupDelayFrames) / envRate);

    // A tempo per bar (four beats), at the bar's middle. A bar far off both its neighbours (a
    // missed or doubled beat) takes their median; real drift, even a sharp turn, is kept.
    std::vector<TempoDriftResult::Point> bars;
    for (size_t i = 0; i + 4 < result.beatsSec.size(); i += 4)
    {
        const double span = result.beatsSec[i + 4] - result.beatsSec[i];
        if (span > 0.0) bars.push_back({(result.beatsSec[i] + result.beatsSec[i + 4]) / 2.0, 240.0 / span});
    }
    if (bars.size() < 4) return result;
    for (size_t i = 0; i < bars.size(); ++i)
    {
        std::vector<double> w;
        for (size_t j = i > 0 ? i - 1 : 0; j <= std::min(bars.size() - 1, i + 1); ++j) w.push_back(bars[j].bpm);
        std::sort(w.begin(), w.end());
        const double median = w[w.size() / 2];
        const bool outlier = std::abs(bars[i].bpm - median) / median > 0.03;
        result.curve.push_back({bars[i].sec, outlier ? median : bars[i].bpm});
    }
    std::vector<double> tempos;
    for (const auto& p : result.curve) tempos.push_back(p.bpm);
    result.mainBpm = percentile(tempos, 0.5);
    result.driftPercent = (percentile(tempos, 0.95) - percentile(tempos, 0.05)) / result.mainBpm * 100.0;

    // Sections: 4 or more bars off the main tempo (a single steady bar inside one is bridged).
    const auto off = [&](size_t i)
    { return std::abs(result.curve[i].bpm - result.mainBpm) / result.mainBpm > TempoDriftResult::kSectionThreshold; };
    for (size_t i = 0; i < result.curve.size();)
    {
        if (!off(i))
        {
            ++i;
            continue;
        }
        size_t end = i;
        while (end + 1 < result.curve.size() && (off(end + 1) || (end + 2 < result.curve.size() && off(end + 2)))) ++end;
        if (end - i + 1 >= 4)
        {
            double sum = 0.0;
            for (size_t j = i; j <= end; ++j) sum += result.curve[j].bpm;
            const double barSec = 240.0 / result.mainBpm;
            result.sections.push_back({result.curve[i].sec - barSec / 2.0, result.curve[end].sec + barSec / 2.0,
                                       sum / static_cast<double>(end - i + 1)});
        }
        i = end + 1;
    }
    result.ok = true;
    return result;
}

TempoDriftResult analyseTempoDrift(const juce::File& file, juce::AudioFormatManager& formatManager, double bpmHint,
                                   const std::function<bool()>& shouldAbort)
{
    const auto decoded = decodeMonoForAnalysis(file, formatManager, kMaxSeconds, kAnalysisRate,
                                               shouldAbort ? shouldAbort : std::function<bool()>([] { return false; }));
    if (!decoded.ok()) return {};
    return analyseTempoDriftSamples(decoded.mono, kAnalysisRate, bpmHint);
}

} // namespace stemcel
