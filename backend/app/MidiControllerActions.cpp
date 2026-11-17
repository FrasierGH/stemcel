#include "MidiControllerActions.h"

#include "Db.h"
#include "Log.h"

#include <algorithm>
#include <cmath>

namespace stemcel::native
{
namespace
{
// Timeline ms per jog step, and how fast a grid-snapped jog may walk (per control).
double jogMsPerStep(const juce::String& control)
{
    if (control == "jogScratch") return 14.0;
    if (control == "jogPitchBend" || control == "wheelPitchBend") return 32.0;
    return 128.0; // jogSearch, wheelSearch
}

double snappedMinIntervalMs(const juce::String& control)
{
    return control == "jogSearch" || control == "wheelSearch" ? 10.0 : 20.0;
}

constexpr double kMarkerSkipEpsilonMs = 1.0;
constexpr double kGridStepEpsilon = 1e-6;

double now()
{
    return juce::Time::getMillisecondCounterHiRes();
}

juce::var object(std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();
    for (const auto& [k, v] : props) o->setProperty(k, v);
    return juce::var(o);
}
} // namespace

MidiControllerActions::MidiControllerActions(EngineClient& client, ProjectMirror& projectMirror, TimelineActions& timelineActions,
                                             TimelineViewState& viewState, LiveState& liveState, AppPreferences& preferences,
                                             Hooks h)
    : engine(client), mirror(projectMirror), actions(timelineActions), view(viewState), live(liveState), prefs(preferences),
      hooks(std::move(h)), subscriptions(client), deviceScan([this] { engine.send("MIDI_DEVICES_REQUEST"); })
{
    subscriptions.on("MIDI_CONTROL", [this](const juce::var& p) { handleControl(p); });
    subscriptions.on("MIDI_DEVICES_LIST", [this](const juce::var& p) { devicesList(p); });
    subscriptions.on("MIDI_DECK_SELECTION", [this](const juce::var& p) { deckSelection(p); });
    if (const auto last = engine.latest("MIDI_DEVICES_LIST"); !last.isVoid()) devicesList(last);
    engine.send("MIDI_DEVICES_REQUEST");
    deviceScan.startTimer(3000);
}

MidiControllerActions::~MidiControllerActions()
{
    deviceScan.stopTimer();
    stopTimer();
}

void MidiControllerActions::rescan()
{
    autoEnableTried.clear();
    engine.send("MIDI_DEVICES_REQUEST");
}

void MidiControllerActions::autoEnable(const juce::var& devices)
{
    const auto* inputs = devices.getProperty("inputs", {}).getArray();
    if (inputs == nullptr) return;
    // The smoke test drives MIDI by injection; a controller on the desk stays out of it.
    if (juce::JUCEApplicationBase::getCommandLineParameters().contains("--smoke-test")) return;
    const auto saved = prefs.getSection("enabledMidiInputs");
    bool changed = false;
    for (const auto& input : *inputs)
    {
        const auto id = input.getProperty("identifier", {}).toString();
        if (id.isEmpty() || static_cast<bool>(input.getProperty("enabled", false))) continue;
        const bool supported = !input.getProperty("controllerProfile", {}).isVoid()
                               || input.getProperty("performanceMapping", "").toString().isNotEmpty();
        if (!supported || autoEnableTried.count(id) != 0) continue;
        const auto choice = saved.getProperty(id, {});
        if (!choice.isVoid() && !static_cast<bool>(choice)) continue; // switched off by the user
        autoEnableTried.insert(id);
        if (choice.isVoid()) prefs.set("enabledMidiInputs", id, true);
        changed = true;
        stemcel::log::info("midi", "enabling " + input.getProperty("name", "").toString() + " for the timeline");
    }
    if (!changed) return;
    prefs.save();
    juce::Array<juce::var> ids;
    if (const auto* o = prefs.getSection("enabledMidiInputs").getDynamicObject())
        for (const auto& p : o->getProperties())
            if (static_cast<bool>(p.value)) ids.add(p.name.toString());
    // Performance mode opens its controllers itself, and gives them back when it closes.
    if (hooks.performanceOpen && hooks.performanceOpen()) return;
    engine.send("MIDI_INPUTS_SET", object({{"identifiers", ids}}));
}

void MidiControllerActions::handleControl(const juce::var& p)
{
    const auto kind = p.getProperty("kind", {}).toString();
    const auto control = p.getProperty("control", {}).toString();
    const auto device = p.getProperty("deviceIdentifier", {}).toString();
    const auto deckVar = p.getProperty("deck", {});
    const int deck = deckVar.isVoid() ? 0 : static_cast<int>(deckVar);
    const bool pressed = static_cast<bool>(p.getProperty("pressed", false));

    // Modifier state is tracked even while blocked, so a held Sync is not lost.
    if (kind == "button" && control == "syncModifier" && deck >= 1 && deck <= 2) syncPressed[deck] = pressed;

    // A modal or editor makes MIDI inert, but Cue still builds the scratch backing.
    const bool blocked = hooks.blocked && hooks.blocked();
    if (blocked && !wasBlocked) suspend();
    wasBlocked = blocked;
    const bool scratchCue = kind == "button" && control == "previousMarker" && pressed && hooks.cueScratch;
    if (blocked && !scratchCue) return;

    if (kind == "relative")
    {
        const double value = static_cast<double>(p.getProperty("value", 0.0));
        if (control == "browseTracks")
            browseRotation(device, value, false);
        else if (control == "timelineZoom")
        {
            if (browseRotation(device, -value, true)) return;
            if (hooks.zoom) hooks.zoom(value > 0 ? 1.25 : 0.8);
        }
        else if (deck >= 1 && deck <= 2)
            queueJog(control, value, syncPressed[deck], device);
        return;
    }
    // On the timeline the controller only navigates: its knobs and faders (gain, EQ, filter,
    // the master level) change nothing that is heard. Performance mode is where they mix.
    if (kind == "absolute") return;
    if (kind == "button" && control == "jogTouch")
    {
        if (deck >= 1 && deck <= 2) jogTouch(device, deck, pressed);
        return;
    }
    if (kind != "button" || !pressed) return;

    if (control == "playPause")
    {
        if (hooks.togglePlay) hooks.togglePlay();
    }
    else if (control == "browsePress")
        browsePress(device);
    else if (control == "previousMarker")
    {
        if (hooks.cueScratch && hooks.cueScratch()) return;
        if (!blocked) previousMarker();
    }
    else if (control == "nextMarker")
        nextMarker();
    else if (control == "markerJump")
        markerJump(static_cast<int>(p.getProperty("pad", 0)) - 1);
    else if (control == "markerToggle")
        toggleMarker(); // the keyboard M: toggle a marker at the exact playhead
}

// ── timer: jog flushes, one per frame ─────────────────────────────────────────
void MidiControllerActions::schedule()
{
    if (!isTimerRunning()) startTimerHz(60);
}

void MidiControllerActions::timerCallback()
{
    const double t = now();
    if (jogPending) flushJog(t);
    if (!jogPending) stopTimer();
}

// ── jog ───────────────────────────────────────────────────────────────────────
void MidiControllerActions::queueJog(const juce::String& control, double delta, bool snapToGrid, const juce::String& device)
{
    const juce::String mode = snapToGrid ? "snapped" : "free";
    if (pendingJogMode.isNotEmpty() && pendingJogMode != mode)
    {
        pendingSeekDeltaMs = 0.0;
        pendingBeatSteps = 0.0;
    }
    pendingJogMode = mode;
    pendingJogDevice = device;
    if (snapToGrid)
    {
        pendingSnapMinIntervalMs = snappedMinIntervalMs(control);
        if (snappedJogUnits != 0.0 && (snappedJogUnits > 0) != (delta > 0)) snappedJogUnits = 0.0;
        pendingBeatSteps += delta;
    }
    else
    {
        snappedJogUnits = 0.0;
        pendingSeekDeltaMs += delta * jogMsPerStep(control);
    }
    jogPending = true;
    schedule();
}

void MidiControllerActions::flushJog(double t)
{
    jogPending = false;
    const double end = model().contentEndMs();
    double unclamped = live.positionMs + pendingSeekDeltaMs;
    if (pendingJogMode == "snapped" && pendingBeatSteps != 0.0)
    {
        snappedJogUnits += pendingBeatSteps;
        if (std::abs(snappedJogUnits) >= 1.0 && t - lastSnappedSeekAt >= pendingSnapMinIntervalMs)
        {
            // A stepped jog walks the grid exactly like the arrow keys (musicTime stepToGridMs),
            // including a Free grid, where the step is relative (one sub-beat).
            const int direction = snappedJogUnits > 0 ? 1 : -1;
            snappedJogUnits -= direction;
            lastSnappedSeekAt = t;
            const double unit = actions.snapStepMs();
            const double bpm = model().bpm > 0.0 ? model().bpm : 120.0;
            if (unit <= 0.0)
                unclamped = juce::jmax(0.0, live.positionMs + direction * 60000.0 / bpm / 4.0);
            else
                unclamped = direction < 0 ? juce::jmax(0.0, std::floor((live.positionMs - kGridStepEpsilon) / unit) * unit)
                                          : (std::floor(live.positionMs / unit + kGridStepEpsilon) + 1.0) * unit;
        }
    }
    pendingSeekDeltaMs = 0.0;
    pendingBeatSteps = 0.0;
    pendingJogMode = {};
    const auto device = std::exchange(pendingJogDevice, {});
    const double position = juce::jmax(0.0, end > 0.0 ? juce::jmin(end, unclamped) : unclamped);
    const double delta = position - live.positionMs;
    if (delta == 0.0) return;
    live.positionMs = position; // so the next frame's deltas build on this one
    if (!holds.empty() && device.isNotEmpty() && scrubAudioEnabled(device))
        engine.send("TRANSPORT_SCRUB", object({{"positionMs", position}, {"deltaMs", delta}}));
    else
        actions.seek(position);
    if (hooks.reveal) hooks.reveal(position);
}

// ── browse ────────────────────────────────────────────────────────────────────
std::vector<const ClipModel*> MidiControllerActions::orderedClips(const juce::String& trackId) const
{
    std::vector<const ClipModel*> out;
    if (const auto* track = model().findTrack(trackId))
        for (const auto& c : track->clips) out.push_back(&c);
    std::stable_sort(out.begin(), out.end(), [](const ClipModel* a, const ClipModel* b) { return a->offsetMs < b->offsetMs; });
    return out;
}

MidiControllerActions::ClipBrowse* MidiControllerActions::activeClipBrowse(const juce::String& device)
{
    auto it = clipBrowse.find(device);
    if (it == clipBrowse.end()) return nullptr;
    if (view.selectedTrackId != it->second.trackId || model().findClip(it->second.cursorClipId) == nullptr)
    {
        clipBrowse.erase(it);
        return nullptr;
    }
    return &it->second;
}

void MidiControllerActions::browseTracks(double delta)
{
    const auto& tracks = model().tracks;
    if (tracks.empty() || delta == 0.0) return;
    const auto at = std::find_if(tracks.begin(), tracks.end(), [this](const TrackModel& t) { return t.id == view.selectedTrackId; });
    const juce::String next = at == tracks.end()
                                  ? tracks.front().id
                                  : tracks[static_cast<size_t>(juce::jlimit(0, static_cast<int>(tracks.size()) - 1,
                                                                            static_cast<int>(at - tracks.begin()) + (delta > 0 ? 1 : -1)))]
                                        .id;
    if (next == view.selectedTrackId) return;
    view.selectedTrackId = next;
    view.changed();
    if (hooks.revealTrack) hooks.revealTrack(next);
}

bool MidiControllerActions::browseRotation(const juce::String& device, double delta, bool extendSelection)
{
    if (auto* state = activeClipBrowse(device))
    {
        if (delta == 0.0) return true;
        const auto clips = orderedClips(state->trackId);
        const auto at = std::find_if(clips.begin(), clips.end(), [state](const ClipModel* c) { return c->id == state->cursorClipId; });
        if (at == clips.end()) return true;
        const int index = juce::jlimit(0, static_cast<int>(clips.size()) - 1,
                                       static_cast<int>(at - clips.begin()) + (delta > 0 ? 1 : -1));
        const auto* next = clips[static_cast<size_t>(index)];
        if (next->id == state->cursorClipId) return true;
        state->cursorClipId = next->id;
        if (extendSelection)
        {
            // Extend from the anchor (the earliest selected clip on the track) to this one.
            double lo = next->offsetMs, hi = next->offsetMs;
            for (const auto* c : clips)
                if (view.selectedClipIds.count(c->id) != 0) lo = juce::jmin(lo, c->offsetMs), hi = juce::jmax(hi, c->offsetMs);
            for (const auto* c : clips)
                if (c->offsetMs >= lo && c->offsetMs <= hi) view.selectedClipIds.insert(c->id);
        }
        else
            view.selectedClipIds = {next->id};
        view.changed();
        if (hooks.reveal) hooks.reveal(next->offsetMs);
        return true;
    }
    if (extendSelection) return false; // shifted rotation in track mode keeps its zoom action
    browseTracks(delta);
    return true;
}

void MidiControllerActions::browsePress(const juce::String& device)
{
    if (auto* state = activeClipBrowse(device))
    {
        const auto trackId = state->trackId;
        clipBrowse.erase(device);
        view.selectedClipIds.clear();
        view.selectedTrackId = trackId;
        view.changed();
        if (hooks.revealTrack) hooks.revealTrack(trackId);
        return;
    }
    const auto trackId = view.selectedTrackId;
    const auto clips = orderedClips(trackId);
    if (clips.empty()) return;
    view.selectedClipIds = {clips.front()->id};
    view.changed();
    clipBrowse[device] = {trackId, clips.front()->id};
    if (hooks.reveal) hooks.reveal(clips.front()->offsetMs);
}

// ── jog touch holds playback; the last release resumes it ─────────────────────────
void MidiControllerActions::jogTouch(const juce::String& device, int deck, bool pressed)
{
    const auto key = device + ":" + juce::String(deck);
    if (!pressed)
    {
        resumeAfterFinalRelease(endHold(key));
        return;
    }
    const bool first = holds.empty();
    if (!holds.insert(key).second) return;
    if (first && live.playing)
    {
        log::info("midi", "deck " + juce::String(deck) + " jog touched; holding playback");
        actions.pause();
        heldPlayback = true;
    }
}

bool MidiControllerActions::endHold(const juce::String& key)
{
    return holds.erase(key) != 0 && holds.empty();
}

bool MidiControllerActions::endHoldsForDevice(const juce::String& device)
{
    bool removed = false;
    for (auto it = holds.begin(); it != holds.end();)
    {
        if (it->startsWith(device + ":"))
        {
            it = holds.erase(it);
            removed = true;
        }
        else
            ++it;
    }
    return removed && holds.empty();
}

void MidiControllerActions::resumeAfterFinalRelease(bool finalRelease)
{
    if (!finalRelease || !std::exchange(heldPlayback, false)) return;
    const double end = model().contentEndMs();
    if (end > 0.0 && live.positionMs >= end) return;
    log::info("midi", "final jog touch released; resuming playback");
    actions.play();
}

void MidiControllerActions::suspend()
{
    stopTimer();
    jogPending = false;
    pendingSeekDeltaMs = pendingBeatSteps = snappedJogUnits = 0.0;
    pendingJogMode = {};
    pendingJogDevice = {};
    if (!holds.empty())
    {
        holds.clear();
        resumeAfterFinalRelease(true);
    }
}

// ── markers (useTransportSkip.ts) ─────────────────────────────────────────────
void MidiControllerActions::seekTo(double positionMs)
{
    live.positionMs = positionMs;
    actions.seek(positionMs);
    if (hooks.reveal) hooks.reveal(positionMs);
}

void MidiControllerActions::previousMarker()
{
    double target = 0.0;
    for (const auto& m : model().markers)
        if (m.positionMs < live.positionMs - kMarkerSkipEpsilonMs && m.positionMs > target) target = m.positionMs;
    seekTo(target);
}

void MidiControllerActions::nextMarker()
{
    std::optional<double> target;
    for (const auto& m : model().markers)
        if (m.positionMs > live.positionMs + kMarkerSkipEpsilonMs && (!target || m.positionMs < *target)) target = m.positionMs;
    if (!target)
    {
        const double end = model().contentEndMs();
        if (end > live.positionMs + kMarkerSkipEpsilonMs) target = end;
    }
    if (target) seekTo(*target);
}

void MidiControllerActions::markerJump(int index)
{
    std::vector<double> positions;
    for (const auto& m : model().markers) positions.push_back(m.positionMs);
    std::sort(positions.begin(), positions.end());
    if (index >= 0 && index < static_cast<int>(positions.size())) seekTo(positions[static_cast<size_t>(index)]);
}

void MidiControllerActions::toggleMarker()
{
    for (const auto& m : model().markers)
        if (std::abs(m.positionMs - live.positionMs) < 1.0)
        {
            actions.removeMarker(m.id);
            return;
        }
    actions.addMarker(live.positionMs);
}

// ── devices ───────────────────────────────────────────────────────────────────
bool MidiControllerActions::scrubAudioEnabled(const juce::String& device) const
{
    return static_cast<bool>(prefs.getSection("midiDevicePreferences").getProperty(device, {}).getProperty("scrubAudioEnabled", false));
}

void MidiControllerActions::devicesList(const juce::var& p)
{
    std::set<juce::String> enabledNow;
    if (const auto* inputs = p.getProperty("inputs", {}).getArray())
        for (const auto& input : *inputs)
            if (static_cast<bool>(input.getProperty("enabled", false))) enabledNow.insert(input.getProperty("identifier", {}).toString());
    // An input that went away (unplugged, disabled) lets go of the playback it was holding.
    for (const auto& id : enabledInputs)
        if (enabledNow.count(id) == 0) resumeAfterFinalRelease(endHoldsForDevice(id));
    enabledInputs = enabledNow;
    autoEnable(p);

    // Once per launch: each enabled input gets its saved deck selection, or its Default deck
    // preference applied live (never saved, so it re-applies until the cue button saves one).
    if (deckSelectionSynced || enabledNow.empty()) return;
    deckSelectionSynced = true;
    const auto saved = prefs.getSection("midiDeckSelections");
    for (const auto& id : enabledNow)
    {
        auto selection = saved.getProperty(id, {});
        if (selection.isVoid())
        {
            const auto defaultDeck =
                prefs.getSection("midiDevicePreferences").getProperty(id, {}).getProperty("defaultDeck", "deck1").toString();
            if (defaultDeck == "none") continue;
            selection = object({{"deck1Enabled", defaultDeck == "deck1"}, {"deck2Enabled", defaultDeck == "deck2"}});
        }
        engine.send("MIDI_DECK_SELECTION_SET", object({{"deviceIdentifier", id},
                                                       {"deck1Enabled", selection.getProperty("deck1Enabled", true)},
                                                       {"deck2Enabled", selection.getProperty("deck2Enabled", false)}}));
    }
}

void MidiControllerActions::deckSelection(const juce::var& p)
{
    // The controller's cue-set buttons changed which decks are live: remember it per device.
    const auto device = p.getProperty("deviceIdentifier", {}).toString();
    if (device.isEmpty()) return;
    const bool deck1 = static_cast<bool>(p.getProperty("deck1Enabled", false));
    const bool deck2 = static_cast<bool>(p.getProperty("deck2Enabled", false));
    prefs.set("midiDeckSelections", device, object({{"deck1Enabled", deck1}, {"deck2Enabled", deck2}}));
    prefs.save();
    for (int deck = 1; deck <= 2; ++deck)
        if (!(deck == 1 ? deck1 : deck2))
        {
            resumeAfterFinalRelease(endHold(device + ":" + juce::String(deck)));
            syncPressed[deck] = false;
        }
}

} // namespace stemcel::native
