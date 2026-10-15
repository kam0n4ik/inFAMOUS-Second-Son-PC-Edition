#include "resources/type_registry.h"

#include <algorithm>
#include <array>

namespace sspc::res {
namespace {

struct Entry {
    u64 hash;
    u32 index;
    const char* name;
};

constexpr Entry kTypes[] = {
#include "resources/type_registry_table.inc"
};

}  // namespace

std::optional<TypeInfo> FindType(u64 hash) {
    const auto it = std::lower_bound(std::begin(kTypes), std::end(kTypes), hash,
                                     [](const Entry& e, u64 h) { return e.hash < h; });
    if (it == std::end(kTypes) || it->hash != hash) return std::nullopt;
    return TypeInfo{it->hash, it->index, it->name};
}

std::optional<u64> TypeHash(std::string_view name) {
    for (const Entry& e : kTypes) {
        if (name == e.name) return e.hash;
    }
    return std::nullopt;
}

size_t TypeCount() { return std::size(kTypes); }

}  // namespace sspc::res
