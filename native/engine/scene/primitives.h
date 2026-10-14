// SecondSonPC native runtime - generated helper geometry (ground grid, boxes).
// Test geometry only; never presented as game content.
#pragma once

#include "core/math.h"
#include "resources/geometry.h"

namespace sspc {

// Checkerboard ground of `tiles` x `tiles` squares of `tile_size` metres centred at the origin,
// split into the two colours (even/odd squares) so they can be drawn with different tints.
void MakeGroundGrid(int tiles, float tile_size, res::MeshData& even, res::MeshData& odd);
res::MeshData MakeBox(const Vec3& min, const Vec3& max);

}  // namespace sspc
