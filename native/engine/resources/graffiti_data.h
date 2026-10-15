// SecondSonPC native runtime - stencil-art (graffiti) definitions of the original game.
//
// A graffiti package (graffiti_<chunk>_<name>.xpps, package kind 0x19) holds one GRAFFITI_MINIGAME_DATA
// object whose script sets its properties and adds its stages (confirmed on graffiti_a8_family, see
// docs/native/missions/GRAFFITI_RESEARCH.md):
//   PHYS_DIMENSIONS (thickness, width, height in cm), CAMERA_POS (world, cm), CAMERA_EULER (radians),
//   optional CAMERA_FIELD_OF_VIEW / PAINT_SIZE, GEOM_NOPAINT (geometry id);
//   ADD_STAGE(stencil geometry, paint geometry, reveal geometry or ~0, colour, light colour, markers)
//   per stage, in-line arguments {u64, u64, u64, pad, vec4, vec4}; runtime stage record 0x50 bytes at
//   CGraffitiMinigame+0x4a8 (eboot 0x1ee960 / 0x1eeb00 ...).
// The markers argument is not an immediate: the k-th LO_ARRAY_DATA of the package lists (by OID) the
// TRANSFORM objects of the k-th stage (all 14 stage-1 markers of graffiti_a8_family lie on the white
// shapes of its stage-1 mask). Geometry ids are the first 8 bytes of a geometry record's 16-byte id.
// The minigame is placed in a world chunk by a PROXY_SITE_DATA object with the same OID.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/math.h"
#include "core/types.h"
#include "resources/game_objects.h"
#include "resources/kcap.h"

namespace sspc::res {

inline constexpr u64 kNoGeometry = ~0ull;

struct GraffitiStageDef {
    u64 stencil_geometry = kNoGeometry;
    u64 paint_geometry = kNoGeometry;
    u64 reveal_geometry = kNoGeometry;
    Vec4 color;
    Vec4 color_light;
    std::vector<Vec3> markers_cm;  // graffiti-local (x = wall normal, y = width, z = height)
};

struct GraffitiDef {
    u64 oid = 0;
    Vec3 phys_dimensions_cm;  // thickness, width, height
    bool has_camera = false;
    Vec3 camera_pos_cm;       // world
    Vec3 camera_euler;        // radians: x roll, y pitch, z yaw (right-handed about the world axes)
    std::optional<float> camera_fov;
    std::optional<float> paint_size;
    u64 geometry_nopaint = kNoGeometry;
    std::vector<GraffitiStageDef> stages;
};

bool LoadGraffitiDefinition(const KcapPackage& pkg, const ObjectCatalog& catalog, GraffitiDef& out, std::string* error);

// Geometry record whose id starts with `id` (nullopt if absent).
std::optional<u32> FindGeometryById(const KcapPackage& pkg, u64 id);

// The chunk object that places the minigame (PROXY_SITE_DATA with the minigame's OID).
std::optional<ObjectTransform> FindGraffitiSite(const ObjectCatalog& chunk, u64 minigame_oid);

}  // namespace sspc::res
