#include "LiveState.h"

namespace stemcel::native
{

LiveState::LiveState(EngineClient& engine) : subscriptions(engine)
{
    subscriptions.on("PLAYHEAD_UPDATE",
                     [this](const juce::var& p)
                     {
                         positionMs = static_cast<double>(p.getProperty("positionMs", positionMs));
                         playing = static_cast<bool>(p.getProperty("isPlaying", playing));
                     });
    subscriptions.on("MASTER_LEVEL",
                     [this](const juce::var& p)
                     {
                         masterPeakL = static_cast<float>(static_cast<double>(p.getProperty("peakL", 0.0)));
                         masterPeakR = static_cast<float>(static_cast<double>(p.getProperty("peakR", 0.0)));
                     });
    subscriptions.on("TRACK_LEVELS",
                     [this](const juce::var& p)
                     {
                         if (const auto* tracks = p.getProperty("tracks", {}).getArray())
                             for (const auto& t : *tracks)
                                 trackPeaks[t.getProperty("id", {}).toString()] = {
                                     static_cast<float>(static_cast<double>(t.getProperty("peakL", 0.0))),
                                     static_cast<float>(static_cast<double>(t.getProperty("peakR", 0.0)))};
                     });
}

} // namespace stemcel::native
