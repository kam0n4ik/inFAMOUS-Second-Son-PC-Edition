// SecondSonPC — "potato" mode for weak PCs (native backend): fewer of the game's draws reach the command processor's
// preparation and the recording thread. The game's command buffers are unchanged; the processor drops whole draws
// of the shadow passes before their record exists, by level:
//   1 - the spot light shadow atlas (headlights, street lamps: the 1024x1024 depth-only target, ~1 100 draws a frame
//       in the city; docs/LIGHTING_PROFILE.md: 34 -> 38 FPS);
//   2 - 1 + the sun's far shadow cascade;
//   3 - 2 + the shadows of small objects (shadow casters' draws with few indices).
// The level is live ([Graphics] potatoMode, the F1 menu): read for every draw. SSPC_POTATO=0..3 overrides the setting
// (measurement stand). SSPC_DIAG_POTATO_CENSUS=1 logs the depth-only draws by target and viewport (what the levels
// are chosen from).
#pragma once

#include <string_view>

#include "base/abi.h"

namespace SecondSon::Gpu::Cp {
class RegisterFile;
struct DrawDescriptor;
} // namespace SecondSon::Gpu::Cp

namespace SecondSon::Gpu::Potato {

/// The menu's choice ("off" | "1" | "2" | "3"); SSPC_POTATO, when set, wins.
void SetLevel(std::string_view choice);
u32 Level();

/// The command processor's thread, for every draw before it is handed on: true - dropped (its changed register groups
/// are carried to the next draw by the caller).
bool Drop(const Cp::RegisterFile& regs, const Cp::DrawDescriptor& d);

/// The command processor's thread, at the game's flip: the frame's shadow cascades counted again.
void NewFrame();

} // namespace SecondSon::Gpu::Potato
