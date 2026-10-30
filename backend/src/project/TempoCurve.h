#pragma once

// The project's tempo over time: the tempo map (docs/native-app.md, "Tempo
// map"). Points ramp linearly from one to the next and hold flat before the first and
// after the last; no points is the project's constant tempo. Shared by the engine (a
// warped clip's stretch ratio at every moment, its length, where a seek lands in its
// source), the project model (clip timing) and the timeline (bars and beats).
//
// Everything here is closed form: the tempo is linear within a segment, so the beats
// (and source time) it covers are a quadratic, and their inverse a square root.

#include <algorithm>
#include <cmath>
#include <vector>

namespace stemcel
{

struct TempoPoint
{
    double timeMs = 0.0;
    double bpm = 120.0;
};

class TempoCurve
{
  public:
    TempoCurve() = default;
    // `points` need not be sorted; non-positive tempos are dropped.
    TempoCurve(const std::vector<TempoPoint>& points, double baseBpm)
        : base(baseBpm > 0.0 ? baseBpm : 120.0)
    {
        for (const auto& p : points)
            if (p.bpm > 0.0 && std::isfinite(p.bpm) && std::isfinite(p.timeMs)) pts.push_back(p);
        std::stable_sort(pts.begin(), pts.end(), [](const TempoPoint& a, const TempoPoint& b) { return a.timeMs < b.timeMs; });
    }

    bool isConstant() const noexcept { return pts.empty(); }
    const std::vector<TempoPoint>& points() const noexcept { return pts; }
    double baseBpm() const noexcept { return base; }

    double bpmAt(double ms) const noexcept
    {
        if (pts.empty()) return base;
        if (ms <= pts.front().timeMs) return pts.front().bpm;
        if (ms >= pts.back().timeMs) return pts.back().bpm;
        const auto i = segmentAt(ms);
        const auto& a = pts[i];
        const auto& b = pts[i + 1];
        const double span = b.timeMs - a.timeMs;
        return span > 0.0 ? a.bpm + (b.bpm - a.bpm) * (ms - a.timeMs) / span : b.bpm;
    }

    // The integral of tempo over [from, to] in bpm·ms (negative when to < from).
    double area(double from, double to) const noexcept
    {
        if (to < from) return -area(to, from);
        if (pts.empty()) return base * (to - from);
        double total = 0.0;
        double t = from;
        // Flat before the first point.
        if (t < pts.front().timeMs)
        {
            const double end = std::min(to, pts.front().timeMs);
            total += pts.front().bpm * (end - t);
            t = end;
        }
        for (size_t i = 0; i + 1 < pts.size() && t < to; ++i)
        {
            const auto& a = pts[i];
            const auto& b = pts[i + 1];
            if (t >= b.timeMs) continue;
            const double s = std::max(t, a.timeMs);
            const double e = std::min(to, b.timeMs);
            if (e > s) total += 0.5 * (bpmAt(s) + bpmAt(e)) * (e - s); // linear: the trapezium is exact
            t = e;
        }
        if (t < to) total += pts.back().bpm * (to - t); // flat after the last point
        return total;
    }

    // Beats from time 0 to `ms`.
    double beatAt(double ms) const noexcept { return area(0.0, ms) / 60000.0; }
    // The time at which `beat` beats have passed since time 0.
    double msAtBeat(double beat) const noexcept { return timeAfter(0.0, beat * 60000.0); }

    // Source time a clip warped to follow this curve covers between timeline `from` and
    // `to`, when its source was recorded at `sourceBpm`.
    double sourceMsBetween(double from, double to, double sourceBpm) const noexcept
    {
        return sourceBpm > 0.0 ? area(from, to) / sourceBpm : to - from;
    }

    // The timeline time at which a clip starting at `start` has played `sourceMs` of a
    // source recorded at `sourceBpm`.
    double timelineAfterSource(double start, double sourceMs, double sourceBpm) const noexcept
    {
        return sourceBpm > 0.0 ? timeAfter(start, sourceMs * sourceBpm) : start + sourceMs;
    }

    // True when the tempo changes anywhere in [from, to].
    bool variesBetween(double from, double to) const noexcept
    {
        if (pts.size() < 2) return false;
        const double first = bpmAt(from);
        if (std::abs(bpmAt(to) - first) > 1.0e-9) return true;
        for (const auto& p : pts)
            if (p.timeMs > from && p.timeMs < to && std::abs(p.bpm - first) > 1.0e-9) return true;
        return false;
    }

  private:
    size_t segmentAt(double ms) const noexcept
    {
        // The last point at or before `ms` (callers keep ms inside the points' span).
        const auto it = std::upper_bound(pts.begin(), pts.end(), ms, [](double v, const TempoPoint& p) { return v < p.timeMs; });
        const auto i = static_cast<size_t>(std::max<std::ptrdiff_t>(0, (it - pts.begin()) - 1));
        return std::min(i, pts.size() - 2);
    }

    // The time `t >= start` at which area(start, t) == target (target >= 0).
    double timeAfter(double start, double target) const noexcept
    {
        if (target <= 0.0) return start;
        if (pts.empty()) return start + target / base;
        double t = start;
        double left = target;
        if (t < pts.front().timeMs)
        {
            const double a = pts.front().bpm * (pts.front().timeMs - t);
            if (a >= left) return t + left / pts.front().bpm;
            left -= a;
            t = pts.front().timeMs;
        }
        for (size_t i = 0; i + 1 < pts.size(); ++i)
        {
            const auto& pa = pts[i];
            const auto& pb = pts[i + 1];
            if (t >= pb.timeMs) continue;
            const double s = std::max(t, pa.timeMs);
            const double segArea = area(s, pb.timeMs);
            if (segArea >= left)
            {
                // bpm(s + x) = v0 + k x; area = v0 x + k x^2 / 2 = left.
                const double v0 = bpmAt(s);
                const double span = pb.timeMs - pa.timeMs;
                const double k = span > 0.0 ? (pb.bpm - pa.bpm) / span : 0.0;
                if (std::abs(k) < 1.0e-12) return s + left / v0;
                const double disc = v0 * v0 + 2.0 * k * left;
                return s + (std::sqrt(std::max(0.0, disc)) - v0) / k;
            }
            left -= segArea;
            t = pb.timeMs;
        }
        return t + left / pts.back().bpm;
    }

    double base = 120.0;
    std::vector<TempoPoint> pts;
};

} // namespace stemcel
