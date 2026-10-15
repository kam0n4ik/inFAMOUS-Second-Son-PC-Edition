// SecondSonPC native runtime - game objects of a KCAP package and the link records of their scripts.
//
// Confirmed on the mission (kind 0xA), graffiti (0x19), chunk (4) and text (7) packages
// (docs/native/missions/MISSION_FORMAT_RESEARCH.md):
//  * root catalog = block type 6: {u32 n, u32 n, n object pointers from +8} (block size 8 + 8n);
//  * object header: +0x10 type (DIC fixup: the file stores 0, the DIC record carries the hash),
//    +0x18 u64 OID, +0x30 pointer into the catalog, +0x38 u32 flags (low 16 bits 0xffff = no parent);
//    spatial objects continue with +0x60 scale (3 x f32), +0x70 rotation quaternion (x, y, z, w),
//    +0x80 position in centimetres (x, y, z, 1) relative to the parent;
//  * DIC chunk: {u32 n, u32 0, n x {u32 location, u32 0, u64 hash}};
//  * PLPS chunk (eboot linker 0x30b1c0): the link table of compiled splice (script) code. Each record
//    names an instruction location and the class member or function it uses; the linker rewrites the
//    instruction to a direct accessor. Immediate operands of property-setting instructions stay in the
//    file and are read here (layouts measured on the packages, see the research document).
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/math.h"
#include "core/types.h"
#include "resources/kcap.h"

namespace sspc::res {

struct GameObject {
    u32 offset = 0;      // data offset of the object header
    u64 type_hash = 0;   // DIC hash stored for +0x10 (0 = none)
    std::string_view type_name;  // from the type registry; empty if unknown (e.g. class-id enum types)
    u64 oid = 0;
    u32 flags = 0;
};

struct ObjectTransform {
    Vec3 scale{1, 1, 1};
    Quat rotation;
    Vec3 position_cm;  // relative to the parent object
};

class ObjectCatalog {
public:
    // Reads the root catalog and the DIC fixups. Returns false if the package has no catalog.
    bool Build(const KcapPackage& pkg, std::string* error);

    const KcapPackage* Package() const { return pkg_; }
    const std::vector<GameObject>& Objects() const { return objects_; }
    std::vector<const GameObject*> OfType(std::string_view type_name) const;
    const GameObject* FindByOid(u64 oid) const;
    // DIC hash recorded for a location (nullopt if no fixup there).
    std::optional<u64> DicHashAt(u32 location) const;
    // Spatial block +0x60..+0x8c; nullopt if it is absent (the position w component must be 1).
    std::optional<ObjectTransform> Transform(const GameObject& obj) const;

private:
    const KcapPackage* pkg_ = nullptr;
    std::vector<GameObject> objects_;
    std::unordered_map<u32, u64> dic_;
};

// Value types carried in the 16-bit type field of a splice instruction (byte 0 opcode, bytes 2..3 type).
enum class SpliceValueType : u16 { Bool = 1, Int = 2, Float = 3, Point = 4, Vector = 5, Id = 7 };

struct SpliceMemberRef {
    u32 location = 0;     // instruction
    u64 class_hash = 0;   // class-id enum hash (ClassIdHash)
    u64 member_hash = 0;  // NameHash of the member, e.g. NameHash("CAMERA_POS")
    u8 opcode = 0;
    u16 value_type = 0;
};

struct SpliceCall {
    u32 location = 0;
    u32 args_offset = 0;  // relative to the argument area of the PLPS chunk
    u64 class_hash = 0;   // 0 for global functions
    u64 member_hash = 0;  // method name hash, or the global function hash
    u8 argc = 0;
};

struct SpliceLinks {
    std::vector<SpliceMemberRef> members;
    std::vector<SpliceCall> methods;
    std::vector<SpliceCall> globals;
};

// Parses every PLPS chunk of the package. Returns false on a malformed record table.
bool ParseSpliceLinks(const KcapPackage& pkg, SpliceLinks& out, std::string* error);

// Immediate operand of a property-setting instruction:
//   opcode 0x0e: value at loc + 0x18 aligned to the value (4 float, 8 id, 16 point/vector);
//   opcode 0x14 with an id: value at loc + 0x28.
// nullopt if the instruction or its type does not match.
std::optional<float> ImmediateFloat(const KcapPackage& pkg, const SpliceMemberRef& ref);
std::optional<u64> ImmediateId(const KcapPackage& pkg, const SpliceMemberRef& ref);
std::optional<Vec4> ImmediateVector(const KcapPackage& pkg, const SpliceMemberRef& ref);
// Start of the in-line argument block of a method call instruction: loc + 0x30 aligned to 16.
u32 CallArgumentBlock(const SpliceCall& call);

}  // namespace sspc::res
