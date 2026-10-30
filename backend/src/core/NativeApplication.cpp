#include "NativeApplication.h"

#include <juce_core/juce_core.h>

#if JUCE_MAC
namespace juce
{
// Defined in juce_events (juce_MessageManager_mac.mm) and declared the same way by JUCE's own
// JUCEApplicationBase::main; it is the call JUCE makes for exactly this purpose.
void initialiseNSApplication();
} // namespace juce
#endif

namespace stemcel
{

void initialiseNativeApplication()
{
#if JUCE_MAC
    juce::initialiseNSApplication();
#endif
}

} // namespace stemcel
