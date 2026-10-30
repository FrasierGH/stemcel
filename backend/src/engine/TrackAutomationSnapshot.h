#pragma once

#include "BreakpointCurve.h"

#include <juce_core/juce_core.h>

#include <vector>

namespace stemcel
{

// Automatable track parameter ids (must match the bridge `paramId` strings).
enum class AutomationParam
{
    filter,
    pan,
    toneBass,
    toneMid,
    toneTreble,
    reverbSend,
    delaySend,
    leveler,
    punch,
    saturationDrive,
    saturationMix,
    bitCrusherRate,
    bitCrusherBits,
    bitCrusherBoost,
    bitCrusherMix,
    level,
    // The master bus's own (on the master's automation only): the project reverb, echo and
    // glue.
    reverbMix,
    reverbSize,
    reverbDecay,
    delayMix,
    delayFeedback,
    reverbTone,
    delayTone,
    glue,
    count_
};

inline bool automationParamFromString(const juce::String& id, AutomationParam& out) noexcept
{
    if (id == "filter") { out = AutomationParam::filter; return true; }
    if (id == "pan") { out = AutomationParam::pan; return true; }
    if (id == "toneBass") { out = AutomationParam::toneBass; return true; }
    if (id == "toneMid") { out = AutomationParam::toneMid; return true; }
    if (id == "toneTreble") { out = AutomationParam::toneTreble; return true; }
    if (id == "reverbSend") { out = AutomationParam::reverbSend; return true; }
    if (id == "delaySend") { out = AutomationParam::delaySend; return true; }
    if (id == "leveler") { out = AutomationParam::leveler; return true; }
    if (id == "punch") { out = AutomationParam::punch; return true; }
    if (id == "saturationDrive") { out = AutomationParam::saturationDrive; return true; }
    if (id == "saturationMix") { out = AutomationParam::saturationMix; return true; }
    if (id == "bitCrusherRate") { out = AutomationParam::bitCrusherRate; return true; }
    if (id == "bitCrusherBits") { out = AutomationParam::bitCrusherBits; return true; }
    if (id == "bitCrusherBoost") { out = AutomationParam::bitCrusherBoost; return true; }
    if (id == "bitCrusherMix") { out = AutomationParam::bitCrusherMix; return true; }
    if (id == "level") { out = AutomationParam::level; return true; }
    if (id == "reverbMix") { out = AutomationParam::reverbMix; return true; }
    if (id == "reverbSize") { out = AutomationParam::reverbSize; return true; }
    if (id == "reverbDecay") { out = AutomationParam::reverbDecay; return true; }
    if (id == "delayMix") { out = AutomationParam::delayMix; return true; }
    if (id == "delayFeedback") { out = AutomationParam::delayFeedback; return true; }
    if (id == "reverbTone") { out = AutomationParam::reverbTone; return true; }
    if (id == "delayTone") { out = AutomationParam::delayTone; return true; }
    if (id == "glue") { out = AutomationParam::glue; return true; }
    return false;
}

// A plugin parameter's lane id, "plugin:<slotId>:<parameter index>" (its values 0..1, the
// parameter's normalised range).
inline bool parsePluginParamId(const juce::String& id, juce::String& slotId, int& index)
{
    if (!id.startsWith("plugin:")) return false;
    const auto rest = id.substring(7);
    const int colon = rest.lastIndexOfChar(':');
    if (colon <= 0 || !rest.substring(colon + 1).containsOnly("0123456789") || rest.substring(colon + 1).isEmpty()) return false;
    slotId = rest.substring(0, colon);
    index = rest.substring(colon + 1).getIntValue();
    return true;
}

// Immutable per-track automation: one optional BreakpointCurve per automatable
// parameter. Built on the message thread, published by raw pointer to the audio
// thread (see `AudioEngine`'s ownership + retire queue). Never mutate after build.
struct TrackAutomationSnapshot
{
    static constexpr int kNumParams = static_cast<int>(AutomationParam::count_);

    bool has[kNumParams] = {};
    BreakpointCurve curves[kNumParams];

    // Plugin parameter lanes. The cursor and the last value sent belong to the audio thread
    // rendering this snapshot (one track's, or the master's).
    struct PluginCurve
    {
        juce::String paramId, slotId;
        int index = 0;
        BreakpointCurve curve;
        mutable std::size_t cursor = 0;
        mutable float lastSent = -1.0F;
    };
    std::vector<PluginCurve> plugins;

    bool hasAny() const noexcept
    {
        for (bool h : has)
            if (h) return true;
        return !plugins.empty();
    }

    // Puts a lane's curve in its place (a parameter's slot, or the plugin list); false for an
    // id that is neither.
    bool setLane(const juce::String& paramId, BreakpointCurve curve)
    {
        juce::String slotId;
        int index = 0;
        if (parsePluginParamId(paramId, slotId, index))
        {
            plugins.push_back({paramId, slotId, index, std::move(curve)});
            return true;
        }
        AutomationParam param{};
        if (!automationParamFromString(paramId, param)) return false;
        has[static_cast<int>(param)] = true;
        curves[static_cast<int>(param)] = std::move(curve);
        return true;
    }

    bool hasParam(AutomationParam p) const noexcept { return has[static_cast<int>(p)]; }
    const BreakpointCurve& curve(AutomationParam p) const noexcept
    {
        return curves[static_cast<int>(p)];
    }
};

} // namespace stemcel
