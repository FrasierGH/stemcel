#pragma once

// A track's output, chosen from a menu: the master, one of the project's buses, or a new bus.
// Used by the track sidebar's Out button and the mixer strips'.

#include "ProjectModel.h"
#include "TimelineActions.h"
#include "Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace stemcel::native::buses
{

inline bool isBus(const TrackModel& track)
{
    return static_cast<bool>(track.raw.getProperty("isBus", false));
}

// The bus a track is sent to ("" for the master).
inline juce::String outputOf(const TrackModel& track)
{
    return track.raw.getProperty("outputBus", "").toString();
}

// What the Out button reads: "Master", or the bus's name.
inline juce::String outputLabel(const ProjectModel& model, const TrackModel& track)
{
    if (isBus(track)) return "Master";
    const auto bus = outputOf(track);
    if (const auto* b = bus.isNotEmpty() ? model.findTrack(bus) : nullptr) return b->name;
    return "Master";
}

inline void showOutputMenu(TimelineActions& actions, const ProjectModel& model, const juce::String& trackId, juce::Component* target)
{
    const auto* track = model.findTrack(trackId);
    if (track == nullptr || isBus(*track)) return;
    const auto current = outputOf(*track);
    juce::PopupMenu menu;
    menu.addSectionHeader("Send to");
    menu.addItem("Master", true, current.isEmpty(), [&actions, trackId] { actions.setTrackOutput(trackId, {}); });
    for (const auto& t : model.tracks)
        if (isBus(t))
            menu.addItem(t.name, true, current == t.id, [&actions, trackId, busId = t.id] { actions.setTrackOutput(trackId, busId); });
    menu.addSeparator();
    menu.addItem("New Bus", [&actions, trackId] { actions.sendToNewBus(trackId); });
    auto options = juce::PopupMenu::Options();
    menu.showMenuAsync(target != nullptr ? options.withTargetComponent(target) : options.withMousePosition());
}

} // namespace stemcel::native::buses
