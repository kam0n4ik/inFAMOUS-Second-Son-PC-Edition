#include "resources/mission_data.h"

#include <algorithm>
#include <format>

#include "core/name_hash.h"

namespace sspc::res {

const MissionPoint* MissionPackageInfo::HeroPoint(u64 oid) const {
    for (const MissionPoint& p : hero_points) {
        if (p.oid == oid) return &p;
    }
    return nullptr;
}

const MissionPoint* MissionPackageInfo::Volume(u64 oid) const {
    for (const MissionPoint& p : volumes) {
        if (p.oid == oid) return &p;
    }
    return nullptr;
}

bool LoadMissionPackage(const KcapPackage& pkg, MissionPackageInfo& out, std::string* error) {
    out = MissionPackageInfo{};
    if (pkg.Header().kind != 0xA) {
        if (error) *error = std::format("package kind {:#x} is not a mission (0xA)", pkg.Header().kind);
        return false;
    }
    ObjectCatalog cat;
    if (!cat.Build(pkg, error)) return false;
    out.objects = static_cast<u32>(cat.Objects().size());
    const auto missions = cat.OfType("MISSION_DATA");
    if (missions.size() != 1) {
        if (error) *error = std::format("expected one MISSION_DATA, found {}", missions.size());
        return false;
    }
    out.mission_oid = missions.front()->oid;
    for (const GameObject* o : cat.OfType("OBJECTIVE_DATA")) out.objectives.push_back(o->oid);
    auto points = [&](std::string_view type, std::vector<MissionPoint>& dst) {
        for (const GameObject* o : cat.OfType(type)) {
            if (const auto t = cat.Transform(*o)) dst.push_back({o->oid, t->position_cm, t->rotation, t->scale});
        }
    };
    points("HERO_MATCH_POINT_DATA", out.hero_points);
    points("VOLUME_BUTTON_DATA", out.volumes);

    SpliceLinks links;
    if (!ParseSpliceLinks(pkg, links, error)) return false;
    static constexpr u64 kTextMembers[] = {NameHash("text"), NameHash("NOTE_TEXT"), NameHash("BANNER_TEXT"),
                                           NameHash("DESCRIPTION_TEXT"), NameHash("title")};
    for (const SpliceMemberRef& r : links.members) {
        if (std::find(std::begin(kTextMembers), std::end(kTextMembers), r.member_hash) == std::end(kTextMembers)) continue;
        if (const auto id = ImmediateId(pkg, r); id && *id && *id != ~0ull) out.text_keys.push_back(*id);
    }
    return true;
}

}  // namespace sspc::res
