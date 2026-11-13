#pragma once

// Live performance mode's state as the engine publishes it (PERF_STATE, PERF_LOADED), read
// into plain structs for the view.

#include <juce_core/juce_core.h>

#include <array>
#include <vector>

namespace stemcel::native::perf
{

constexpr int kDecks = 2;

struct CuePoint
{
    double pos = -1.0, end = -1.0;
    bool set() const { return pos >= 0.0; }
    bool isLoop() const { return end > pos && pos >= 0.0; }
};

struct DeckState
{
    int serial = 0;
    bool loaded = false, playing = false, previewing = false, atCue = false;
    double lengthSec = 0.0, posSec = 0.0, cueSec = 0.0;
    bool loopActive = false, loopArmed = false, loopInPending = false;
    double loopInSec = 0.0, loopOutSec = 0.0;
    int loopAdjust = 0;
    bool sync = false, master = false, keyLock = false, quantize = true, touched = false;
    double slider = 0.5, range = 0.10, tempo = 1.0, bpm = 0.0, effectiveBpm = 0.0, rate = 0.0;
    int semitones = 0;
    std::array<CuePoint, 8> hotCues{};
    std::vector<CuePoint> memoryCues;
    float trim = 0.5F, eqHi = 0.5F, eqMid = 0.5F, eqLow = 0.5F, filter = 0.5F, fader = 1.0F;
    bool hpCue = false;
    juce::String padMode = "hotCue";
    int beatJumpPage = 1, keyPage = 0, keyboardCue = -1, padFxHeld = -1;
    bool bound = false; // the channel drives a timeline track
    float peak = 0.0F;
};

struct FxState
{
    int type = 1, step = 5, channel = 0, colorType = 0, releaseType = 1;
    juce::String typeName = "ECHO", stepLabel = "1", colorName = "FILTER";
    float level = 0.5F;
    bool on = false, tap = false;
    double bpm = 120.0;
};

struct SamplerState
{
    int bank = 0;
    float volume = 0.8F;
    juce::uint64 loaded = 0, playing = 0;
    bool isLoaded(int slot) const { return ((loaded >> slot) & 1ULL) != 0; }
    bool isPlaying(int slot) const { return ((playing >> slot) & 1ULL) != 0; }
};

// What was loaded, from PERF_LOADED (and the library).
struct DeckTrackInfo
{
    int serial = 0;
    juce::String itemId, title, artist, key, audioPath;
    double bpm = 0.0, lengthSec = 0.0, anchorSec = 0.0;
    std::vector<double> beats; // the analysed beats (seconds), as the deck quantizes to them
};

struct State
{
    bool open = false, cueOutput = false, recording = false;
    double clock = 0.0, sampleRate = 44100.0;
    float crossfader = 0.5F;
    int masterDeck = 0;
    std::array<DeckState, kDecks> decks{};
    FxState fx;
    SamplerState sampler;
    juce::StringArray midi; // the controller monitor's last lines
    juce::String cueDevice; // where the headphone cue goes
};

inline CuePoint cueFromVar(const juce::var& v)
{
    if (!v.isObject()) return {};
    return {static_cast<double>(v.getProperty("pos", -1.0)), static_cast<double>(v.getProperty("end", -1.0))};
}

inline void readState(const juce::var& p, State& s)
{
    s.open = static_cast<bool>(p.getProperty("open", false));
    s.cueOutput = static_cast<bool>(p.getProperty("cueOutput", false));
    s.recording = static_cast<bool>(p.getProperty("recording", false));
    s.cueDevice = p.getProperty("cueDevice", "").toString();
    s.clock = static_cast<double>(p.getProperty("clock", 0.0));
    s.sampleRate = static_cast<double>(p.getProperty("sampleRate", 44100.0));
    s.crossfader = static_cast<float>(static_cast<double>(p.getProperty("crossfader", 0.5)));
    s.masterDeck = static_cast<int>(p.getProperty("masterDeck", 0));
    if (const auto* m = p.getProperty("midi", {}).getArray())
    {
        s.midi.clear();
        for (const auto& line : *m) s.midi.add(line.toString());
    }
    if (const auto fx = p.getProperty("fx", {}); fx.isObject())
    {
        s.fx.type = static_cast<int>(fx.getProperty("type", 1));
        s.fx.typeName = fx.getProperty("typeName", "").toString();
        s.fx.step = static_cast<int>(fx.getProperty("step", 5));
        s.fx.stepLabel = fx.getProperty("stepLabel", "").toString();
        s.fx.channel = static_cast<int>(fx.getProperty("channel", 0));
        s.fx.level = static_cast<float>(static_cast<double>(fx.getProperty("level", 0.5)));
        s.fx.on = static_cast<bool>(fx.getProperty("on", false));
        s.fx.tap = static_cast<bool>(fx.getProperty("tap", false));
        s.fx.bpm = static_cast<double>(fx.getProperty("bpm", 120.0));
        s.fx.colorType = static_cast<int>(fx.getProperty("colorType", 0));
        s.fx.colorName = fx.getProperty("colorName", "").toString();
        s.fx.releaseType = static_cast<int>(fx.getProperty("releaseType", 1));
    }
    if (const auto sm = p.getProperty("sampler", {}); sm.isObject())
    {
        s.sampler.bank = static_cast<int>(sm.getProperty("bank", 0));
        s.sampler.volume = static_cast<float>(static_cast<double>(sm.getProperty("volume", 0.8)));
        s.sampler.loaded = static_cast<juce::uint64>(sm.getProperty("loaded", "0").toString().getHexValue64());
        s.sampler.playing = static_cast<juce::uint64>(sm.getProperty("playing", "0").toString().getHexValue64());
    }
    const auto* decks = p.getProperty("decks", {}).getArray();
    if (decks == nullptr) return;
    for (int i = 0; i < juce::jmin(kDecks, decks->size()); ++i)
    {
        const auto& d = (*decks)[i];
        auto& x = s.decks[static_cast<size_t>(i)];
        auto num = [&](const char* k, double def) { return static_cast<double>(d.getProperty(k, def)); };
        auto flag = [&](const char* k) { return static_cast<bool>(d.getProperty(k, false)); };
        x.serial = static_cast<int>(d.getProperty("serial", 0));
        x.loaded = flag("loaded");
        x.playing = flag("playing");
        x.previewing = flag("previewing");
        x.atCue = flag("atCue");
        x.lengthSec = num("lengthSec", 0.0);
        x.posSec = num("posSec", 0.0);
        x.cueSec = num("cueSec", 0.0);
        x.loopActive = flag("loopActive");
        x.loopArmed = flag("loopArmed");
        x.loopInPending = flag("loopInPending");
        x.loopInSec = num("loopInSec", 0.0);
        x.loopOutSec = num("loopOutSec", 0.0);
        x.loopAdjust = static_cast<int>(d.getProperty("loopAdjust", 0));
        x.sync = flag("sync");
        x.master = flag("master");
        x.keyLock = flag("keyLock");
        x.quantize = static_cast<bool>(d.getProperty("quantize", true));
        x.touched = flag("touched");
        x.slider = num("slider", 0.5);
        x.range = num("range", 0.1);
        x.tempo = num("tempo", 1.0);
        x.bpm = num("bpm", 0.0);
        x.effectiveBpm = num("effectiveBpm", 0.0);
        x.rate = num("rate", 0.0);
        x.semitones = static_cast<int>(d.getProperty("semitones", 0));
        if (const auto* hot = d.getProperty("hotCues", {}).getArray())
            for (int h = 0; h < juce::jmin(8, hot->size()); ++h) x.hotCues[static_cast<size_t>(h)] = cueFromVar((*hot)[h]);
        x.memoryCues.clear();
        if (const auto* mem = d.getProperty("memoryCues", {}).getArray())
            for (const auto& c : *mem) x.memoryCues.push_back(cueFromVar(c));
        x.trim = static_cast<float>(num("trim", 0.5));
        x.eqHi = static_cast<float>(num("eqHi", 0.5));
        x.eqMid = static_cast<float>(num("eqMid", 0.5));
        x.eqLow = static_cast<float>(num("eqLow", 0.5));
        x.filter = static_cast<float>(num("filter", 0.5));
        x.fader = static_cast<float>(num("fader", 1.0));
        x.hpCue = flag("hpCue");
        x.padMode = d.getProperty("padMode", "hotCue").toString();
        x.beatJumpPage = static_cast<int>(d.getProperty("beatJumpPage", 1));
        x.keyPage = static_cast<int>(d.getProperty("keyPage", 0));
        x.keyboardCue = static_cast<int>(d.getProperty("keyboardCue", -1));
        x.padFxHeld = static_cast<int>(d.getProperty("padFxHeld", -1));
        x.bound = static_cast<bool>(d.getProperty("bound", false));
        x.peak = static_cast<float>(num("peak", 0.0));
    }
}

} // namespace stemcel::native::perf
