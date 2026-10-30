#pragma once

#include <juce_core/juce_core.h>

namespace stemcel
{

// Per-user application-data root that holds the `Stemcel/` folder (peaks, decoded and
// plugin caches). It must be the same directory Electron's `app.getPath('appData')`
// returns, because main confines renderer reads of those caches to it. On Windows that is
// JUCE's `userApplicationDataDirectory` (%APPDATA%); on macOS JUCE returns `~/Library`,
// so this appends `Application Support` to match Electron.
juce::File userAppDataDirectory();

// The system temp directory as Electron's `app.getPath('temp')` sees it. The backend writes
// unsaved-project artifacts (stems, recordings, samples) under `<temp>/Stemcel/`, and main
// trusts that same tree for renderer reads. On macOS JUCE's `tempDirectory` is a per-app
// folder under `~/Library/Caches`, so this reads `$TMPDIR` (inherited from Electron) instead.
juce::File systemTempDirectory();

// The name of the app's own folders under those roots, the home folder and temp ("Stemcel";
// settable so a test can point elsewhere).
juce::String appFolderName();
void setAppFolderName(const juce::String& name);

// Moves each `<oldName>` folder (home, app data) to `<newName>` the first time, so models,
// settings, caches and the sample library carry over a rename of the app.
void migrateAppFolders(const juce::String& oldName, const juce::String& newName);

// A saved absolute path into a folder migrateAppFolders moved, pointed at where it is now
// (unchanged when it still exists or was never in one).
juce::String remapMovedAppPath(const juce::String& path);

} // namespace stemcel
