#include "resources/texture_library.h"

namespace sspc::res {

bool TextureLibrary::AddPackage(const std::wstring& path) {
    auto file = std::make_unique<KcapFile>();
    if (file->Open(path) != KcapError::Ok) return false;
    Index(file->Package());
    owned_.push_back(std::move(file));
    return true;
}

void TextureLibrary::AddPackage(const KcapPackage& pkg) { Index(pkg); }

void TextureLibrary::Index(const KcapPackage& pkg) {
    for (const TextureInfo& t : FindTextures(pkg)) index_.try_emplace(t.id, Entry{&pkg, t});
}

bool TextureLibrary::Decode(u64 id, TextureData& out, std::string* error) const {
    const auto it = index_.find(id);
    if (it == index_.end()) {
        if (error) *error = "texture id not in any library package";
        return false;
    }
    return DecodeTexture(*it->second.package, it->second.info, out, error);
}

}  // namespace sspc::res
