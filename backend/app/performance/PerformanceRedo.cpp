// Redo from here: going back over a recorded take in real time.
//
// REDO binds each channel to its deck's track of the take under the playhead and plays the
// timeline from there. Untouched, the take plays as it was. A knob or fader moved takes over
// once it reaches the take's value (TrackDriver's pickup), and is written from that moment to
// the stop, ramping back to the take over a beat. A deck control (tempo, sync, loops, hot cues
// and pads, the jog while playing) takes the deck over: it loads what the take was playing at
// the point it had reached, at its tempo, and plays on live while the take's track is silenced.
//
// At the stop (REC on), the deck's playing becomes the take's from the takeover to the stop,
// and the rest of the take joins on: where the take plays the same moment of the same track the
// deck stopped on, the timeline is spliced there (everything after moves up or back by the
// difference). With no such moment, the rest of the deck's track can be replaced or left.
// With REC off a pass is a rehearsal: KEEP writes the last one's moves.

#include "PerformanceView.h"

#include "Log.h"
#include "TimeEdit.h"
#include "Theme.h"

namespace stemcel::native::perf
{
namespace
{
// A deck control that changes what the deck plays, so touching it takes the deck over.
bool takesTheDeck(const juce::String& control, bool timelinePlaying)
{
    static const juce::StringArray controls{"tempo", "sync", "syncLong", "loopIn", "loopInLong", "loopOut",
                                            "reloopExit", "callPrev", "callNext", "pad", "padShift"};
    if (controls.contains(control)) return true;
    return timelinePlaying && (control == "jogBend" || control == "jogScratch");
}

double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }

juce::var obj(std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();
    for (const auto& [k, v] : props) o->setProperty(k, v);
    return juce::var(o);
}
} // namespace

void PerformanceView::setUpRedo()
{
    redoButton.setClickingTogglesState(true);
    redoButton.setTooltip("Redo from here: the take under the playhead plays as it was until you touch a control, then you "
                          "take over; with REC on, each pass is written when the timeline stops");
    redoButton.onClick = [this]
    {
        if (redoButton.getToggleState()) startRedo();
        else stopRedo();
    };
    addAndMakeVisible(redoButton);
    keepButton.setTooltip("Write the last rehearsed pass (REC was off)");
    keepButton.onClick = [this]
    {
        if (driver.keepPass() && ctx.toast) ctx.toast("The pass is written into the take", true);
    };
    addChildComponent(keepButton);
    joinLight.setText("JOIN", juce::dontSendNotification);
    joinLight.setJustificationType(juce::Justification::centred);
    joinLight.setFont(uiFont(12.0F, true));
    joinLight.setTooltip("Lit when stopping now joins the deck back onto the take seamlessly");
    addChildComponent(joinLight);
}

// The take's deck tracks around the playhead: "Deck 1…" and "Deck 2…" with a clip there (the
// last take's, when there are several).
std::array<juce::String, kDecks> PerformanceView::takeTracksAt(double ms) const
{
    std::array<juce::String, kDecks> out;
    for (const auto& t : ctx.mirror.model().tracks)
        for (int d = 1; d <= kDecks; ++d)
            if (t.name.startsWith("Deck " + juce::String(d)))
                for (const auto& c : t.clips)
                    if (c.offsetMs <= ms + 2000.0 && c.endMs() >= ms) out[static_cast<size_t>(d - 1)] = t.id;
    return out;
}

void PerformanceView::startRedo()
{
    const double at = ctx.live.positionMs;
    const auto tracks = takeTracksAt(at);
    if (std::all_of(tracks.begin(), tracks.end(), [](const juce::String& t) { return t.isEmpty(); }))
    {
        redoButton.setToggleState(false, juce::dontSendNotification);
        if (ctx.toast) ctx.toast("Put the timeline's playhead just before the stretch to redo, in a recorded take", false);
        return;
    }
    redo = Redo{};
    redo.active = true;
    redo.tracks = tracks;
    redo.write = recButton.getToggleState();
    driver.setHoldPasses(!redo.write);
    driver.dropPass();
    for (int d = 1; d <= kDecks; ++d)
    {
        const auto& id = tracks[static_cast<size_t>(d - 1)];
        if (id.isEmpty()) continue;
        for (size_t i = 0; i < targetIds.size(); ++i)
            if (targetIds[i] == id) targetBoxes[static_cast<size_t>(d - 1)]->setSelectedId(static_cast<int>(i) + 2, juce::sendNotification);
    }
    if (ctx.toast)
        ctx.toast(juce::String("Redo: the take plays as it was until you touch a control") + (redo.write ? "" : " (REC off: a rehearsal, KEEP writes it)"),
                  true);
    ctx.actions.play();
}

void PerformanceView::stopRedo()
{
    if (!redo.active) return;
    if (ctx.live.playing) ctx.actions.pause(); // the pass ends (and is written) as the timeline stops
    endPass();
    redo.active = false;
    driver.setHoldPasses(false);
    for (int d = 1; d <= kDecks; ++d) targetBoxes[static_cast<size_t>(d - 1)]->setSelectedId(1, juce::sendNotification);
    redoButton.setToggleState(false, juce::dontSendNotification);
    joinLight.setVisible(false);
}

void PerformanceView::beginPass()
{
    redo.running = true;
    redo.passStartMs = ctx.live.positionMs;
    redo.passStartClock = state.clock;
    for (auto& t : redo.takeovers) t = Takeover{};
    // The take's clips as they are now, to find where the deck joins back on.
    for (int d = 0; d < kDecks; ++d)
    {
        redo.original[static_cast<size_t>(d)].clear();
        if (const auto* track = ctx.mirror.model().findTrack(redo.tracks[static_cast<size_t>(d)]))
            redo.original[static_cast<size_t>(d)] = track->clips;
    }
    if (redo.write)
    {
        // The decks' playing is recorded from here, for a takeover: the recording restarts, so
        // the take begins at the pass (the engine's start event clears it).
        ctx.engine.send("PERF_RECORD", obj({{"on", false}}));
        ctx.engine.send("PERF_RECORD", obj({{"on", true}}));
    }
}

void PerformanceView::takeOver(int deck)
{
    auto& over = redo.takeovers[static_cast<size_t>(deck - 1)];
    if (over.on) return;
    const auto trackId = driver.boundTrack(deck);
    const double at = ctx.live.positionMs;
    const auto* track = ctx.mirror.model().findTrack(trackId);
    const ClipModel* clip = nullptr;
    if (track != nullptr)
        for (const auto& c : track->clips)
            if (c.offsetMs <= at && c.endMs() > at && !c.raw.getProperty("reversed", false)) clip = &c;
    if (clip == nullptr) return; // nothing playing on the take there: the channel stays on the track
    over.on = true;
    over.loading = true;
    over.atMs = at;
    over.trackId = trackId;
    over.itemId = clip->libraryItemId;
    over.clipOffsetMs = clip->offsetMs;
    over.clipInMs = clip->inMs;
    over.ratio = clip->effectiveTempoRatio > 0.0 ? clip->effectiveTempoRatio : 1.0;
    over.keyLock = clip->raw.getProperty("warpMode", "").toString() == "pro";
    over.serialBefore = infos[static_cast<size_t>(deck - 1)].serial;
    // The channel goes back to its deck (the take's track plays on until the deck starts),
    // at once: the deck's PLAY must not reach the timeline.
    driver.bind(deck, {});
    targetBoxes[static_cast<size_t>(deck - 1)]->setSelectedId(1, juce::dontSendNotification);
    decks[static_cast<size_t>(deck - 1)].display->showDeck();
    loadItem(over.itemId, deck);
    if (!redo.write && ctx.toast) ctx.toast("Deck " + juce::String(deck) + " taken over (REC is off: it plays, it isn't written)", true);
}

void PerformanceView::redoTick()
{
    if (!redo.active) return;
    keepButton.setVisible(driver.hasHeldPass());
    const bool playing = ctx.live.playing;
    if (playing && !redo.running) beginPass();
    if (!playing && redo.running) endPass();
    // A deck taken over: once loaded, to where the take had got, at its tempo, playing.
    bool anyOver = false, joinable = false;
    for (int d = 1; d <= kDecks; ++d)
    {
        auto& over = redo.takeovers[static_cast<size_t>(d - 1)];
        if (!over.on) continue;
        anyOver = true;
        const auto& s = state.decks[static_cast<size_t>(d - 1)];
        const auto& info = infos[static_cast<size_t>(d - 1)];
        if (over.loading && s.loaded && info.serial != over.serialBefore && info.itemId == over.itemId)
        {
            over.loading = false;
            // The deck starts where the take has got to now, and the take's track falls silent:
            // the takeover runs from this moment, with no gap while it loaded.
            over.atMs = ctx.live.positionMs;
            ctx.engine.send("PERF_TRACK_LIVE", obj({{"trackId", over.trackId}, {"values", obj({{"level", -60.0}})}}));
            const double sourceSec = (over.clipInMs + (ctx.live.positionMs - over.clipOffsetMs) * over.ratio) / 1000.0;
            sendControl(ctx.engine, "keyLock", d, over.keyLock ? 1.0 : 0.0);
            sendControl(ctx.engine, "tempoHold", d, over.ratio);
            sendControl(ctx.engine, "seek", d, sourceSec);
            if (!s.playing)
            {
                sendControl(ctx.engine, "play", d, 1.0);
                sendControl(ctx.engine, "play", d, 0.0);
            }
        }
        joinable = joinable || joinFor(d).has_value();
    }
    joinLight.setVisible(anyOver);
    joinLight.setColour(juce::Label::textColourId, joinable ? colours::emerald500 : colours::textFaint);
}

// Where the take plays the moment deck `deck` is playing now: the same track at the same
// position, at (about) the same tempo. Empty when it never does.
std::optional<PerformanceView::Join> PerformanceView::joinFor(int deck) const
{
    const auto& over = redo.takeovers[static_cast<size_t>(deck - 1)];
    const auto& s = state.decks[static_cast<size_t>(deck - 1)];
    if (!over.on || over.loading || !s.loaded) return std::nullopt;
    const auto& info = infos[static_cast<size_t>(deck - 1)];
    const double posMs = (s.posSec + (s.playing ? (nowMs() - decks[static_cast<size_t>(deck - 1)].receivedAt) / 1000.0 * s.rate : 0.0)) * 1000.0;
    std::optional<Join> best;
    for (const auto& c : redo.original[static_cast<size_t>(deck - 1)])
    {
        if (c.libraryItemId != info.itemId) continue;
        const double ratio = c.effectiveTempoRatio > 0.0 ? c.effectiveTempoRatio : 1.0;
        if (posMs < c.inMs || posMs > c.inMs + c.durationMs) continue;
        if (std::abs(s.tempo - ratio) > 0.02 * ratio) continue;
        const double atMs = c.offsetMs + (posMs - c.inMs) / ratio;
        if (atMs < over.atMs - 1.0) continue; // before the takeover: not a way on
        const Join j{atMs, ctx.live.positionMs, c.libraryItemId, c.inMs + (atMs - c.offsetMs) * ratio, ratio};
        if (!best.has_value() || std::abs(j.originalMs - j.stopMs) < std::abs(best->originalMs - best->stopMs)) best = j;
    }
    return best;
}

void PerformanceView::endPass()
{
    if (!redo.running) return;
    redo.running = false;
    const double stopMs = ctx.live.positionMs;
    std::map<int, juce::String> onto;
    std::vector<std::pair<int, std::optional<Join>>> joins;
    for (int d = 1; d <= kDecks; ++d)
    {
        auto& over = redo.takeovers[static_cast<size_t>(d - 1)];
        if (!over.on) continue;
        onto[d] = over.trackId;
        auto join = joinFor(d);
        if (join.has_value()) join->stopMs = stopMs;
        joins.emplace_back(d, join);
        // The take's track sounds again; the deck stops.
        ctx.engine.send("PERF_TRACK_LIVE", obj({{"trackId", over.trackId}, {"all", true}}));
        if (state.decks[static_cast<size_t>(d - 1)].playing)
        {
            sendControl(ctx.engine, "play", d, 1.0);
            sendControl(ctx.engine, "play", d, 0.0);
        }
    }
    if (onto.empty() || !redo.write)
    {
        if (redo.write) ctx.engine.send("PERF_RECORD", obj({{"on", false}}));
        rebindAfterPass();
        return;
    }
    // The deck's playing over the stretch it took, onto its track, and the join: after the
    // engine's last events (the closing segments) arrive.
    ctx.engine.send("PERF_RECORD", obj({{"on", false}}));
    const auto takeovers = redo.takeovers;
    const double passStartMs = redo.passStartMs, passStartClock = redo.passStartClock;
    juce::Component::SafePointer<PerformanceView> safe(this);
    juce::Timer::callAfterDelay(150, [safe, takeovers, onto, joins, stopMs, passStartMs, passStartClock]
    {
        if (safe == nullptr) return;
        auto& v = *safe;
        const double rate = v.state.sampleRate > 0.0 ? v.state.sampleRate : 44100.0;
        const double startMs = passStartMs + (v.take.startClock() - passStartClock) / rate * 1000.0;
        log::info("performance", "redo pass: take from " + juce::String(startMs, 0) + " ms, audio " + (v.take.hasAudio() ? "yes" : "no") + ", "
                                     + juce::String(static_cast<int>(v.take.edits(startMs).clips.size())) + " clip(s), stopped at " + juce::String(stopMs, 0));
        bool needsChoice = false;
        juce::String choiceTrack;
        double choiceFrom = 0.0;
        v.ctx.engine.inUndoGroup("Redo pass", [&]
        {
            auto replace = [&]
            {
                for (const auto& [deck, trackId] : onto)
                    v.clearRegion(trackId, takeovers[static_cast<size_t>(deck - 1)].atMs, stopMs);
            };
            if (v.take.hasAudio()) v.take.write(v.ctx.engine, v.ctx.mirror, v.ctx.actions, startMs, ++v.takeNumber, replace, onto, "Redo pass");
            v.ctx.mirror.refreshNow();
            // The join: the first deck taken over decides (the timeline moves for everything).
            if (!joins.empty())
            {
                const auto& [deck, join] = joins.front();
                const auto& trackId = onto.at(deck);
                if (join.has_value())
                {
                    const double delta = join->originalMs - stopMs;
                    if (delta > 1.0)
                        timeedit::deleteTime(v.ctx.actions, v.ctx.mirror, stopMs, stopMs + delta); // the take had got there later
                    else if (delta < -1.0)
                    {
                        // The take had got there sooner: room for the stretch it played since,
                        // which the deck's pass replaced, put back after the stop.
                        timeedit::insertTime(v.ctx.actions, v.ctx.mirror, stopMs, -delta);
                        const auto clipId = EngineClient::newId("clip");
                        v.ctx.engine.send("CLIP_ADD", obj({{"trackId", trackId}, {"clipId", clipId}, {"libraryItemId", join->itemId},
                                                           {"positionMs", stopMs}, {"inMs", join->sourceMs}, {"durationMs", -delta * join->ratio}}));
                        const bool timed = std::abs(join->ratio - 1.0) > 1.0e-4;
                        v.ctx.engine.send("CLIP_SET_WARP", obj({{"clipId", clipId}, {"warpEnabled", timed}, {"warpMode", "repitch"},
                                                                {"tempoRatio", join->ratio}, {"semitones", 0}, {"cents", 0}}));
                    }
                }
                else
                {
                    needsChoice = true;
                    choiceTrack = trackId;
                    choiceFrom = stopMs;
                }
            }
        });
        v.take.clear();
        v.rebindAfterPass();
        if (needsChoice)
        {
            // No moment in the take matches where the deck stopped.
            auto options = juce::MessageBoxOptions()
                               .withIconType(juce::MessageBoxIconType::QuestionIcon)
                               .withTitle("The take can't join on here")
                               .withMessage("The deck stopped on a moment the rest of the take never plays (a different track, "
                                            "position or tempo). Replace the rest of this deck's track, or leave it as it was?")
                               .withButton("Replace the rest")
                               .withButton("Leave it");
            juce::AlertWindow::showAsync(options, [safe, choiceTrack, choiceFrom](int result)
            {
                if (safe == nullptr || result != 1) return;
                safe->ctx.engine.inUndoGroup("Replace the rest of the take",
                                             [&] { safe->clearRegion(choiceTrack, choiceFrom, 1.0e12); });
            });
        }
        else if (v.ctx.toast)
            v.ctx.toast("Redo pass written" + juce::String(joins.empty() || !joins.front().second.has_value() ? "" : ", joined back onto the take"), true);
    });
}

void PerformanceView::rebindAfterPass()
{
    // The channels back on the take's tracks, for the next pass.
    if (!redo.active) return;
    for (int d = 1; d <= kDecks; ++d)
    {
        const auto& id = redo.tracks[static_cast<size_t>(d - 1)];
        if (id.isEmpty() || driver.boundTrack(d) == id) continue;
        refreshTargets();
        for (size_t i = 0; i < targetIds.size(); ++i)
            if (targetIds[i] == id) targetBoxes[static_cast<size_t>(d - 1)]->setSelectedId(static_cast<int>(i) + 2, juce::sendNotification);
    }
    for (auto& t : redo.takeovers) t = Takeover{};
}

bool PerformanceView::redoTakesOver(const juce::String& control, int deck)
{
    if (!redo.active || !redo.running || deck < 1 || deck > kDecks || driver.boundTrack(deck).isEmpty()) return false;
    if (!takesTheDeck(control, ctx.live.playing)) return false;
    takeOver(deck);
    return true;
}

} // namespace stemcel::native::perf
