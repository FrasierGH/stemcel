#pragma once

// A take: what the decks did while recording (the engine's PERF_REC log), and how it becomes
// timeline edits — a track per deck, a clip for every stretch of a track that reached the
// master (split where the source jumped, the tempo or key changed, or the jog took over), and
// automation for the channel's level (trim × fader × crossfader), EQ and filter. Written as
// one undo step, at the playhead the take started at.

#include "EngineClient.h"
#include "PerfState.h"
#include "ProjectModel.h"
#include "TimelineActions.h"

#include <juce_core/juce_core.h>

#include <map>
#include <vector>

namespace stemcel::native::perf
{

struct TakeSegment
{
    double at = 0.0; // performance clock, samples
    int deck = 0;
    juce::String itemId;
    double endPos = 0.0, startPos = 0.0; // seconds in the file
    double tempo = 1.0;
    int semitones = 0;
    bool moving = false, audible = false, keyLock = false, scratch = false;
};

struct TakePoint
{
    double at = 0.0;
    double value = 0.0;
};

// The clips and curves a take turns into (pure data, for tests).
struct TakeEdits
{
    struct Clip
    {
        int deck = 0;
        juce::String itemId;
        double positionMs = 0.0, inMs = 0.0, durationMs = 0.0; // durationMs: source time
        double tempoRatio = 1.0; // source ms per timeline ms
        int semitones = 0;
        bool keyLock = false, reversed = false;
        double timelineMs() const { return tempoRatio > 0.0 ? durationMs / tempoRatio : durationMs; }
    };
    std::vector<Clip> clips;
    // [deck - 1][param] → points (timeline ms, value)
    std::map<int, std::map<juce::String, std::vector<std::pair<double, double>>>> automation;
    double lengthMs = 0.0;
    std::vector<int> decksUsed;
    // Sampler hits: clips on a track per slot.
    struct Hit
    {
        int slot = 0;
        juce::String itemId;
        double positionMs = 0.0, durationMs = 0.0;
    };
    std::vector<Hit> samples;
    std::vector<std::pair<double, double>> samplerLevel; // (ms, dB)
    std::map<juce::String, std::vector<std::pair<double, double>>> master; // the master's Beat FX
};

class PerformanceTake
{
  public:
    void clear();
    void add(const juce::var& events); // a PERF_REC batch
    bool hasAudio() const;
    double lengthSec() const;
    bool started() const { return startAt >= 0.0; }

    TakeEdits edits(double startMs) const;
    // Writes it as tracks, clips and automation (one undo step); false when nothing was heard.
    bool write(EngineClient& engine, ProjectMirror& mirror, TimelineActions& actions, double startMs, int takeNumber) const;

  private:
    double startAt = -1.0, lastAt = 0.0, rate = 44100.0;
    std::vector<TakeSegment> segments;
    std::map<int, std::vector<std::pair<double, double>>> platter; // deck → (at, pos)
    std::map<int, std::map<juce::String, std::vector<TakePoint>>> params;
    struct SampleHit
    {
        double at = 0.0, stopAt = -1.0, lengthSec = 0.0;
        int slot = 0;
        juce::String itemId;
    };
    std::vector<SampleHit> sampleHits;
};

} // namespace stemcel::native::perf
