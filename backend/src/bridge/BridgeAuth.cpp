#include "BridgeAuth.h"

namespace stemcel::bridge_auth
{

bool isTokenValid(const juce::String& expectedToken, const juce::var& payload)
{
    if (expectedToken.isEmpty())
    {
        return true;
    }

    const juce::String token = payload.getProperty("token", juce::var()).toString();
    if (token.length() != expectedToken.length())
    {
        return false;
    }

    int diff = 0;
    for (int i = 0; i < expectedToken.length(); ++i)
    {
        diff |= static_cast<int>(expectedToken[i]) ^ static_cast<int>(token[i]);
    }
    return diff == 0;
}

} // namespace stemcel::bridge_auth
