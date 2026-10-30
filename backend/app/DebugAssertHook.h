#pragma once

namespace stemcel::native
{
// Debug Windows builds: logs CRT assertions with a symbolized stack (no-op elsewhere).
void installDebugAssertHook();
} // namespace stemcel::native
