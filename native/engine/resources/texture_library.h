// SecondSonPC native runtime - textures looked up by id across several packages.
//
// Materials name their textures by 64-bit id; a texture can live in the material's own package or
// in a shared one (e.g. hero.flavor(default).xpps, core_common.sprig.xpps). The library keeps the
// packages memory-mapped and decodes a texture on request.
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "resources/kcap.h"
#include "resources/texture.h"

namespace sspc::res {

class TextureLibrary {
public:
    // Adds a package to search; the first package that holds an id wins. Returns false if the
    // package cannot be opened (the library stays usable).
    bool AddPackage(const std::wstring& path);
    // Adds an already opened package (not owned; must outlive the library).
    void AddPackage(const KcapPackage& pkg);

    bool Contains(u64 id) const { return index_.contains(id); }
    bool Decode(u64 id, TextureData& out, std::string* error) const;
    size_t TextureCount() const { return index_.size(); }

private:
    struct Entry {
        const KcapPackage* package;
        TextureInfo info;
    };
    void Index(const KcapPackage& pkg);

    std::vector<std::unique_ptr<KcapFile>> owned_;
    std::unordered_map<u64, Entry> index_;
};

}  // namespace sspc::res
