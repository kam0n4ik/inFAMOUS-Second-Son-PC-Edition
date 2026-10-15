#include "resources/game_objects.h"

#include <cmath>
#include <format>

#include "resources/type_registry.h"

namespace sspc::res {
namespace {

constexpr u32 kMaxObjects = 1u << 20;
constexpr u32 kMaxRecords = 1u << 22;

u32 AlignUp(u32 v, u32 a) { return (v + a - 1) & ~(a - 1); }

}  // namespace

bool ObjectCatalog::Build(const KcapPackage& pkg, std::string* error) {
    pkg_ = &pkg;
    objects_.clear();
    dic_.clear();
    for (const KcapChunk& c : pkg.Chunks()) {
        if (c.tag != kTagDic || c.size < 8) continue;
        u32 n = 0;
        pkg.Read(c.payload, n);
        if (n > kMaxRecords || 8 + static_cast<u64>(n) * 16 > c.size) {
            if (error) *error = std::format("DIC chunk at {:#x}: {} records do not fit {} bytes", c.payload, n, c.size);
            return false;
        }
        for (u32 i = 0; i < n; ++i) {
            u32 loc = 0;
            u64 hash = 0;
            pkg.Read(c.payload + 8 + i * 16, loc);
            pkg.Read(c.payload + 8 + i * 16 + 8, hash);
            dic_[loc] = hash;
        }
    }
    const KcapBlock* root = nullptr;
    for (const KcapBlock& b : pkg.Blocks()) {
        if (b.type == kBlockTypeRoot && !b.is_virtual) {
            root = &b;
            break;
        }
    }
    if (!root) {
        if (error) *error = "no root catalog (block type 6)";
        return false;
    }
    u32 n = 0;
    if (!pkg.Read(root->offset, n) || n > kMaxObjects || 8 + static_cast<u64>(n) * 8 > root->size) {
        if (error) *error = std::format("root catalog at {:#x}: bad object count {}", root->offset, n);
        return false;
    }
    objects_.reserve(n);
    for (u32 i = 0; i < n; ++i) {
        const auto p = pkg.PointerAt(root->offset + 8 + i * 8);
        if (!p || !pkg.InRange(*p, 0x40)) continue;
        GameObject o;
        o.offset = *p;
        if (const auto it = dic_.find(*p + 0x10); it != dic_.end()) o.type_hash = it->second;
        if (o.type_hash) {
            if (const auto t = FindType(o.type_hash)) o.type_name = t->name;
        }
        pkg.Read(*p + 0x18, o.oid);
        pkg.Read(*p + 0x38, o.flags);
        objects_.push_back(o);
    }
    return true;
}

std::vector<const GameObject*> ObjectCatalog::OfType(std::string_view type_name) const {
    std::vector<const GameObject*> out;
    for (const GameObject& o : objects_) {
        if (o.type_name == type_name) out.push_back(&o);
    }
    return out;
}

const GameObject* ObjectCatalog::FindByOid(u64 oid) const {
    for (const GameObject& o : objects_) {
        if (o.oid == oid) return &o;
    }
    return nullptr;
}

std::optional<u64> ObjectCatalog::DicHashAt(u32 location) const {
    const auto it = dic_.find(location);
    if (it == dic_.end()) return std::nullopt;
    return it->second;
}

std::optional<ObjectTransform> ObjectCatalog::Transform(const GameObject& obj) const {
    if (!pkg_) return std::nullopt;
    float v[12];
    if (!pkg_->Read(obj.offset + 0x60, v)) return std::nullopt;
    // +0x60 scale xyz (+pad), +0x70 quaternion, +0x80 position xyz + w = 1.
    if (std::abs(v[11] - 1.0f) > 1e-4f) return std::nullopt;
    for (float f : v) {
        if (!std::isfinite(f)) return std::nullopt;
    }
    ObjectTransform t;
    t.scale = {v[0], v[1], v[2]};
    t.rotation = Normalize(Quat{v[4], v[5], v[6], v[7]});
    t.position_cm = {v[8], v[9], v[10]};
    return t;
}

bool ParseSpliceLinks(const KcapPackage& pkg, SpliceLinks& out, std::string* error) {
    out = SpliceLinks{};
    for (const KcapChunk& c : pkg.Chunks()) {
        if (c.tag != kTagPlps) continue;
        u32 n1 = 0, n2 = 0, n3 = 0;
        if (c.size < 0x10 || !pkg.Read(c.payload, n1) || !pkg.Read(c.payload + 4, n2) || !pkg.Read(c.payload + 8, n3)) {
            if (error) *error = std::format("PLPS chunk at {:#x} too small", c.payload);
            return false;
        }
        const u64 tables = 0x10 + static_cast<u64>(n1) * 0x18 + static_cast<u64>(n2) * 0x30 + static_cast<u64>(n3) * 0x28;
        if (n1 > kMaxRecords || n2 > kMaxRecords || n3 > kMaxRecords || tables > c.size) {
            if (error) *error = std::format("PLPS chunk at {:#x}: record tables ({}, {}, {}) exceed {} bytes", c.payload, n1, n2,
                                            n3, c.size);
            return false;
        }
        u32 o = c.payload + 0x10;
        for (u32 i = 0; i < n1; ++i, o += 0x18) {
            SpliceMemberRef r;
            pkg.Read(o, r.location);
            pkg.Read(o + 8, r.class_hash);
            pkg.Read(o + 16, r.member_hash);
            pkg.Read(r.location, r.opcode);
            pkg.Read(r.location + 2, r.value_type);
            out.members.push_back(r);
        }
        for (u32 i = 0; i < n2; ++i, o += 0x30) {
            SpliceCall call;
            pkg.Read(o, call.location);
            pkg.Read(o + 4, call.args_offset);
            pkg.Read(o + 8, call.class_hash);
            pkg.Read(o + 16, call.member_hash);
            pkg.Read(o + 0x28, call.argc);
            out.methods.push_back(call);
        }
        for (u32 i = 0; i < n3; ++i, o += 0x28) {
            SpliceCall call;
            pkg.Read(o, call.location);
            pkg.Read(o + 4, call.args_offset);
            pkg.Read(o + 8, call.member_hash);
            pkg.Read(o + 0x20, call.argc);
            out.globals.push_back(call);
        }
    }
    return true;
}

std::optional<float> ImmediateFloat(const KcapPackage& pkg, const SpliceMemberRef& ref) {
    if (ref.opcode != 0x0e || ref.value_type != static_cast<u16>(SpliceValueType::Float)) return std::nullopt;
    float v = 0;
    if (!pkg.Read(ref.location + 0x18, v) || !std::isfinite(v)) return std::nullopt;
    return v;
}

std::optional<u64> ImmediateId(const KcapPackage& pkg, const SpliceMemberRef& ref) {
    if (ref.value_type != static_cast<u16>(SpliceValueType::Id)) return std::nullopt;
    u32 at = 0;
    if (ref.opcode == 0x0e) at = AlignUp(ref.location + 0x18, 8);
    else if (ref.opcode == 0x14) at = ref.location + 0x28;
    else return std::nullopt;
    u64 v = 0;
    if (!pkg.Read(at, v)) return std::nullopt;
    return v;
}

std::optional<Vec4> ImmediateVector(const KcapPackage& pkg, const SpliceMemberRef& ref) {
    if (ref.opcode != 0x0e || (ref.value_type != static_cast<u16>(SpliceValueType::Point) &&
                               ref.value_type != static_cast<u16>(SpliceValueType::Vector))) {
        return std::nullopt;
    }
    float v[4];
    if (!pkg.Read(AlignUp(ref.location + 0x18, 16), v)) return std::nullopt;
    for (float f : v) {
        if (!std::isfinite(f)) return std::nullopt;
    }
    return Vec4{v[0], v[1], v[2], v[3]};
}

u32 CallArgumentBlock(const SpliceCall& call) { return AlignUp(call.location + 0x30, 16); }

}  // namespace sspc::res
