#include "resources/exports.h"

#include <cstring>

namespace sspc::res {

size_t ExportIndex::KeyHash::operator()(const std::array<u8, 16>& k) const {
    u64 a, b;
    std::memcpy(&a, k.data(), 8);
    std::memcpy(&b, k.data() + 8, 8);
    return static_cast<size_t>(a ^ (b * 0x9E3779B97F4A7C15ull));
}

bool ExportIndex::AddPackage(const std::wstring& path) {
    auto f = std::make_unique<KcapFile>();
    if (f->Open(path) != KcapError::Ok) return false;
    AddPackage(f->Package());
    owned_.push_back(std::move(f));
    return true;
}

void ExportIndex::AddPackage(const KcapPackage& pkg) {
    for (const KcapLink& l : pkg.Links()) {
        if ((l.kind != LinkKind::Pointer && l.kind != LinkKind::Pointer64) || l.location < 0x18) continue;
        const u32 entry = l.location - 0x18;
        u32 w[5];
        if (!pkg.Read(entry, w)) continue;
        if (w[0] != (w[1] ^ w[2] ^ w[3] ^ w[4]) || (w[1] | w[2] | w[3] | w[4]) == 0) continue;
        std::array<u8, 16> key;
        std::memcpy(key.data(), &w[1], 16);
        map_.try_emplace(key, ExportRef{&pkg, l.target});
    }
}

std::optional<ExportRef> ExportIndex::Find(const std::array<u8, 16>& key) const {
    const auto it = map_.find(key);
    if (it == map_.end()) return std::nullopt;
    return it->second;
}

}  // namespace sspc::res
