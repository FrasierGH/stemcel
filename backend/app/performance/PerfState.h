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
    int beatJumpPage = 1, keyPage = 0, keyboardCue = -1;
    float peak = 0.0F;
};

// What was loaded, from PERF_LOADED (and the library).
struct DeckTrackInfo
{
    int serial = 0;
    juce::String itemId, title, artist, key, audioPath;
    double bpm = 0.0, lengthSec = 0.0, anchorSec = 0.0;
};

struct State
{
    bool open = false, cueOutput = false, recording = false;
    double clock = 0.0, sampleRate = 44100.0;
    float crossfader = 0.5F;
    int masterDeck = 0;
    std::array<DeckState, kDecks> decks{};
    juce::StringArray midi; // the controller monitor's last lines
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
    s.clock = static_cast<double>(p.getProperty("clock", 0.0));
    s.sampleRate = static_cast<double>(p.getProperty("sampleRate", 44100.0));
    s.crossfader = static_cast<float>(static_cast<double>(p.getProperty("crossfader", 0.5)));
    s.masterDeck = static_cast<int>(p.getProperty("masterDeck", 0));
    if (const auto* m = p.getProperty("midi", {}).getArray())
    {
        s.midi.clear();
        for (const auto& line : *m) s.midi.add(line.toString());
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
        x.peak = static_cast<float>(num("peak", 0.0));
    }
}

} // namespace stemcel::native::perf
