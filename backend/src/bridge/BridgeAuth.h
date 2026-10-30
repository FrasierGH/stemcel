#pragma once

#include <juce_core/juce_core.h>

namespace stemcel::bridge_auth
{

/** Returns true when `payload.token` satisfies the expected per-session bridge token. */
bool isTokenValid(const juce::String& expectedToken, const juce::var& payload);

} // namespace stemcel::bridge_auth
