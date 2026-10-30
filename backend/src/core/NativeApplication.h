#pragma once

namespace stemcel
{

// Prepares the platform application object that JUCE's message loop runs on. Call once,
// before constructing juce::ScopedJuceInitialiser_GUI, in any entry point that then calls
// MessageManager::runDispatchLoop().
//
// On macOS that loop is `[NSApp run]`, and JUCE creates the shared NSApplication only from
// JUCEApplicationBase::main. Stemcel's executables have their own main(), so without this
// NSApp stays nil, `[NSApp run]` does nothing, and the loop returns the moment it starts.
// A no-op on other platforms.
void initialiseNativeApplication();

} // namespace stemcel
