#include "TestRegistry.h"

#include "midi/MidiControllerProfiles.h"
#include "performance/DeckPlayer.h"
#include "performance/PerfCurves.h"
#include "performance/PerformanceEngine.h"
#include "performance/PerformanceMapping.h"

#include <cmath>
#include <vector>

namespace stemcel::tests
{
namespace
{
using namespace stemcel::perf;

constexpr double kRate = 44100.0;
constexpr int kBlock = 441; // 10 ms

std::shared_ptr<DeckTrack> makeTrack(double seconds, double bpm, int serial = 1)
{
    auto audio = std::make_shared<juce::AudioBuffer<float>>(2, static_cast<int>(seconds * kRate));
    for (int i = 0; i < audio->getNumSamples(); ++i)
    {
        const auto v = static_cast<float>(0.25 * std::sin(2.0 * juce::MathConstants<double>::pi * 220.0 * i / kRate));
        audio->setSample(0, i, v);
        audio->setSample(1, i, v);
    }
    auto t = std::make_shared<DeckTrack>();
    t->serial = serial;
    t->audio = audio;
    t->sampleRate = kRate;
    t->bpm = bpm;
    t->anchorSec = 0.0;
    return t;
}

struct Rec final : DeckRecorder
{
    std::vector<Segment> segments;
    int platterPoints = 0;
    void segment(const Segment& s) noexcept override { segments.push_back(s); }
    void platter(int, juce::int64, double) noexcept override { ++platterPoints; }
};

struct DeckRig
{
    DeckPlayer deck{1};
    std::shared_ptr<DeckTrack> track;
    std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
    Rec rec;
    juce::int64 clock = 0;

    explicit DeckRig(double bpm = 120.0) : track(makeTrack(30.0, bpm))
    {
        deck.prepare(kRate, kBlock);
        deck.load(track.get(), nullptr);
    }
    void press(Control c, float v = 1.0F, int index = 0)
    {
        Event e;
        e.control = c;
        e.deck = 1;
        e.value = v;
        e.index = static_cast<std::int8_t>(index);
        deck.handle(e);
    }
    void run(double seconds, bool audible = true)
    {
        const int blocks = static_cast<int>(std::lround(seconds * kRate / kBlock));
        float* out[2] = {l.data(), r.data()};
        for (int i = 0; i < blocks; ++i)
        {
            deck.render(out, kBlock, SyncSource{}, &rec, clock, audible);
            clock += kBlock;
        }
    }
    DeckView view() const
    {
        DeckView v;
        deck.fillView(v);
        return v;
    }
};

void testCueButtonFollowsCdjRules()
{
    DeckRig rig;
    // Paused away from the cue point: CUE sets it here, snapped to the nearest beat.
    rig.press(Control::seek, 2.26F); // between beats (0.5 s each at 120 BPM)
    rig.run(0.05);
    rig.press(Control::cue, 1.0F);
    rig.press(Control::cue, 0.0F);
    auto v = rig.view();
    require(std::abs(v.cueSec - 2.5) < 1.0e-6 || std::abs(v.cueSec - 2.0) < 1.0e-6,
            "CUE while paused should set a quantized cue point");
    require(!v.playing && !v.previewing, "setting a cue point should not start playback");
    const double cue = v.cueSec;

    // At the cue point: holding CUE plays, releasing returns and pauses.
    rig.press(Control::cue, 1.0F);
    rig.run(1.0);
    v = rig.view();
    require(v.previewing && v.posSec > cue + 0.9, "holding CUE at the cue point should play");
    rig.press(Control::cue, 0.0F);
    rig.run(0.02);
    v = rig.view();
    require(!v.previewing && !v.playing && std::abs(v.posSec - cue) < 1.0e-6,
            "releasing CUE should return to the cue point and pause");

    // PLAY during the hold keeps playing after CUE is released.
    rig.press(Control::cue, 1.0F);
    rig.run(0.5);
    rig.press(Control::play, 1.0F);
    rig.press(Control::play, 0.0F);
    rig.press(Control::cue, 0.0F);
    rig.run(0.5);
    v = rig.view();
    require(v.playing && v.posSec > cue + 0.9, "PLAY during a CUE hold should latch playback");

    // Playing: CUE is Back Cue.
    rig.press(Control::cue, 1.0F);
    rig.press(Control::cue, 0.0F);
    rig.run(0.02);
    v = rig.view();
    require(!v.playing && std::abs(v.posSec - cue) < 1.0e-6, "CUE while playing should return to the cue point");
}

void testCueHoldIsRecordedAsAudibleSegment()
{
    DeckRig rig;
    rig.press(Control::cue, 1.0F); // the load put the cue on the first beat; we are on it
    rig.run(1.0, true);
    rig.press(Control::cue, 0.0F);
    rig.run(0.2, true);
    bool sawMoving = false, sawStop = false;
    double movedFrom = -1.0, endedAt = -1.0;
    for (const auto& s : rig.rec.segments)
    {
        if (s.moving && s.audible && !sawMoving)
        {
            sawMoving = true;
            movedFrom = s.startPos;
        }
        else if (sawMoving && !s.moving && !sawStop)
        {
            sawStop = true;
            endedAt = s.endPos;
        }
    }
    require(sawMoving && sawStop, "a CUE hold should open and close an audible segment");
    require(std::abs((endedAt - movedFrom) - 1.0) < 0.03, "the segment should cover what was heard (about 1 s)");
}

void testLoopsAndBeatJump()
{
    DeckRig rig;
    rig.press(Control::play);
    rig.run(1.1);
    rig.press(Control::beatLoop, 2.0F); // 2 beats = 1 s
    auto v = rig.view();
    require(v.loopActive && std::abs((v.loopOutSec - v.loopInSec) - 1.0) < 1.0e-6, "a 2-beat loop should be 1 s at 120 BPM");
    require(std::abs(v.loopInSec - 1.0) < 1.0e-6, "the loop should start on the beat before the playhead");
    rig.run(3.0);
    v = rig.view();
    require(v.posSec >= v.loopInSec - 1.0e-6 && v.posSec < v.loopOutSec, "playback should stay inside the loop");
    int wraps = 0;
    for (const auto& s : rig.rec.segments)
        if (std::abs(s.endPos - 2.0) < 1.0e-3 && std::abs(s.startPos - 1.0) < 1.0e-3) ++wraps;
    require(wraps >= 2, "each pass of the loop should be its own segment for the timeline");
    rig.press(Control::callPrev); // halve
    v = rig.view();
    require(std::abs((v.loopOutSec - v.loopInSec) - 0.5) < 1.0e-6, "CUE/LOOP CALL ◁ should halve the loop");
    rig.press(Control::reloopExit);
    v = rig.view();
    require(!v.loopActive, "RELOOP/EXIT should leave the loop");
    const double before = v.posSec;
    rig.press(Control::beatJump, 4.0F);
    v = rig.view();
    require(std::abs(v.posSec - (before + 2.0)) < 1.0e-6, "a 4-beat jump should move 2 s at 120 BPM");
}

void testHotCues()
{
    DeckRig rig;
    rig.press(Control::seek, 4.0F);
    rig.press(Control::hotCue, 1.0F, 0);
    rig.press(Control::hotCue, 0.0F, 0);
    auto v = rig.view();
    require(v.hotCues[0].set() && std::abs(v.hotCues[0].pos - 4.0) < 1.0e-6, "an empty hot cue pad should store the position");
    rig.press(Control::seek, 10.0F);
    rig.press(Control::hotCue, 1.0F, 0);
    rig.run(0.5);
    v = rig.view();
    require(v.previewing && v.posSec > 4.4, "holding a hot cue while paused should play from it");
    rig.press(Control::hotCue, 0.0F, 0);
    rig.run(0.02);
    v = rig.view();
    require(!v.playing && std::abs(v.posSec - 4.0) < 1.0e-6, "releasing it should return to it");
    rig.press(Control::hotCueDelete, 1.0F, 0);
    require(!rig.view().hotCues[0].set(), "SHIFT + pad should delete the hot cue");
}

void testTempoAndScratch()
{
    DeckRig rig;
    rig.press(Control::tempo, 1.0F); // +10% (default range)
    rig.press(Control::play);
    rig.run(1.0);
    auto v = rig.view();
    require(std::abs(v.tempo - 1.10) < 1.0e-6, "the slider's + end should be +range");
    require(std::abs(v.posSec - v.cueSec - 1.10) < 0.02, "the deck should play 10% fast");
    // Touch the platter and hold still: the record stops under the hand.
    rig.press(Control::jogTouch, 1.0F);
    rig.run(0.2);
    const double held = rig.view().posSec;
    rig.run(0.3);
    require(std::abs(rig.view().posSec - held) < 0.01, "a held platter should stop the audio");
    rig.press(Control::jogScratch, 720.0F); // one turn forward = 1.8 s
    rig.run(0.3);
    require(std::abs(rig.view().posSec - (held + 1.8)) < 0.02, "a turn of the platter should move 1.8 s");
    require(rig.rec.platterPoints > 0, "scratching should record the platter's path");
    rig.press(Control::jogTouch, 0.0F);
    rig.run(0.5);
    require(rig.view().posSec > held + 1.8 + 0.4, "releasing the platter should play on");
}

void testEngineSyncAndRecordingParams()
{
    PerformanceEngine engine;
    engine.prepareToPlay(kBlock, kRate);
    engine.setOpen(true);
    auto a = makeTrack(20.0, 120.0, 1);
    auto b = makeTrack(20.0, 125.0, 2);
    engine.load(1, a);
    engine.load(2, b);
    juce::AudioBuffer<float> out(2, kBlock);
    auto render = [&](double seconds)
    {
        for (int i = 0; i < static_cast<int>(seconds * kRate / kBlock); ++i)
            engine.getNextAudioBlock(juce::AudioSourceChannelInfo(&out, 0, kBlock));
    };
    auto post = [&](Control c, int deck, float v)
    {
        Event e;
        e.control = c;
        e.deck = static_cast<std::int8_t>(deck);
        e.value = v;
        engine.post(e);
    };
    render(0.05);
    post(Control::play, 1, 1.0F);
    render(0.5);
    post(Control::sync, 2, 1.0F);
    render(0.05);
    const auto& v = engine.view();
    require(v.masterDeck == 1, "syncing deck 2 while deck 1 plays should make deck 1 the master");
    require(std::abs(v.decks[1].effectiveBpm - 120.0) < 1.0e-6, "the synced deck should take the master's tempo");

    engine.setRecording(true);
    post(Control::fader, 2, 0.7F);
    post(Control::crossfader, 0, 0.5F);
    post(Control::cue, 2, 1.0F); // deck 2 is on its cue point
    render(0.5);
    post(Control::cue, 2, 0.0F);
    render(0.1);
    std::vector<RecEvent> log;
    engine.drainRecording(log);
    bool level = false, audible = false;
    for (const auto& r : log)
    {
        if (r.kind == RecEvent::Kind::param && r.deck == 2 && r.param == static_cast<std::uint8_t>(RecParam::levelDb))
        {
            const double expected = curves::gainToDb(curves::faderGain(0.7) *
                                                     curves::crossfaderGain(0.5, 1, curves::CrossfaderCurve::smooth));
            if (std::abs(r.a - expected) < 0.05) level = true;
        }
        if (r.kind == RecEvent::Kind::segment && r.deck == 2 && (r.flags & 3) == 3) audible = true;
    }
    require(level, "the recorder should log deck 2's level as fader × crossfader in dB");
    require(audible, "a CUE hold with the fader up should be an audible segment");
}

void testDdj400Mapping()
{
    juce::String error;
    const auto mapping =
        Mapping::fromFile(midiMappingDirectory().getChildFile("performance").getChildFile("pioneer-ddj-400.json"), error);
    require(mapping != nullptr, ("the DDJ-400 mapping should load: " + error).toStdString().c_str());
    require(Mapping::forDevice("2- DDJ-400") != nullptr, "the DDJ-400 should find its performance mapping");
    Mapping::Translator t(mapping);
    Event e[4];
    auto one = [&](std::initializer_list<int> bytes)
    {
        std::vector<juce::uint8> b;
        for (int x : bytes) b.push_back(static_cast<juce::uint8>(x));
        return t.translate(b.data(), static_cast<int>(b.size()), e, 4);
    };
    require(one({0x91, 0x0C, 0x7F}) == 1 && e[0].control == Control::cue && e[0].deck == 2 && e[0].value > 0.5F,
            "deck 2 CUE should map");
    require(one({0x90, 0x0C, 0x00}) == 1 && e[0].control == Control::cue && e[0].value < 0.5F, "CUE release should map");
    require(one({0x90, 0x14, 0x7F}) == 1 && e[0].control == Control::loopInLong, "LOOP IN held should be the 4-beat loop");
    require(one({0x99, 0x63, 0x7F}) == 1 && e[0].control == Control::pad && e[0].deck == 2 &&
                e[0].mode == PadMode::beatLoop && e[0].index == 3,
            "a BEAT LOOP pad on deck 2 should map with its index");
    require(one({0x98, 0x07, 0x7F}) == 1 && e[0].control == Control::padShift && e[0].mode == PadMode::hotCue &&
                e[0].index == 7,
            "SHIFT + hot cue pad 8 should map");
    require(one({0xB0, 0x22, 0x43}) == 1 && e[0].control == Control::jogScratch && std::abs(e[0].value - 3.0F) < 1.0e-6,
            "the jog top should give relative ticks");
    require(one({0xB6, 0x40, 0x7F}) == 1 && e[0].control == Control::browse && std::abs(e[0].value + 1.0F) < 1.0e-6,
            "browse anticlockwise should be −1");
    one({0xB1, 0x13, 0x40});
    require(one({0xB1, 0x33, 0x00}) == 1 && e[0].control == Control::fader && e[0].deck == 2 &&
                std::abs(e[0].value - static_cast<float>(0x40 << 7) / 16383.0F) < 1.0e-6,
            "a 14-bit channel fader should combine MSB and LSB");
    require(one({0x94, 0x11, 0x7F}) == 1 && e[0].control == Control::fxChannel && e[0].index == 1,
            "CH SELECT 2 should map");
    require(one({0x94, 0x10, 0x00}) == 0, "the CH SELECT switch's release half should be ignored");
    require(!mapping->leds("pad.hotCue", 1, 7).empty() && mapping->leds("pad.hotCue", 1, 7)[0].status == 0x97 &&
                mapping->leds("pad.hotCue", 1, 7)[0].data1 == 0x07,
            "pad LEDs should resolve per deck");
    require(mapping->leds("play", 2).size() == 2, "PLAY should light its plain and SHIFT addresses");
}
} // namespace

void addPerformanceTests(std::vector<TestCase>& tests)
{
    tests.push_back({"Performance deck CUE follows CDJ rules", testCueButtonFollowsCdjRules});
    tests.push_back({"Performance CUE hold is recorded as an audible segment", testCueHoldIsRecordedAsAudibleSegment});
    tests.push_back({"Performance loops and beat jump", testLoopsAndBeatJump});
    tests.push_back({"Performance hot cues", testHotCues});
    tests.push_back({"Performance tempo and scratch", testTempoAndScratch});
    tests.push_back({"Performance sync and recorded levels", testEngineSyncAndRecordingParams});
    tests.push_back({"Performance DDJ-400 mapping", testDdj400Mapping});
}

} // namespace stemcel::tests
