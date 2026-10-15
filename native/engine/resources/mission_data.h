// SecondSonPC native runtime - what a mission package of the original game contains.
//
// Mission packages (msn_<name>.sprig.xpps) are KCAP kind 0xA: each holds exactly one MISSION_DATA
// object, its OBJECTIVE_DATA objects, rules (EVENT_RULE/UPDATE_RULE/PARALLEL_TASK), dialogue, trigger
// volumes (VOLUME_BUTTON_DATA), hero placement points (HERO_MATCH_POINT_DATA) and briefs - all with
// world-frame transforms (docs/native/missions/MISSION_FORMAT_RESEARCH.md). Their logic is compiled
// splice code that this runtime does not execute; this reader exposes the data the native mission
// definitions refer to (objects by OID, texts by key hash).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/math.h"
#include "core/types.h"
#include "resources/game_objects.h"

namespace sspc::res {

struct MissionPoint {
    u64 oid = 0;
    Vec3 position_cm;  // world frame
    Quat rotation;
    Vec3 scale{1, 1, 1};
};

struct MissionPackageInfo {
    u64 mission_oid = 0;
    std::vector<u64> objectives;            // OBJECTIVE_DATA OIDs in catalog order
    std::vector<MissionPoint> hero_points;  // HERO_MATCH_POINT_DATA
    std::vector<MissionPoint> volumes;      // VOLUME_BUTTON_DATA (the shape the scale applies to is not decoded)
    std::vector<u64> text_keys;             // NameHash of texts set by the scripts (immediate ids of text members)
    u32 objects = 0;

    const MissionPoint* HeroPoint(u64 oid) const;
    const MissionPoint* Volume(u64 oid) const;
};

bool LoadMissionPackage(const KcapPackage& pkg, MissionPackageInfo& out, std::string* error);

}  // namespace sspc::res
