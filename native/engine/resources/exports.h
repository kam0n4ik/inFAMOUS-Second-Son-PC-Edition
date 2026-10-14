// SecondSonPC native runtime - objects exported by packages, for resolving KNLX external links.
//
// Export entry (eboot lookup 0x3109d0, key hash 0x5ebee0), 0x20 bytes:
//   +0x00 u32 hash = XOR of the four u32 words of the key
//   +0x04 u8[16] key
//   +0x18 u64 pointer to the exported object (a KNLI pointer link in the file)
// Entries are found by checking every pointer link for this pattern (the XOR check makes chance
// matches negligible).
#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "resources/kcap.h"

namespace sspc::res {

struct ExportRef {
    const KcapPackage* package = nullptr;
    u32 object = 0;
};

class ExportIndex {
public:
    bool AddPackage(const std::wstring& path);  // opens and keeps the package mapped
    void AddPackage(const KcapPackage& pkg);    // not owned
    std::optional<ExportRef> Find(const std::array<u8, 16>& key) const;
    size_t Size() const { return map_.size(); }

private:
    struct KeyHash {
        size_t operator()(const std::array<u8, 16>& k) const;
    };
    std::vector<std::unique_ptr<KcapFile>> owned_;
    std::unordered_map<std::array<u8, 16>, ExportRef, KeyHash> map_;
};

}  // namespace sspc::res
