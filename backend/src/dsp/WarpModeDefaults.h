#pragma once

namespace stemcel
{

// The warp mode a clip gets when none is chosen: Signalsmith Stretch, which measured
// closest to the source and smeared transients least of the modes, at the lowest CPU
// cost (docs/native-app.md, "Time-stretch quality"). Clips that saved a mode keep it.
inline constexpr const char* kDefaultWarpMode = "pro";

} // namespace stemcel
