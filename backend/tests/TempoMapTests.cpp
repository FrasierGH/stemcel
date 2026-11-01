// Tempo map: the curve's arithmetic, clip timing along it, the engine following it (warped
// audio landing where the timeline draws it while the tempo ramps), undo and tempo edits.

#include "TestRegistry.h"

#include "AudioEngine.h"
#include "BeatRepeatSnapshot.h"
#include "BridgeServer.h"
#include "Metronome.h"
#include "OffsetSource.h"
#include "ProjectSettingsCommands.h"
#include "ProjectState.h"
#include "TempoCurve.h"
#include "TempoMapCommands.h"
#include "WarpProcessor.h"

#include <cmath>
#include <memory>
#include <vector>

namespace stemcel::tests
{
namespace
{
stemcel::BridgeServer silentBridge()
{
    return stemcel::BridgeServer("test-token", [](stemcel::BridgeServer&, const juce::String&, const juce::var&) {});
}

juce::var tempoMapPayload(std::initializer_list<stemcel::TempoPoint> points)
{
    juce::Array<juce::var> list;
    for (const auto& p : points)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("timeMs", p.timeMs);
        o->setProperty("bpm", p.bpm);
        list.add(juce::var(o));
    }
    auto* payload = new juce::DynamicObject();
    payload->setProperty("points", list);
    return juce::var(payload);
}

void testTempoCurveArithmetic()
{
    const stemcel::TempoCurve constant({}, 120.0);
    requireNear(constant.bpmAt(5000.0), 120.0, 1e-12, "no points is the base tempo");
    requireNear(constant.beatAt(1000.0), 2.0, 1e-12, "120 BPM is two beats a second");
    requireNear(constant.msAtBeat(8.0), 4000.0, 1e-9, "and eight beats take four seconds");

    // Flat 120 until 1 s, a ramp to 150 at 5 s, flat 150 after.
    const stemcel::TempoCurve ramp({{5000.0, 150.0}, {1000.0, 120.0}}, 100.0);
    requireNear(ramp.bpmAt(0.0), 120.0, 1e-12, "before the first point the tempo holds");
    requireNear(ramp.bpmAt(3000.0), 135.0, 1e-12, "half way up the ramp is half way between");
    requireNear(ramp.bpmAt(9000.0), 150.0, 1e-12, "after the last point the tempo holds");
    // Beats over the ramp: average 135 BPM for 4 s = 9 beats; plus 2 beats in the first second.
    requireNear(ramp.beatAt(5000.0), 11.0, 1e-9, "beats integrate the tempo");
    for (const double beat : {0.5, 2.0, 5.3, 11.0, 17.25})
        requireNear(ramp.beatAt(ramp.msAtBeat(beat)), beat, 1e-9, "msAtBeat inverts beatAt");
    // A clip recorded at 120 starting at 0: its first 2 s of source play by 1 s, and on the
    // ramp it plays faster than it was recorded, so 10 s of source take less than 10 s.
    requireNear(ramp.timelineAfterSource(0.0, 1000.0, 120.0), 1000.0, 1e-9, "at its own tempo a clip plays at its speed");
    const double end = ramp.timelineAfterSource(0.0, 10000.0, 120.0);
    require(end < 10000.0, "faster than recorded is shorter");
    requireNear(ramp.sourceMsBetween(0.0, end, 120.0), 10000.0, 1e-6, "sourceMsBetween inverts timelineAfterSource");
    require(ramp.variesBetween(500.0, 1500.0) && !ramp.variesBetween(6000.0, 8000.0) && !ramp.variesBetween(0.0, 900.0),
            "variesBetween sees a change only where the tempo moves");
}

// A clip whose source drifts follows the project tempo beat for beat, with or without a map:
// a source that really ran at 132 (detected as 120) takes 132 beats' worth of the timeline.
void testDriftingSourceFollowsBeatForBeat()
{
    stemcel::ProjectState state;
    state.setBpm(120.0);
    require(state.addLibraryItem("src", "C:\a.wav", "a.wav", 60000.0, 48000, 2), "source should add");
    require(state.setLibraryItemBpm("src", 120.0), "source BPM should apply");
    require(state.addTrack("t1") && state.addClip("t1", "c1", "src", 0.0, 60000.0), "clip should add");
    require(state.setClipWarp("c1", true, std::nullopt, std::nullopt, false, std::nullopt, std::nullopt, std::nullopt),
            "clip should warp");
    require(state.getClipTempoFollowBpm("c1") == 0.0, "a steady clip without a map does not follow");
    require(state.setClipSourceTempo("c1", {{0.0, 132.0}, {60000.0, 132.0}}), "the source tempo should apply");
    require(state.getClipTempoFollowBpm("c1") > 0.0, "a drifting clip follows even without a map");
    // 60 s at 132 BPM is 132 beats; at the project's 120 they take 66 s.
    requireNear(state.getClipEffectiveTiming("c1").durationMs, 66000.0, 1e-6, "beats land on the project's beats");
    require(state.setClipSourceTempo("c1", {}), "clearing should apply");
    requireNear(state.getClipEffectiveTiming("c1").durationMs, 60000.0, 1e-6, "cleared, it plays at its detected tempo");
}

// A warped clip whose ratio follows the project lasts as long as the map takes to play it.
void testClipTimingFollowsTheTempoMap()
{
    stemcel::ProjectState state;
    state.setBpm(120.0);
    require(state.addLibraryItem("src", "C:\\a.wav", "a.wav", 8000.0, 48000, 2), "source should add");
    require(state.setLibraryItemBpm("src", 120.0), "source BPM should apply");
    require(state.addTrack("t1") && state.addClip("t1", "c1", "src", 0.0, 8000.0), "clip should add");
    require(state.setClipWarp("c1", true, std::nullopt, std::nullopt, false, std::nullopt, std::nullopt, std::nullopt),
            "clip should warp");
    requireNear(state.getClipEffectiveTiming("c1").durationMs, 8000.0, 1e-9, "at the source tempo nothing moves");

    state.getUndoManager().beginNewTransaction("Tempo map");
    require(state.setTempoMap({{0.0, 120.0}, {4000.0, 160.0}}), "the map should apply");
    const auto curve = state.getTempoCurve();
    const auto timing = state.getClipEffectiveTiming("c1");
    require(timing.warpActive, "a clip on a tempo ramp is warped");
    requireNear(timing.durationMs, curve.timelineAfterSource(0.0, 8000.0, 120.0), 1e-6,
                "its length is how long the map takes to play its source");
    require(!state.setTempoMap({{0.0, 120.0}, {4000.0, 160.0}}), "the same map again is no change");

    // Undo returns the constant tempo.
    state.getUndoManager().undo();
    require(state.getTempoMap().empty(), "undo removes the map");
    requireNear(state.getClipEffectiveTiming("c1").durationMs, 8000.0, 1e-9, "and the clip's length with it");
}

// The engine follows: TEMPO_MAP_SET through the dispatcher marks warped clips as followers;
// a pinned ratio and an unwarped clip do not follow; an empty map stops it.
void testEngineFollowsTheTempoMap()
{
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("stemcel-tempomap-" + juce::Uuid().toString());
    dir.createDirectory();
    const auto wav = dir.getChildFile("loop.wav");
    {
        juce::AudioBuffer<float> buffer(2, 96000);
        buffer.clear();
        std::unique_ptr<juce::OutputStream> stream(wav.createOutputStream());
        std::unique_ptr<juce::AudioFormatWriter> writer(juce::WavAudioFormat().createWriterFor(
            stream, juce::AudioFormatWriterOptions{}.withSampleRate(48000).withNumChannels(2).withBitsPerSample(16)));
        require(writer != nullptr && writer->writeFromAudioSampleBuffer(buffer, 0, 96000), "test wav should write");
    }
    stemcel::AudioEngine engine;
    engine.initialise({}, {}, nullptr);
    stemcel::ProjectState state;
    state.setBpm(120.0);
    require(state.addLibraryItem("src", wav.getFullPathName(), "loop.wav", 2000.0, 48000, 2), "source should add");
    require(state.setLibraryItemBpm("src", 120.0), "source BPM should apply");
    require(state.addTrack("t1"), "track should add");
    for (const auto* id : {"warped", "pinned", "dry"})
    {
        require(state.addClip("t1", id, "src", 0.0, 2000.0), "clip should add");
        require(engine.addClip("t1", id, wav, 0.0), "engine clip should attach");
    }
    for (const auto* id : {"warped", "pinned"})
    {
        require(state.setClipWarp(id, true, std::nullopt, std::nullopt, false, std::nullopt, std::nullopt, std::nullopt),
                "clip should warp");
        require(engine.setClipWarp(id, true, std::nullopt, 1.0, std::nullopt, std::nullopt), "engine clip should warp");
    }
    require(state.setClipWarp("pinned", std::nullopt, std::nullopt, 1.1, false, std::nullopt, std::nullopt, std::nullopt),
            "ratio should pin");

    stemcel::syncTempoMap(engine, state);
    require(!engine.clipFollowsTempoMapForTest("warped"), "with no map nothing follows");

    require(stemcel::applyTempoMapSet(tempoMapPayload({{0.0, 120.0}, {1000.0, 140.0}}), state), "the map should set");
    stemcel::syncTempoMap(engine, state);
    require(engine.clipFollowsTempoMapForTest("warped"), "a warped clip follows the map");
    require(!engine.clipFollowsTempoMapForTest("pinned"), "a pinned ratio opts out");
    require(!engine.clipFollowsTempoMapForTest("dry"), "an unwarped clip plays at its own speed");

    require(stemcel::applyTempoMapSet(tempoMapPayload({}), state), "clearing should apply");
    stemcel::syncTempoMap(engine, state);
    require(!engine.clipFollowsTempoMapForTest("warped"), "an empty map stops following");
    engine.shutdown();
    dir.deleteRecursively();
}

// The point of it all: through the clip's real playback path, with the tempo ramping, every
// beat of the source comes out where the curve puts it (and so where the timeline draws it).
void testFollowedClipLandsBeatsOnTheCurve()
{
    constexpr double sr = 48000.0;
    constexpr double sourceBpm = 120.0;
    constexpr int beats = 16;
    const int beatSamples = static_cast<int>(sr * 60.0 / sourceBpm);
    juce::AudioBuffer<float> clicks(1, beatSamples * beats);
    clicks.clear();
    for (int b = 1; b < beats; ++b) clicks.setSample(0, b * beatSamples, 1.0f);

    // 120 for a second, up to 150 by 4 s, then flat: the clip starts half a second in.
    const stemcel::TempoCurve curve({{1000.0, 120.0}, {4000.0, 150.0}}, 120.0);
    constexpr juce::int64 clipStart = 24000;
    const double startMs = clipStart * 1000.0 / sr;

    for (const juce::String mode : {"repitch", "pro"})
    {
        juce::MemoryAudioSource child(clicks, false);
        stemcel::OffsetSource source(&child);
        source.setClipWindowAtomic(clipStart, 0, clicks.getNumSamples());
        stemcel::WarpProcessor warp(1, sr, stemcel::warpModeSpec(mode), 1.0, 1024);
        warp.prepareToPlay(512);
        source.setWarpProcessor(&warp);
        source.setTempoFollow(&curve, sourceBpm, sr);
        source.prepareToPlay(512, sr);
        source.setNextReadPosition(0);

        const auto total = static_cast<int>(source.getTotalLength());
        const double expectedEnd = curve.timelineAfterSource(startMs, beats * 500.0, sourceBpm) * sr / 1000.0;
        requireNear(static_cast<double>(total), expectedEnd, 2.0, (mode + ": the clip ends where the curve says").toRawUTF8());

        juce::AudioBuffer<float> out(1, total);
        out.clear();
        for (int done = 0; done < total; done += 512)
        {
            juce::AudioSourceChannelInfo info(&out, done, juce::jmin(512, total - done));
            source.getNextAudioBlock(info);
        }
        // Each click: the loudest sample near where the curve puts it.
        for (int b = 1; b < beats; ++b)
        {
            const double expected = curve.timelineAfterSource(startMs, b * 500.0, sourceBpm) * sr / 1000.0;
            const int from = static_cast<int>(expected) - 2000;
            int at = from;
            for (int i = from; i < from + 4000 && i < total; ++i)
                if (std::abs(out.getSample(0, i)) > std::abs(out.getSample(0, at))) at = i;
            requireNear(static_cast<double>(at), expected, sr / 1000.0,
                        (mode + ": every beat lands on the curve, first to last").toRawUTF8());
        }
        source.releaseResources();
    }
}

// A source whose tempo drifts (its clicks speed up from 120 to 132), followed with its own
// curve onto a steady 120: every click lands on a beat of the timeline.
void testDriftingSourceLandsOnTheBeat()
{
    constexpr double sr = 48000.0;
    constexpr int beats = 16;
    const stemcel::TempoCurve sourceCurve({{0.0, 120.0}, {7000.0, 132.0}}, 120.0);
    const stemcel::TempoCurve flat({}, 120.0);
    const auto lengthSamples = static_cast<int>(std::ceil(sourceCurve.msAtBeat(beats + 1) * sr / 1000.0));
    juce::AudioBuffer<float> clicks(1, lengthSamples);
    clicks.clear();
    for (int b = 1; b < beats; ++b)
        clicks.setSample(0, static_cast<int>(std::round(sourceCurve.msAtBeat(b) * sr / 1000.0)), 1.0f);

    constexpr juce::int64 clipStart = 24000;
    const double startMs = clipStart * 1000.0 / sr;
    juce::MemoryAudioSource child(clicks, false);
    stemcel::OffsetSource source(&child);
    source.setClipWindowAtomic(clipStart, 0, clicks.getNumSamples());
    stemcel::WarpProcessor warp(1, sr, stemcel::warpModeSpec("repitch"), 1.0, 1024);
    warp.prepareToPlay(512);
    source.setWarpProcessor(&warp);
    source.setTempoFollow(&flat, 120.0, sr, &sourceCurve);
    source.prepareToPlay(512, sr);
    source.setNextReadPosition(0);

    const auto total = static_cast<int>(source.getTotalLength());
    juce::AudioBuffer<float> out(1, total);
    out.clear();
    for (int done = 0; done < total; done += 512)
    {
        juce::AudioSourceChannelInfo info(&out, done, juce::jmin(512, total - done));
        source.getNextAudioBlock(info);
    }
    for (int b = 1; b < beats; ++b)
    {
        const double expected = (startMs + b * 500.0) * sr / 1000.0;
        const int from = static_cast<int>(expected) - 2000;
        int at = from;
        for (int i = from; i < from + 4000 && i < total; ++i)
            if (std::abs(out.getSample(0, i)) > std::abs(out.getSample(0, at))) at = i;
        requireNear(static_cast<double>(at), expected, sr / 1000.0, "every drifting beat lands on the steady grid");
    }
    source.releaseResources();
}

// Typing a new project tempo keeps every beat: the map's tempos scale and its points move
// with the clips and markers.
void testProjectTempoEditScalesTheMap()
{
    stemcel::ProjectState state;
    stemcel::AudioEngine engine;
    auto bridge = silentBridge();
    state.setBpm(120.0);
    require(state.setTempoMap({{2000.0, 120.0}, {6000.0, 140.0}}), "the map should apply");
    const double beatBefore = state.getTempoCurve().beatAt(6000.0);
    auto* payload = new juce::DynamicObject();
    payload->setProperty("bpm", 150.0);
    stemcel::handleProjectSetBpm(juce::var(payload), engine, state, bridge);
    const auto map = state.getTempoMap();
    require(map.size() == 2, "the map keeps its points");
    requireNear(map[1].bpm, 175.0, 1e-9, "tempos scale with the project tempo");
    requireNear(map[1].timeMs, 4800.0, 1e-9, "and points move like the clips do");
    requireNear(state.getTempoCurve().beatAt(map[1].timeMs), beatBefore, 1e-9, "so every point is on its old beat");
}

// The metronome and beat repeats sit on the map's beats, not the base tempo's.
void testMetronomeAndBeatRepeatFollowTheMap()
{
    constexpr double sr = 48000.0;
    const stemcel::TempoCurve curve({{0.0, 120.0}, {4000.0, 180.0}}, 100.0);

    stemcel::Metronome metronome;
    metronome.prepare(sr);
    metronome.setEnabled(true);
    metronome.setBpm(100.0);
    metronome.setTempoCurve(&curve);
    const int total = static_cast<int>(sr * 6.0);
    juce::AudioBuffer<float> out(1, total);
    out.clear();
    for (int done = 0; done < total; done += 512)
        metronome.render(out, done, juce::jmin(512, total - done), done, sr);
    for (int beat = 1; beat < 12; ++beat)
    {
        const auto at = static_cast<int>(std::llround(curve.msAtBeat(beat) * sr / 1000.0));
        require(std::abs(out.getSample(0, at)) < 1.0e-6 && std::abs(out.getSample(0, at + 5)) > 1.0e-3,
                "each click starts on the map's beat");
    }

    std::vector<stemcel::BeatRepeatRegion> regions{{"r", 8.0, 4.0, "1/4"}};
    const auto snap = stemcel::makeBeatRepeatSnapshot(regions, sr, 100.0, &curve);
    require(snap->regions.size() == 1, "the region survives");
    requireNear(static_cast<double>(snap->regions[0].startSample), curve.msAtBeat(8.0) * sr / 1000.0, 1.0,
                "a region starts on its beat along the map");
    requireNear(static_cast<double>(snap->regions[0].endSample), curve.msAtBeat(12.0) * sr / 1000.0, 1.0,
                "and ends on its beat");
    requireNear(static_cast<double>(snap->regions[0].divisionSamples), 60.0 / curve.bpmAt(curve.msAtBeat(8.0)) * sr, 1.0,
                "its repeat is a beat long at the tempo where it starts");
}

} // namespace

void addTempoMapTests(std::vector<TestCase>& tests)
{
    tests.push_back({"Metronome and beat repeat follow the tempo map", testMetronomeAndBeatRepeatFollowTheMap});
    tests.push_back({"Tempo curve arithmetic", testTempoCurveArithmetic});
    tests.push_back({"Clip timing follows the tempo map", testClipTimingFollowsTheTempoMap});
    tests.push_back({"A drifting source follows the project tempo beat for beat", testDriftingSourceFollowsBeatForBeat});
    tests.push_back({"Engine follows the tempo map", testEngineFollowsTheTempoMap});
    tests.push_back({"A followed clip lands its beats on the curve", testFollowedClipLandsBeatsOnTheCurve});
    tests.push_back({"A drifting source followed with its curve lands every beat on the grid", testDriftingSourceLandsOnTheBeat});
    tests.push_back({"Project tempo edit scales the tempo map", testProjectTempoEditScalesTheMap});
}

} // namespace stemcel::tests
