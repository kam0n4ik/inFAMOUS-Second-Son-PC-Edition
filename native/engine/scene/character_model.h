// SecondSonPC native runtime - character model assembled from a game package.
//
// Loads every geometry part of a character package (e.g. art/cache/hero.xpps), either from the
// converted .spcmesh cache (when its source hash matches the package) or by decoding the KCAP
// package directly; decoding runs on the job system. Part visibility defaults: parts drawn in the
// main view (view mask bit 1) with duplicates (same bounds and index count = clothing variants)
// hidden. The game chooses variants at run time from instance flags not decoded yet.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "core/jobs.h"
#include "core/math.h"
#include "resources/geometry.h"

namespace sspc {

struct ModelPartInfo {
    u32 source_offset = 0;
    u16 view_mask = 0;
    u32 vertex_count = 0;
    u32 triangle_count = 0;
    Vec3 bounds_min, bounds_max;
    bool default_visible = true;
};

struct LoadedModel {
    std::vector<res::MeshData> meshes;
    std::vector<ModelPartInfo> parts;
    std::array<u8, 16> source_hash{};
    Vec3 bounds_min, bounds_max;  // of the default-visible parts
    bool from_cache = false;
    double load_seconds = 0;
};

// `cache_path` may be empty (no cache). Returns false with `error` set on failure.
bool LoadCharacterModel(const std::wstring& package_path, const std::wstring& cache_path, JobSystem& jobs,
                        LoadedModel& out, std::string& error);

void ChooseDefaultParts(LoadedModel& model);

}  // namespace sspc
