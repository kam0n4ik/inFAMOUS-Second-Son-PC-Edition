// SecondSonPC native runtime - names of the game object types found in the packages.
//
// Every object of a package names its type in a DIC fixup (docs/native/missions/MISSION_FORMAT_RESEARCH.md):
//  * a type-name hash (not derivable from the name; the game looks it up in its dictionary), for the
//    data records such as MISSION_DATA or GRAFFITI_MINIGAME_DATA - resolved by a table generated once
//    from the dictionary for the types that occur in the packages this runtime reads
//    (tools/research/missions/gen_type_table.py);
//  * or a class-id enum value, NameHash("cid_" + ClassName) with the CamelCase class name
//    ("cid_GraffitiMinigame"), which is computable and needs no table. Script link records (PLPS) name
//    classes this way too.
#pragma once

#include <optional>
#include <string_view>

#include "core/name_hash.h"
#include "core/types.h"

namespace sspc::res {

struct TypeInfo {
    u64 hash = 0;
    u32 index = 0;           // index into the game's class-name table (same numbering as the cid enum)
    std::string_view name;   // e.g. "MISSION_DATA"
};

// Type-name hash -> type (nullopt for hashes outside the generated table).
std::optional<TypeInfo> FindType(u64 hash);
// Name -> type-name hash (nullopt if the type is not in the table).
std::optional<u64> TypeHash(std::string_view name);
size_t TypeCount();

// Class-id enum value of a class as stored in script link records: NameHash("cid_" + camel_name).
constexpr u64 ClassIdHash(std::string_view camel_name) { return NameHash(camel_name, NameHash("cid_")); }

}  // namespace sspc::res
