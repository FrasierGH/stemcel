#pragma once

// DJ transitions between two tracks (docs/native-app.md, "Transitions"): a library of presets
// - blends, EQ and bass swaps, filter fades, echo and reverb throws, cuts, brakes, spinbacks,
// loop rolls, tempo ramps - each a set of automation shapes for the outgoing and incoming deck
// over the window the two clips overlap, plus the odd clip effect. Applying one writes ordinary
// automation (so it can be seen and edited afterwards) and records which preset joins the two
// clips (MIX_TRANSITION_SET), all as one undo step.

#include "Automation.h"
#include "EngineClient.h"
#include "ProjectModel.h"

#include <juce_core/juce_core.h>

#include <map>
#include <optional>
#include <vector>

namespace stemcel::native
{
class TimelineActions;

namespace mix
{
// One automation parameter's shape over the window: (position 0..1, value in the parameter's
// own units). Two points at the same position are a step (the second lands a millisecond later).
// A curve drawn by hand also carries its points with their segment shapes (`shaped`, timeMs
// being the position 0..1), written as they are; `points` then samples it, for drawing.
struct Shape
{
    juce::String param;
    std::vector<std::pair<double, double>> points;
    std::vector<automation::Point> shaped;
};

enum class Extra
{
    none,
    brake,     // the outgoing clip stops like a turntable at its end
    backspin,  // the outgoing clip spins back at its end
    loopRoll,  // the outgoing deck's last bar repeats at 1/4, 1/8, then 1/16
    tempoRamp  // the tempo map ramps from the outgoing tempo to the incoming one over the window
};

struct Preset
{
    juce::String id, name, category, description;
    double bars = 8.0; // a good length for it, for the auto mixer
    std::vector<Shape> out, in;
    Extra extra = Extra::none;
    bool usesEcho = false, usesReverb = false;
    // What it suits, for the auto mixer: keeps the two tracks' harmonies from playing together
    // for long (cuts, throws, filters), copes with a big tempo gap, and how it moves the energy.
    bool harmonicSafe = false;
    bool tempoGapSafe = false;
    int energy = 0; // -1 lets it down, 0 keeps it, +1 lifts it
};

const std::vector<Preset>& presets();
const Preset* find(const juce::String& id);
juce::StringArray categories();

// The window an outgoing clip and an incoming clip on another track share: from where the
// incoming one starts to where the outgoing one ends.
std::optional<std::pair<double, double>> overlap(const ClipModel& out, const ClipModel& in);

// Every (outgoing, incoming) pair this clip is part of.
std::vector<std::pair<juce::String, juce::String>> pairsFor(const ProjectModel& model, const juce::String& clipId);

// The transition recorded between two clips (either order), if any.
const juce::var* recordFor(const ProjectModel& model, const juce::String& outClipId, const juce::String& inClipId);

// The window a pair's transition covers: its record's (where it may have been dragged), else
// the clips' overlap.
std::optional<std::pair<double, double>> windowOf(const ProjectModel& model, const juce::String& outClipId,
                                                  const juce::String& inClipId);
// How far a transition's window may run: over the clips that sit end to end with the
// outgoing one on its track, and with the incoming one on its track (both tracks must have
// audio there), so a transition can span a clip that was split or followed by another.
std::pair<double, double> windowLimits(const ProjectModel& model, const ClipModel& out, const ClipModel& in);
// Writes a recorded transition again over `window` (a preset, or edited halves), as one
// undo step: what dragging its edges or its label does.
bool reapply(EngineClient& engine, TimelineActions& actions, const ProjectModel& model, const juce::var& record,
             std::pair<double, double> window);
// After a clip was moved or trimmed (its timeline span was oldStartMs..oldEndMs; `model` has
// it as it is now), every transition it is part of follows: a move carries the window with
// it, and an edge the window ended (or started) on takes the window's end (start) along, so a
// transition at the end of an outgoing clip stretches when the clip is lengthened. A window
// with no room left is removed; `removed` gets those transitions' labels.
void retimeAfterClipEdit(EngineClient& engine, TimelineActions& actions, const ProjectModel& model, const juce::String& clipId,
                         double oldStartMs, double oldEndMs, juce::StringArray& removed);

// Applies a preset over the pair's overlap (or `window` when given), replacing any transition
// already between them. One undo step. Returns false when the clips do not overlap.
bool apply(EngineClient& engine, TimelineActions& actions, const ProjectModel& model, const juce::String& outClipId,
           const juce::String& inClipId, const juce::String& presetId,
           std::optional<std::pair<double, double>> window = std::nullopt);

// The transition clipboard: a copied preset, pasted onto another join (in this project or
// another tab's).
void copyPreset(const juce::String& presetId);
juce::String copiedPreset();

// Undoes a transition's effect (its automation over the window, its clip effects, loop roll and
// tempo ramp) and forgets it. One undo step.
void remove(EngineClient& engine, TimelineActions& actions, const ProjectModel& model, const juce::String& transitionId);

// ── Outs and ins ───────────────────────────────────────────────────────────────
// A transition as two halves chosen and tweaked apart: how the outgoing track leaves (an
// Out) and how the incoming one arrives (an In). Each half is a handful of moves over its own
// span of the window, with one curve shape for all of them.
struct Half
{
    juce::String kind;                 // its HalfKind, for the name
    double from = 0.0, to = 1.0;       // where in the window it acts (0..1)
    juce::String curve = "equalPower"; // equalPower, linear, fast, slow, sCurve
    // The fader: fade over the span; "edge" fades over its last quarter (an Out) or first
    // quarter (an In); cut steps at the span's end (Out) or start (In); none leaves it.
    juce::String level = "fade";
    juce::String filter = "none"; // none, lowPass, highPass: swept over the span
    double filterDepth = 1.0;     // how far it sweeps, 0..1 of the filter's range
    // The EQ bands: none, swap (a step at swapAt, the bass-swap move) or sweep (over the span).
    juce::String low = "none", mid = "none", high = "none";
    double swapAt = 0.5;
    double killDb = -40.0;          // how far a band goes down
    juce::String effect = "none";   // Outs: echo, reverb or crush, thrown over the span
    Extra extra = Extra::none;      // Outs: brake, backspin or loopRoll at the clip's end
    // Curves drawn by hand, by parameter (timeMs is the position 0..1 across the window):
    // they replace what the options above would write for that parameter.
    std::map<juce::String, std::vector<automation::Point>> manual;

    juce::var toVar() const;
    static Half fromVar(const juce::var& v);
};

struct HalfKind
{
    juce::String id, name, description;
    bool outgoing = true;
    Half defaults;
};
const std::vector<HalfKind>& halfKinds();
const HalfKind* findHalf(const juce::String& id);

// The automation a half writes on its track (window-relative points).
std::vector<Shape> shapesFor(const Half& h, bool outgoing);
// A starting point for drawing `param` by hand: the curve the options write now, thinned to a
// few points (or a flat line at the parameter's default over the half's span).
std::vector<automation::Point> manualSeed(const Half& h, bool outgoing, const juce::String& param);

// Both sides of one join, with the window-level tempo ramp.
struct Join
{
    std::optional<Half> out, in;
    bool rampTempo = false;
};
// What a recorded transition is as halves: its own, or its preset's nearest (to edit it).
Join joinOf(const juce::var& record);
Join joinForPreset(const juce::String& presetId);
// "Echo Out + Muffled Intro", or the preset's name.
juce::String labelOf(const juce::var& record);

// Applies a join over `window` (by default the pair's current window, windowOf), replacing
// what is there. One undo step.
bool applyJoin(EngineClient& engine, TimelineActions& actions, const ProjectModel& model, const juce::String& outClipId,
               const juce::String& inClipId, const Join& join, std::optional<std::pair<double, double>> window = std::nullopt);

// Something from the Transitions panel dropped on a clip ("transition:out:<kind>",
// "transition:in:<kind>" or "transition:preset:<id>"): applies it to that clip's join nearest
// `atMs` (an Out where the clip is the outgoing one, an In where it is the incoming one, the
// other half kept). Returns why nothing happened, or empty.
juce::String drop(EngineClient& engine, TimelineActions& actions, const ProjectModel& model, const juce::String& clipId,
                  double atMs, const juce::String& what);
} // namespace mix
} // namespace stemcel::native
