#pragma once

// One performance deck: a track in RAM played like a CDJ — play/pause, the CUE button's
// rules, loops, beat jump, hot and memory cues, the tempo slider, key lock, sync, jog (scratch,
// pitch bend, search) — on the audio thread. Every control reaches it as an Event applied at
// the start of a block, so its state has one owner and its timing is sample-accurate, which
// is what lets a take be written back to the timeline exactly.

#include "PerfTypes.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <memory>
#include <vector>

namespace stemcel::perf
{

constexpr int kHotCues = 8;
constexpr int kMemoryCues = 16;

struct Cue
{
    double pos = -1.0; // seconds in the file; < 0 = empty
    double end = -1.0; // loop end in seconds; < 0 = not a loop
    bool set() const noexcept { return pos >= 0.0; }
    bool isLoop() const noexcept { return end > pos && pos >= 0.0; }
};

// A loaded track. Built on a worker thread, handed to the audio thread by pointer, kept alive
// by the engine until no deck holds it.
struct DeckTrack
{
    int serial = 0;
    juce::String itemId, title, artist, key; // message thread only
    std::shared_ptr<const juce::AudioBuffer<float>> audio; // stereo
    double sampleRate = 44100.0; // of `audio`
    double bpm = 0.0;            // 0 = no beat grid
    double anchorSec = 0.0;      // a downbeat
    double cueSec = -1.0;        // the cue point to start at; < 0 = the first beat
    std::array<Cue, kHotCues> hotCues{};
    std::array<Cue, kMemoryCues> memoryCues{};
};

// What the deck tells the recorder (implemented by the engine, called on the audio thread).
class DeckRecorder
{
  public:
    virtual ~DeckRecorder() = default;
    struct Segment
    {
        int deck = 0, serial = 0;
        juce::int64 at = 0;          // performance clock (output samples)
        double endPos = 0.0;         // where the previous segment ended (seconds in the file)
        double startPos = 0.0;       // where this one starts
        double tempo = 1.0;          // tempo factor (1 = the track's own tempo)
        int semitones = 0;
        bool moving = false;         // playing or held on a cue
        bool audible = false;        // reaching the master
        bool keyLock = false;
        bool scratch = false;        // jog in control: the platter is recorded point by point
    };
    virtual void segment(const Segment& s) noexcept = 0;
    virtual void platter(int deck, juce::int64 at, double pos) noexcept = 0;
};

// The master deck as a follower sees it, at the start of the block.
struct SyncSource
{
    bool valid = false;
    double bpm = 0.0;       // effective
    double beatPhase = 0.0; // beats since its grid's anchor
    bool moving = false;
};

// For the UI and LEDs, published every block.
struct DeckView
{
    int serial = 0;
    double lengthSec = 0.0, posSec = 0.0, cueSec = 0.0;
    bool loaded = false, playing = false, previewing = false, atCue = false;
    bool loopActive = false, loopArmed = false, loopInPending = false;
    double loopInSec = 0.0, loopOutSec = 0.0;
    int loopAdjust = 0; // 1 = in, 2 = out
    bool sync = false, master = false, keyLock = false, quantize = true, touched = false;
    float slider = 0.5F;
    double range = 0.10, tempo = 1.0, bpm = 0.0, effectiveBpm = 0.0, rate = 0.0;
    int semitones = 0;
    std::array<Cue, kHotCues> hotCues{};
    std::array<Cue, kMemoryCues> memoryCues{};
    int cueRevision = 0; // bumps when hot or memory cues change, for saving them
};

class DeckPlayer
{
  public:
    explicit DeckPlayer(int deckNumber);
    ~DeckPlayer();
    DeckPlayer(const DeckPlayer&) = delete;
    DeckPlayer& operator=(const DeckPlayer&) = delete;

    void prepare(double outputSampleRate, int maxBlock);

    // Audio thread ------------------------------------------------------------------------
    // `copy` (Instant Doubles) takes its position, play state, loop and tempo too.
    void load(const DeckTrack* track, const DeckPlayer* copy) noexcept;
    void handle(const Event& e) noexcept;
    void setSync(bool on) noexcept;
    void setMaster(bool on) noexcept { isMaster = on; }
    bool syncOn() const noexcept { return syncEnabled; }
    void syncPhaseNow(const SyncSource& master) noexcept; // a SYNC press: line up now
    // Renders `n` samples (pre-mixer) into out[0..1]; `audible` = it reaches the master now.
    void render(float* const* out, int n, const SyncSource& master, DeckRecorder* recorder, juce::int64 clock,
                bool audible) noexcept;
    void fillView(DeckView& view) const noexcept;
    void announce() noexcept { forceSegment = true; } // the next block starts a segment
    // The take ends here: the open segment is closed for the recorder.
    void endTake(DeckRecorder* r, juce::int64 at) noexcept;

    const DeckTrack* track() const noexcept { return loaded; }
    bool moving() const noexcept;
    double effectiveBpm() const noexcept;
    double beatPhase() const noexcept; // beats since the grid's anchor
    // For PAD modes the engine resolves: lengths in beats and the current position.
    double beatSeconds() const noexcept;

  private:
    struct Stretch;
    enum class Preview
    {
        none,
        cue,
        hotCue
    };

    // Positions are in source samples of the loaded track.
    double length() const noexcept;
    double sr() const noexcept;
    double beatLen() const noexcept; // samples per beat (0 = no grid)
    double anchor() const noexcept;
    double snapNearest(double p) const noexcept;
    double snapFloor(double p, double gridLen) const noexcept;
    double tempoFactor() const noexcept;
    void jumpTo(double p) noexcept;          // a discontinuity, cross-faded and recorded
    void startPlaying() noexcept;
    void stopPlaying() noexcept;
    void setLoop(double in, double out, bool jumpInside) noexcept;
    void exitLoop() noexcept { loopActive = false; loopAdjust = 0; }
    void cueButton(bool pressed) noexcept;
    void hotCueButton(int index, bool pressed) noexcept;
    void callMemory(int direction) noexcept;
    void storeMemory() noexcept;
    void deleteMemory() noexcept;
    void beatLoop(double beats) noexcept;
    void beatJump(double beats) noexcept;
    void loopResize(double factor) noexcept;
    void jog(Control control, double ticks) noexcept;
    void bumpCues() noexcept { ++cueRevision; }

    void renderVarispeed(float* const* out, int offset, int n, double targetRate) noexcept;
    void renderStretch(float* const* out, int offset, int n, double rate, double transpose) noexcept;
    void readWrapped(float* const* dest, double& cursor, int n) const noexcept; // loop-aware copy
    float sampleAt(int channel, double p) const noexcept;
    void advance(double& p, double delta, bool record, juce::int64 at) noexcept;
    void emitSegment(juce::int64 at, double endPos) noexcept;

    const int deck;
    double outRate = 44100.0;
    int maxBlock = 512;
    const DeckTrack* loaded = nullptr;

    double pos = 0.0, cuePoint = 0.0;
    bool playing = false;
    Preview preview = Preview::none;
    int previewHotCue = -1;
    double previewReturn = 0.0;

    bool loopActive = false, loopArmed = false, loopInPending = false;
    double loopIn = 0.0, loopOut = 0.0, pendingIn = 0.0;
    int loopAdjust = 0;

    float slider = 0.5F;
    double range = 0.10;
    double heldTempo = 1.0; // the tempo when sync was switched off, until the slider moves
    bool tempoHeld = false;
    bool syncEnabled = false, isMaster = false, keyLockOn = false, quantizeOn = true;
    double syncTempo = 1.0;
    int semitones = 0;

    // Jog
    bool touched = false;
    double platter = 0.0;     // where the hand has put the record (scratch target)
    double bendTicks = 0.0;   // decaying side-of-wheel motion
    int jogQuietBlocks = 0;
    double currentRate = 0.0; // smoothed, source samples per output sample
    double phaseCorrection = 0.0;

    std::array<Cue, kHotCues> hotCues{};
    std::array<Cue, kMemoryCues> memoryCues{};
    int memoryCursor = -1;
    int cueRevision = 0;

    // Cross-fade from the old position after a jump, pause or path change.
    struct Tail
    {
        bool active = false;
        double pos = 0.0, rate = 0.0;
        int left = 0;
    } tail;
    int fadeInLeft = 0;
    float runGain = 0.0F;

    std::unique_ptr<Stretch> stretch;
    bool stretchActive = false;

    // Recording
    DeckRecorder* recorder = nullptr;
    bool jumped = false;
    double jumpFrom = 0.0;
    struct RecKey
    {
        bool moving = false, audible = false, keyLock = false, scratch = false;
        int semitones = 0;
        bool operator!=(const RecKey& o) const noexcept
        {
            return moving != o.moving || audible != o.audible || keyLock != o.keyLock || scratch != o.scratch ||
                   semitones != o.semitones;
        }
    } lastKey;
    double segmentTempo = 1.0;
    juce::int64 segmentStart = 0;
    bool forceSegment = true;
    bool audibleNow = false;
    juce::int64 clockNow = 0;
};

} // namespace stemcel::perf
