#pragma once

// The performance (DJ) section of the audio engine: two decks, a DJ mixer (trim, 3-band EQ,
// FILTER, channel faders, crossfader) and a headphone cue bus, beside the timeline in the
// engine's top mixer. Controls arrive as Events (from a controller on its MIDI thread, or the
// view) and are applied at the start of each block on the audio thread; the state is published
// for the view and LEDs, and while recording, what happened is logged for the timeline.

#include "DeckPlayer.h"
#include "DjFx.h"
#include "PerfCurves.h"
#include "ToneEq.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace stemcel::perf
{

// A lock-free single-writer, single-reader latest-value hand-off.
template <typename T>
class TripleBuffer
{
  public:
    T& writeBuffer() noexcept { return buffers[static_cast<size_t>(back)]; }
    void publish() noexcept
    {
        back = middle.exchange(back | kDirty, std::memory_order_acq_rel) & kIndex;
    }
    // Returns the latest published value (or the previous one when nothing new arrived).
    const T& read() noexcept
    {
        if ((middle.load(std::memory_order_relaxed) & kDirty) != 0)
            front = middle.exchange(front, std::memory_order_acq_rel) & kIndex;
        return buffers[static_cast<size_t>(front)];
    }

  private:
    static constexpr int kDirty = 4, kIndex = 3;
    std::array<T, 3> buffers{};
    int back = 0, front = 1;
    std::atomic<int> middle{2};
};

// One thing the take recorder logged, in performance-clock samples.
struct RecEvent
{
    enum class Kind : std::uint8_t
    {
        segment,
        platter,
        param,
        clockRate,
        sample // a sampler slot: flags 1 = started, 0 = stopped; param = slot; a = its length (s)
    };
    juce::int64 at = 0;
    Kind kind = Kind::segment;
    std::uint8_t deck = 0;  // 1..kDecks, 0 = master, kSamplerDeck = the sampler
    std::uint8_t param = 0; // RecParam
    std::uint8_t flags = 0; // segment: 1 moving, 2 audible, 4 keyLock, 8 scratch
    std::int16_t semitones = 0;
    std::int32_t serial = 0;
    double a = 0.0, b = 0.0; // segment: endPos, startPos; platter: pos; param: value
    float tempo = 1.0F;
};

constexpr int kSamplerDeck = kDecks + 1;
constexpr int kSamplerSlots = 64; // 4 banks of 16

enum class RecParam : std::uint8_t
{
    levelDb,
    bassDb,
    midDb,
    trebleDb,
    filter,
    colorFxType,
    colorFx,
    beatFxType,
    beatFxBeats,
    beatFxLevel,
    beatFxOn,
    padFxType,
    padFxBeats,
    padFxLevel,
    padFxOn,
    fxBpm,
    count
};
// Pad FX presets (rekordbox's PAD FX1 / PAD FX2 banks of 16: pads, then SHIFT + pads).
struct PadFxPreset
{
    int type = 1;  // djfx::BeatFx
    int step = 5;  // djfx::kBeatSteps
    float level = 0.6F;
    bool release = false; // the Release FX pad
};
const PadFxPreset& padFxPreset(int bank, int pad, bool shifted) noexcept;
const char* recParamName(RecParam p) noexcept; // the timeline automation param id

struct ChannelView
{
    float trim = 0.5F, eqHi = 0.5F, eqMid = 0.5F, eqLow = 0.5F, filter = 0.5F, fader = 1.0F;
    bool hpCue = false;
    PadMode padMode = PadMode::hotCue;
    int beatJumpPage = 1, keyPage = 0, keyboardCue = -1;
    int padFxHeld = -1; // a Pad FX pad held (0..15), or -1
};

struct EngineView
{
    bool open = false;
    juce::int64 clock = 0;
    double sampleRate = 44100.0;
    std::array<DeckView, kDecks> decks{};
    std::array<ChannelView, kDecks> channels{};
    float crossfader = 0.5F;
    int masterDeck = 0; // 1..kDecks, 0 = none
    bool cueOutput = false; // headphones on outputs 3–4
    bool recording = false;
    // The Beat FX section and Sound Color FX.
    int fxType = 1, fxStep = djfx::kDefaultBeatStep, fxChannel = 0, colorType = 0, releaseType = 1;
    float fxLevel = 0.5F;
    bool fxOn = false, fxTapMode = false;
    double fxBpm = 120.0;
    // The sampler: its bank, volume, and the slots loaded and playing (bit per slot).
    int samplerBank = 0;
    float samplerVolume = 0.8F;
    std::uint64_t samplesLoaded = 0, samplesPlaying = 0;
    std::array<int, kSamplerSlots> sampleSerials{};
};

class PerformanceEngine final : public juce::AudioSource
{
  public:
    PerformanceEngine();
    ~PerformanceEngine() override;

    // Message thread ------------------------------------------------------------------------
    void setOpen(bool open);
    bool isOpen() const noexcept { return opened.load(std::memory_order_acquire); }
    void setRecording(bool on);
    // From any thread but the audio thread (MIDI, message).
    void post(const Event& e);
    // `track` stays alive here until no deck holds it; `copyFromDeck` > 0 = Instant Doubles.
    void load(int deck, std::shared_ptr<const DeckTrack> track, int copyFromDeck = 0);
    // A sampler slot's sample (0..kSamplerSlots-1); null empties it.
    void loadSample(int slot, std::shared_ptr<const DeckTrack> sample);
    const EngineView& view() { return views.read(); }
    // Takes the recorder's events since the last call.
    void drainRecording(std::vector<RecEvent>& out);
    // Post-fader channel peaks since the last call (for meters and the controller's LEDs).
    void consumePeaks(std::array<float, kDecks>& peaks);
    void setCrossfaderCurve(curves::CrossfaderCurve c) noexcept { crossfaderCurve.store(static_cast<int>(c)); }
    void setCueOutputAvailable(bool on) noexcept { cueOutputAvailable.store(on); }
    void collectGarbage();

    // Audio thread ----------------------------------------------------------------------------
    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void releaseResources() override {}
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override;
    // The headphone mix of the block just rendered, into `buffer`'s channels from `firstChannel`.
    void renderCue(juce::AudioBuffer<float>& buffer, int firstChannel, int startSample, int numSamples) noexcept;

  private:
    struct Queued
    {
        Event event;
        const DeckTrack* track = nullptr;
        int copyFrom = 0;
        bool isLoad = false;
        int sampleSlot = -1; // a sampler load
    };
    struct Channel
    {
        ToneEq eq;
        juce::LinearSmoothedValue<float> gain;
        float trim = 0.5F, eqHi = 0.5F, eqMid = 0.5F, eqLow = 0.5F, filter = 0.5F, fader = 1.0F;
        bool hpCue = false;
        PadMode padMode = PadMode::hotCue;
        int beatJumpPage = 1, keyPage = 0, keyboardCue = -1;
        int padFxHeld = -1;
        int padFxRestoreStep = -1; // BEAT ◁/▷ during a held pad changes it until it is let go
        djfx::DjFxChain fx;
        bool wasMoving = false;
        // The recorder's last values per param, and when they went out.
        std::array<double, static_cast<size_t>(RecParam::count)> sent{};
        std::array<juce::int64, static_cast<size_t>(RecParam::count)> sentAt{};
        std::array<bool, static_cast<size_t>(RecParam::count)> pending{};
    };
    class Recorder;

    void pushRec(const RecEvent& r) noexcept;
    void apply(const Queued& q) noexcept;
    void applyPad(int deck, const Event& e, bool shifted) noexcept;
    void syncButton(int deck, bool longPress) noexcept;
    void updateMaster() noexcept;
    SyncSource syncSourceFor(int deck) const noexcept;
    double channelGain(int deck) const noexcept;
    void recordParams(int deck, juce::int64 at, bool force) noexcept;
    void recordMasterParams(juce::int64 at, bool force) noexcept;
    void beatFxControl(const Event& e) noexcept;
    void applyBeatFx() noexcept;
    void triggerSample(int slot, bool start) noexcept;
    void renderSampler(float* const* out, int numOutChannels, int n) noexcept;
    double fxBpmFor(int deck) const noexcept;
    void renderBlock(float* const* out, int numOutChannels, int n) noexcept;
    void publishView() noexcept;

    std::array<std::unique_ptr<DeckPlayer>, kDecks> decks;
    std::array<Channel, kDecks> channels;
    float crossfader = 0.5F;
    int masterDeck = 0;
    // Beat FX section
    int fxType = 1, fxStep = djfx::kDefaultBeatStep, fxChannel = 0, colorType = 0, releaseType = 1;
    float fxLevel = 0.5F;
    bool fxOn = false, fxTapMode = false;
    double tapBpm = 0.0;
    juce::int64 lastTapAt = -1;
    juce::int64 releaseLeft = 0; // a Release FX playing out (samples)
    int releaseUnit = -1;        // 0 = master, 1..kDecks = that channel's Beat FX
    djfx::BeatFxUnit masterFx;
    std::array<double, static_cast<size_t>(RecParam::count)> masterSent{};
    std::array<juce::int64, static_cast<size_t>(RecParam::count)> masterSentAt{};
    // Sampler
    struct Voice
    {
        const DeckTrack* sample = nullptr;
        double pos = 0.0;
        bool playing = false;
    };
    std::array<Voice, kSamplerSlots> voices{};
    int samplerBank = 0;
    float samplerVolume = 0.8F;
    double samplerSentDb = 1000.0;
    double rate = 44100.0;
    int maxBlock = 512;
    juce::int64 clock = 0;
    bool wasRecording = false;
    juce::AudioBuffer<float> deckBuffer, cueBuffer;
    int cueSamples = 0;

    std::atomic<bool> opened{false}, recordingOn{false}, recordingRestart{false};
    std::atomic<bool> cueOutputAvailable{false};
    std::atomic<int> crossfaderCurve{static_cast<int>(curves::CrossfaderCurve::smooth)};

    // Controls in (many writers, the audio thread reads)
    static constexpr int kQueueSize = 4096;
    std::vector<Queued> queue = std::vector<Queued>(kQueueSize); // on the heap: the engine may live on a stack
    juce::AbstractFifo queueFifo{kQueueSize};
    juce::SpinLock writeLock;

    // The recorder's log out (the audio thread writes, the message thread reads)
    static constexpr int kRecSize = 32768;
    std::vector<RecEvent> recQueue = std::vector<RecEvent>(kRecSize);
    juce::AbstractFifo recFifo{kRecSize};
    std::unique_ptr<Recorder> recorder;
    std::atomic<int> recDropped{0};

    TripleBuffer<EngineView> views;
    std::array<std::atomic<float>, kDecks> peaks{};

    // Tracks the decks may hold (message thread).
    std::mutex tracksMutex;
    std::vector<std::shared_ptr<const DeckTrack>> tracks;
};

} // namespace stemcel::perf
