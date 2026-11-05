#pragma once

#include "ProjectModel.h"
#include "TimelineActions.h"

#include <juce_core/juce_core.h>

#include <set>

namespace stemcel::native::timeedit
{

// Edits that move time itself, on every track at once, each one undo step. Clips crossing an
// edge are cut there; markers, automation points and tempo changes after it move with the
// clips, and mix transitions follow their clips. A locked clip (or a linked one, which can't
// be cut on the timeline) in the way refuses the edit, with a message.

// Removes [fromMs, toMs): what lies in it goes, and everything after moves left to close it.
bool deleteTime(TimelineActions& actions, ProjectMirror& mirror, double fromMs, double toMs);
// Opens a gap of `lengthMs` at `atMs`: everything from there on moves right.
bool insertTime(TimelineActions& actions, ProjectMirror& mirror, double atMs, double lengthMs);
// Deletes the clips, and on each of their tracks moves the later clips left by the deleted
// clips' lengths, closing the gaps they leave (other tracks stay put).
void rippleDelete(TimelineActions& actions, ProjectMirror& mirror, const std::set<juce::String>& clipIds);

} // namespace stemcel::native::timeedit
