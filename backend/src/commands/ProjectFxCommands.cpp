#include "ProjectFxCommands.h"

#include "AudioEngine.h"
#include "BridgeServer.h"
#include "CommandHelpers.h"
#include "PayloadHelpers.h"
#include "ProjectState.h"
#include "SharedFx.h"

namespace stemcel
{

using stemcel::bridge::readOptionalNumber;
using stemcel::bridge::readOptionalString;

void handleProjectSetReverb(const juce::var& payload, stemcel::AudioEngine& engine,
                            stemcel::ProjectState& projectState,
                            stemcel::BridgeServer& bridge)
{
    const float size = static_cast<float>(
        readOptionalNumber(payload, "size").value_or(projectState.getProjectReverbSize()));
    const float decay = static_cast<float>(
        readOptionalNumber(payload, "decay").value_or(projectState.getProjectReverbDecay()));
    const float tone = static_cast<float>(
        readOptionalNumber(payload, "tone").value_or(projectState.getProjectReverbTone()));
    const float mix = static_cast<float>(
        readOptionalNumber(payload, "mix").value_or(projectState.getProjectReverbMix()));

    const bool changed = projectState.setProjectReverb(size, decay, tone, mix);
    if (!changed) return;

    const float canonSize = projectState.getProjectReverbSize();
    const float canonDecay = projectState.getProjectReverbDecay();
    const float canonTone = projectState.getProjectReverbTone();
    const float canonMix = projectState.getProjectReverbMix();

    // Live UI gesture → glide (snap=false) to avoid zipper noise.
    engine.setProjectReverb(canonSize, canonDecay, canonTone, canonMix, /*snap*/ false);

    broadcastApplied(bridge, "PROJECT_REVERB_APPLIED",
                     {{"size", canonSize},
                      {"decay", canonDecay},
                      {"tone", canonTone},
                      {"mix", canonMix}});
}

void handleProjectSetDelay(const juce::var& payload, stemcel::AudioEngine& engine,
                           stemcel::ProjectState& projectState,
                           stemcel::BridgeServer& bridge)
{
    const juce::String noteValue =
        readOptionalString(payload, "noteValue").value_or(projectState.getProjectDelayNoteValue());
    const float feedback = static_cast<float>(
        readOptionalNumber(payload, "feedback").value_or(projectState.getProjectDelayFeedback()));
    const float tone = static_cast<float>(
        readOptionalNumber(payload, "tone").value_or(projectState.getProjectDelayTone()));
    const float mix = static_cast<float>(
        readOptionalNumber(payload, "mix").value_or(projectState.getProjectDelayMix()));

    const bool changed = projectState.setProjectDelay(noteValue, feedback, tone, mix);
    if (!changed) return;

    const juce::String canonNote = projectState.getProjectDelayNoteValue();
    const float canonFeedback = projectState.getProjectDelayFeedback();
    const float canonTone = projectState.getProjectDelayTone();
    const float canonMix = projectState.getProjectDelayMix();

    // Shared resolver keeps live playback and offline mixdown aligned.
    const double delayMs = stemcel::delayNoteToMs(canonNote, projectState.getBpm());
    engine.setProjectDelay(delayMs, canonFeedback, canonTone, canonMix, /*snap*/ false);

    broadcastApplied(bridge, "PROJECT_DELAY_APPLIED",
                     {{"noteValue", canonNote},
                      {"feedback", canonFeedback},
                      {"tone", canonTone},
                      {"mix", canonMix}});
}

void handleProjectSetMixGlue(const juce::var& payload, stemcel::AudioEngine& engine,
                             stemcel::ProjectState& projectState,
                             stemcel::BridgeServer& bridge)
{
    const auto amount = readOptionalNumber(payload, "amount");
    const auto makeup = readOptionalNumber(payload, "makeupDb");
    if (!amount.has_value() && !makeup.has_value()) return;

    bool changed = false;
    if (amount.has_value()) changed |= projectState.setProjectMixGlueAmount(static_cast<float>(*amount));
    if (makeup.has_value()) changed |= projectState.setProjectMixGlueMakeupDb(static_cast<float>(*makeup));
    if (!changed) return;

    const float canonicalAmount = projectState.getProjectMixGlueAmount();
    const float canonicalMakeup = projectState.getProjectMixGlueMakeupDb();
    engine.setProjectMixGlue(canonicalAmount, /*snap*/ false);
    engine.setProjectMixGlueMakeup(canonicalMakeup, /*snap*/ false);
    broadcastApplied(bridge, "PROJECT_MIX_GLUE_APPLIED", {{"amount", canonicalAmount}, {"makeupDb", canonicalMakeup}});
}

} // namespace stemcel
