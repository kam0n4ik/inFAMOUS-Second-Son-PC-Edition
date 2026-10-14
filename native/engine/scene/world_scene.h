// SecondSonPC native runtime - a world chunk of the original game, ready to draw and walk on.
#pragma once

#include <string>
#include <vector>

#include "core/jobs.h"
#include "core/math.h"
#include "gameplay/terrain.h"
#include "renderer/renderer.h"

namespace sspc {

struct WorldScene {
    std::vector<DrawItem> draws;  // one per drawable instance, world in metres
    TerrainGrid terrain;
    Vec3 spawn{0, 0, 0};
    u32 instances_total = 0;
    u32 instances_drawn = 0;
    u32 instances_external = 0;  // geometry lives in another package (KNLX), not loaded yet
    u32 geometry_uploaded = 0;
    u32 textures_uploaded = 0;
    double load_seconds = 0;
};

// Loads every scene object of a chunk package: geometry is decoded on the job system, uploaded
// once per slot and drawn per instance (kind 0). Walkable triangles feed the terrain grid.
bool LoadWorldChunk(const std::wstring& package_path, const std::vector<std::wstring>& texture_packages,
                    Renderer& renderer, JobSystem& jobs, WorldScene& out, std::string& error);

}  // namespace sspc
