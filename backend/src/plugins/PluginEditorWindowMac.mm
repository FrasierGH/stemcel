#import <AppKit/AppKit.h>

namespace stemcel::plugins
{
// A plugin editor as a child window of the main window: kept in front of it (and moved and
// minimised with it), but not in front of other apps. JUCE hands out each window's NSView.
void attachChildWindow(void* ownerView, void* childView)
{
    NSView* owner = (__bridge NSView*) ownerView;
    NSView* child = (__bridge NSView*) childView;
    if (owner == nil || child == nil || owner.window == nil || child.window == nil || owner.window == child.window) return;
    [owner.window addChildWindow:child.window ordered:NSWindowAbove];
}
} // namespace stemcel::plugins
