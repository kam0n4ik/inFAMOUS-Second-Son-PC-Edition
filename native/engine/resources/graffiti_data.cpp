#include "resources/graffiti_data.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#include "core/name_hash.h"
#include "resources/geometry.h"
#include "resources/type_registry.h"

namespace sspc::res {
namespace {

constexpr u64 kClassMinigame = ClassIdHash("GraffitiMinigame");
constexpr u32 kMaxMarkers = 256;

u64 IdPrefix(const std::array<u8, 16>& id) {
    u64 v = 0;
    std::memcpy(&v, id.data(), 8);
    return v;
}

bool Finite(const Vec4& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w); }

// Marker positions of one LO_ARRAY_DATA: +0x58 pointer to u64 OIDs, +0x60 count. The TRANSFORM
// objects that follow the array in the catalog are its own (a package can repeat an OID in two
// arrays with different objects), so they are matched first.
bool ReadMarkers(const KcapPackage& pkg, const ObjectCatalog& cat, size_t array_index, std::vector<Vec3>& out,
                 std::string* error) {
    const auto& objs = cat.Objects();
    const GameObject& arr = objs[array_index];
    const auto list = pkg.PointerAt(arr.offset + 0x58);
    u64 count = 0;
    if (!list || !pkg.Read(arr.offset + 0x60, count) || count > kMaxMarkers || !pkg.InRange(*list, count * 8)) {
        if (error) *error = std::format("LO_ARRAY at {:#x}: invalid member list", arr.offset);
        return false;
    }
    for (u32 i = 0; i < count; ++i) {
        u64 oid = 0;
        pkg.Read(*list + i * 8, oid);
        const GameObject* found = nullptr;
        for (size_t j = array_index + 1; j < objs.size() && objs[j].type_name == "TRANSFORM_DATA"; ++j) {
            if (objs[j].oid == oid) {
                found = &objs[j];
                break;
            }
        }
        if (!found) found = cat.FindByOid(oid);
        const auto t = found ? cat.Transform(*found) : std::nullopt;
        if (!t) {
            if (error) *error = std::format("LO_ARRAY at {:#x}: member {:#x} has no transform", arr.offset, oid);
            return false;
        }
        out.push_back(t->position_cm);
    }
    return true;
}

}  // namespace

std::optional<u32> FindGeometryById(const KcapPackage& pkg, u64 id) {
    for (const GeometryRecord& g : FindGeometry(pkg)) {
        if (IdPrefix(g.id) == id) return g.offset;
    }
    return std::nullopt;
}

bool LoadGraffitiDefinition(const KcapPackage& pkg, const ObjectCatalog& catalog, GraffitiDef& out, std::string* error) {
    out = GraffitiDef{};
    const auto minigames = catalog.OfType("GRAFFITI_MINIGAME_DATA");
    if (minigames.size() != 1) {
        if (error) *error = std::format("expected one GRAFFITI_MINIGAME_DATA, found {}", minigames.size());
        return false;
    }
    out.oid = minigames.front()->oid;
    SpliceLinks links;
    if (!ParseSpliceLinks(pkg, links, error)) return false;

    bool have_dims = false;
    for (const SpliceMemberRef& r : links.members) {
        if (r.class_hash != kClassMinigame) continue;
        if (r.member_hash == NameHash("PHYS_DIMENSIONS")) {
            if (const auto v = ImmediateVector(pkg, r)) {
                out.phys_dimensions_cm = {v->x, v->y, v->z};
                have_dims = true;
            }
        } else if (r.member_hash == NameHash("CAMERA_POS")) {
            if (const auto v = ImmediateVector(pkg, r)) {
                out.camera_pos_cm = {v->x, v->y, v->z};
                out.has_camera = true;
            }
        } else if (r.member_hash == NameHash("CAMERA_EULER")) {
            if (const auto v = ImmediateVector(pkg, r)) out.camera_euler = {v->x, v->y, v->z};
        } else if (r.member_hash == NameHash("CAMERA_FIELD_OF_VIEW")) {
            out.camera_fov = ImmediateFloat(pkg, r);
        } else if (r.member_hash == NameHash("PAINT_SIZE")) {
            out.paint_size = ImmediateFloat(pkg, r);
        } else if (r.member_hash == NameHash("GEOM_NOPAINT")) {
            if (const auto v = ImmediateId(pkg, r)) out.geometry_nopaint = *v;
        }
    }
    if (!have_dims || out.phys_dimensions_cm.y <= 0 || out.phys_dimensions_cm.z <= 0) {
        if (error) *error = "PHYS_DIMENSIONS not set";
        return false;
    }

    std::vector<SpliceCall> stage_calls;
    for (const SpliceCall& c : links.methods) {
        if (c.class_hash == kClassMinigame && c.member_hash == NameHash("ADD_STAGE")) stage_calls.push_back(c);
    }
    std::sort(stage_calls.begin(), stage_calls.end(), [](const SpliceCall& a, const SpliceCall& b) { return a.location < b.location; });
    std::vector<size_t> arrays;
    for (size_t i = 0; i < catalog.Objects().size(); ++i) {
        if (catalog.Objects()[i].type_name == "LO_ARRAY_DATA") arrays.push_back(i);
    }
    if (stage_calls.empty()) {
        if (error) *error = "no ADD_STAGE call";
        return false;
    }
    for (size_t s = 0; s < stage_calls.size(); ++s) {
        const u32 a = CallArgumentBlock(stage_calls[s]);
        GraffitiStageDef st;
        float colors[8];
        if (!pkg.Read(a, st.stencil_geometry) || !pkg.Read(a + 8, st.paint_geometry) || !pkg.Read(a + 16, st.reveal_geometry) ||
            !pkg.Read(a + 32, colors)) {
            if (error) *error = std::format("ADD_STAGE #{} arguments out of range", s);
            return false;
        }
        st.color = {colors[0], colors[1], colors[2], colors[3]};
        st.color_light = {colors[4], colors[5], colors[6], colors[7]};
        if (!Finite(st.color) || !Finite(st.color_light) || !FindGeometryById(pkg, st.stencil_geometry) ||
            !FindGeometryById(pkg, st.paint_geometry)) {
            if (error) *error = std::format("ADD_STAGE #{}: stencil/paint geometry not in the package", s);
            return false;
        }
        if (s < arrays.size() && !ReadMarkers(pkg, catalog, arrays[s], st.markers_cm, error)) return false;
        out.stages.push_back(std::move(st));
    }
    return true;
}

std::optional<ObjectTransform> FindGraffitiSite(const ObjectCatalog& chunk, u64 minigame_oid) {
    for (const GameObject* o : chunk.OfType("PROXY_SITE_DATA")) {
        if (o->oid == minigame_oid) return chunk.Transform(*o);
    }
    return std::nullopt;
}

}  // namespace sspc::res
