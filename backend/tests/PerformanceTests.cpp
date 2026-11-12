#include "TestRegistry.h"

#include "midi/MidiControllerProfiles.h"
#include "performance/DeckPlayer.h"
#include "performance/PerfCurves.h"
#include "performance/PerformanceEngine.h"
#include "performance/PerformanceMapping.h"
#include "DjFx.h"

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
        e.index = static_cast<std::int16_t>(index);
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
        e.deck = static_cast<std::int16_t>(deck);
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
                                                     curves::crossfaderGain(0.5, 1, curves::CrossfaderCurve::full));
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
void testCueStaysWhereCueSetIt()
{
    DeckRig rig;
    const double cue = rig.view().cueSec;
    rig.press(Control::jogBend, 300.0F); // paused: the wheel moves the position
    rig.run(0.05);
    require(rig.view().posSec > cue + 0.5, "the jog should move a paused deck");
    rig.press(Control::jogSearch, 50.0F);
    rig.press(Control::beatJump, 4.0F);
    require(std::abs(rig.view().cueSec - cue) < 1.0e-9, "the jog and beat jump should not move the cue point");
    rig.press(Control::play);
    rig.run(0.5);
    rig.press(Control::cue);
    rig.press(Control::cue, 0.0F);
    rig.run(0.02);
    require(std::abs(rig.view().posSec - cue) < 1.0e-6, "CUE while playing should go back to where CUE set the cue point");
    // Quantize off: the cue point is set exactly where the deck is.
    rig.press(Control::quantize, 0.0F);
    rig.press(Control::seek, 2.26F);
    rig.press(Control::cue);
    rig.press(Control::cue, 0.0F);
    require(std::abs(rig.view().cueSec - 2.26) < 1.0e-4, "without quantize the cue point should not snap");
}

void testQuantizeFollowsAnalysedBeats()
{
    // Beats that drift from a rigid grid: the cue snaps to the analysed beat.
    auto track = makeTrack(30.0, 120.0);
    for (int k = 0; k < 40; ++k) track->beats.push_back(0.1 + k * 0.5 + (k >= 10 ? 0.07 : 0.0));
    DeckPlayer deck(1);
    deck.prepare(kRate, kBlock);
    deck.load(track.get(), nullptr);
    Event e;
    e.deck = 1;
    e.control = Control::seek;
    e.value = 6.2F; // near the 13th beat, 6.17 s
    deck.handle(e);
    e.control = Control::cue;
    e.value = 1.0F;
    deck.handle(e);
    DeckView v;
    deck.fillView(v);
    require(std::abs(v.cueSec - 6.17) < 1.0e-6, "quantize should snap to the analysed beat, not a rigid grid");
}

void testTempoRangeHoldsTempoUntilPickup()
{
    DeckRig rig;
    rig.press(Control::tempo, 0.75F); // +5% at ±10%
    require(std::abs(rig.view().tempo - 1.05) < 1.0e-6, "the slider should set the tempo");
    rig.press(Control::syncShift); // ±16%
    require(std::abs(rig.view().tempo - 1.05) < 1.0e-6, "a new range should keep the tempo");
    rig.press(Control::tempo, 0.70F); // towards where +5% is in ±16% (0.656), not there yet
    require(std::abs(rig.view().tempo - 1.05) < 1.0e-6, "the slider should not take over before it reaches the tempo");
    rig.press(Control::tempo, 0.66F);
    require(std::abs(rig.view().tempo - (1.0 + 0.16 * 0.32)) < 1.0e-6, "past the pickup the slider should take over");
}

void testSyncedDeckKeepsTempoWhenItPlays()
{
    PerformanceEngine engine;
    engine.prepareToPlay(kBlock, kRate);
    engine.setOpen(true);
    engine.load(1, makeTrack(20.0, 120.0, 1));
    engine.load(2, makeTrack(20.0, 125.0, 2));
    juce::AudioBuffer<float> out(2, kBlock);
    auto render = [&](double seconds)
    {
        for (int i = 0; i < static_cast<int>(seconds * kRate / kBlock); ++i)
            engine.getNextAudioBlock(juce::AudioSourceChannelInfo(&out, 0, kBlock));
    };
    auto post = [&](Control c, int deck)
    {
        Event e;
        e.control = c;
        e.deck = static_cast<std::int16_t>(deck);
        e.value = 1.0F;
        engine.post(e);
    };
    render(0.05);
    post(Control::sync, 2); // both paused: deck 1 leads
    render(0.05);
    require(std::abs(engine.view().decks[1].effectiveBpm - 120.0) < 1.0e-6, "sync should match deck 1");
    post(Control::play, 2); // deck 2 plays alone and takes the master
    render(0.5);
    require(std::abs(engine.view().decks[1].effectiveBpm - 120.0) < 1.0e-6, "the deck should keep its synced tempo when it plays");
}

bool finite(const std::vector<float>& v)
{
    for (const float x : v)
        if (!std::isfinite(x) || std::abs(x) > 8.0F) return false;
    return true;
}

void testBeatFxUnits()
{
    // Every Beat FX: stays finite on noise, passes the signal untouched when off and idle, and
    // the ones with tails (echo, reverb...) ring out to silence after the input stops.
    juce::Random random(7);
    for (int type = 0; type < static_cast<int>(djfx::BeatFx::count); ++type)
    {
        djfx::BeatFxUnit unit;
        unit.allocate(kRate);
        unit.setBpm(124.0);
        unit.setType(type);
        unit.setLevel(0.8F);
        std::vector<float> l(kBlock), r(kBlock);
        auto run = [&](bool noise, int blocks, double& peak)
        {
            bool ok = true;
            for (int b = 0; b < blocks; ++b)
            {
                for (int i = 0; i < kBlock; ++i)
                {
                    l[static_cast<size_t>(i)] = noise ? random.nextFloat() * 0.5F - 0.25F : 0.0F;
                    r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];
                }
                unit.process(l.data(), r.data(), kBlock);
                ok = ok && finite(l) && finite(r);
                for (const float x : l) peak = std::max(peak, static_cast<double>(std::abs(x)));
            }
            return ok;
        };
        double peak = 0.0;
        // Off from the start: exactly the input.
        std::vector<float> before(kBlock);
        for (int i = 0; i < kBlock; ++i) before[static_cast<size_t>(i)] = l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] = 0.1F * std::sin(0.05F * i);
        unit.process(l.data(), r.data(), kBlock);
        require(l == before, (juce::String("Beat FX ") + djfx::beatFxName(type) + " should pass the signal when off").toRawUTF8());
        unit.setOn(true);
        require(run(true, 200, peak), (juce::String("Beat FX ") + djfx::beatFxName(type) + " should stay finite").toRawUTF8());
        unit.setOn(false);
        peak = 0.0;
        run(false, 1500, peak); // 15 s of silence for the longest tails (Spiral, Helix)
        peak = 0.0;
        run(false, 200, peak); // then 2 more: nothing left
        require(peak < 1.0e-3, (juce::String("Beat FX ") + djfx::beatFxName(type) + " should ring out to silence (peak "
                                + juce::String(peak, 6) + ")").toRawUTF8());
    }
}

void testColorFxUnits()
{
    juce::Random random(3);
    for (int type = 0; type < static_cast<int>(djfx::ColorFx::count); ++type)
        for (const float amount : {-0.8F, 0.8F})
        {
            djfx::ColorFxUnit unit;
            unit.allocate(kRate);
            unit.setBpm(120.0);
            unit.setType(type);
            unit.setAmount(amount);
            std::vector<float> l(kBlock), r(kBlock);
            bool ok = true;
            for (int b = 0; b < 100; ++b)
            {
                for (int i = 0; i < kBlock; ++i) l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] = random.nextFloat() * 0.5F - 0.25F;
                unit.process(l.data(), r.data(), kBlock);
                ok = ok && finite(l) && finite(r);
            }
            require(ok, (juce::String("Sound Color FX ") + djfx::colorFxName(type) + " should stay finite").toRawUTF8());
        }
}

void testEffectsAndSamplerAreRecorded()
{
    PerformanceEngine engine;
    engine.prepareToPlay(kBlock, kRate);
    engine.setOpen(true);
    engine.load(1, makeTrack(20.0, 120.0, 1));
    engine.loadSample(0, makeTrack(1.0, 0.0, 9));
    juce::AudioBuffer<float> out(2, kBlock);
    auto render = [&](double seconds)
    {
        for (int i = 0; i < static_cast<int>(seconds * kRate / kBlock); ++i)
            engine.getNextAudioBlock(juce::AudioSourceChannelInfo(&out, 0, kBlock));
    };
    auto post = [&](Control c, int deck, float v, int index = 0, PadMode mode = PadMode::hotCue)
    {
        Event e;
        e.control = c;
        e.deck = static_cast<std::int16_t>(deck);
        e.value = v;
        e.index = static_cast<std::int16_t>(index);
        e.mode = mode;
        engine.post(e);
    };
    render(0.05);
    engine.setRecording(true);
    post(Control::play, 1, 1.0F);
    post(Control::fxChannel, 0, 1.0F, 0); // CH1
    post(Control::fxOn, 0, 1.0F);
    render(0.5);
    post(Control::fxOn, 0, 1.0F); // off
    post(Control::pad, 1, 1.0F, 2, PadMode::padFx1); // ROLL 1/4 held
    render(0.3);
    post(Control::pad, 1, 0.0F, 2, PadMode::padFx1);
    post(Control::pad, 1, 1.0F, 0, PadMode::sampler); // slot 1
    render(0.3);
    std::vector<RecEvent> log;
    engine.drainRecording(log);
    bool beatOn = false, beatOff = false, padOn = false, sampleStarted = false;
    for (const auto& r : log)
    {
        if (r.kind == RecEvent::Kind::param && r.deck == 1 && r.param == static_cast<std::uint8_t>(RecParam::beatFxOn))
        {
            if (r.a > 0.5) beatOn = true;
            else if (beatOn) beatOff = true;
        }
        if (r.kind == RecEvent::Kind::param && r.deck == 1 && r.param == static_cast<std::uint8_t>(RecParam::padFxOn) && r.a > 0.5)
            padOn = true;
        if (r.kind == RecEvent::Kind::sample && r.param == 0 && (r.flags & 1) != 0 && r.serial == 9) sampleStarted = true;
    }
    require(beatOn && beatOff, "the Beat FX on deck 1 should be recorded on and then off");
    require(padOn, "a held Pad FX pad should be recorded");
    require(sampleStarted, "a sampler pad should record its hit");
    require((engine.view().samplesPlaying & 1ULL) != 0, "the sampler slot should be playing");
}
} // namespace

void addPerformanceTests(std::vector<TestCase>& tests)
{
    tests.push_back({"Performance cue stays where CUE set it", testCueStaysWhereCueSetIt});
    tests.push_back({"Performance quantize follows analysed beats", testQuantizeFollowsAnalysedBeats});
    tests.push_back({"Performance tempo range holds tempo until pickup", testTempoRangeHoldsTempoUntilPickup});
    tests.push_back({"Performance synced deck keeps tempo when it plays", testSyncedDeckKeepsTempoWhenItPlays});
    tests.push_back({"Performance Beat FX units", testBeatFxUnits});
    tests.push_back({"Performance Sound Color FX units", testColorFxUnits});
    tests.push_back({"Performance effects and sampler are recorded", testEffectsAndSamplerAreRecorded});
    tests.push_back({"Performance deck CUE follows CDJ rules", testCueButtonFollowsCdjRules});
    tests.push_back({"Performance CUE hold is recorded as an audible segment", testCueHoldIsRecordedAsAudibleSegment});
    tests.push_back({"Performance loops and beat jump", testLoopsAndBeatJump});
    tests.push_back({"Performance hot cues", testHotCues});
    tests.push_back({"Performance tempo and scratch", testTempoAndScratch});
    tests.push_back({"Performance sync and recorded levels", testEngineSyncAndRecordingParams});
    tests.push_back({"Performance DDJ-400 mapping", testDdj400Mapping});
}

} // namespace stemcel::tests
