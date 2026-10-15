#include "resources/game_text.h"

#include <algorithm>
#include <cstring>
#include <format>

#include "core/log.h"
#include "core/name_hash.h"
#include "resources/game_objects.h"

namespace sspc::res {
namespace {

constexpr u32 kMaxEntries = 1u << 20;
constexpr u32 kMaxString = 64 * 1024;

std::string_view CString(const KcapPackage& pkg, u32 at) {
    const auto data = pkg.Data();
    if (at >= data.size()) return {};
    const size_t limit = std::min<size_t>(data.size() - at, kMaxString);
    const auto* p = reinterpret_cast<const char*>(data.data() + at);
    const size_t len = strnlen(p, limit);
    if (len == limit) return {};  // not terminated within the limit
    return {p, len};
}

}  // namespace

bool GameText::Open(const std::wstring& path, std::string* error) {
    entries_.clear();
    if (const auto e = file_.Open(path); e != KcapError::Ok) {
        if (error) *error = std::format("{}: {}", log::Narrow(path), ToString(e));
        return false;
    }
    const KcapPackage& pkg = file_.Package();
    ObjectCatalog catalog;
    if (!catalog.Build(pkg, error)) return false;
    const auto texts = catalog.OfType("GAMETEXT");
    if (texts.empty()) {
        if (error) *error = "no GAMETEXT object";
        return false;
    }
    const u32 obj = texts.front()->offset;
    const auto table = pkg.PointerAt(obj + 0x40);
    u64 count = 0;
    if (!table || !pkg.Read(obj + 0x48, count) || count == 0 || count > kMaxEntries || !pkg.InRange(*table, count * 0x20)) {
        if (error) *error = "GAMETEXT string table invalid";
        return false;
    }
    entries_.reserve(static_cast<size_t>(count));
    for (u32 i = 0; i < count; ++i) {
        const u32 e = *table + i * 0x20;
        u64 hash = 0;
        pkg.Read(e, hash);
        auto s = pkg.PointerAt(e + 8);
        if (!s) {
            // Some entries (dialogue lines) carry the offset without a KNLI fixup: accept the raw
            // offset only when the key stored there hashes to the entry's hash.
            u32 raw = 0;
            pkg.Read(e + 8, raw);
            const std::string_view key = CString(pkg, raw);
            if (key.empty() || NameHash(key) != hash) continue;
            s = raw;
        }
        u8 layout = 2;
        pkg.Read(e + 0x15, layout);
        entries_.push_back({static_cast<s64>(hash), *s, layout});
    }
    // Stored sorted by signed hash; keep the order guaranteed for the binary search.
    if (!std::is_sorted(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) { return a.hash < b.hash; })) {
        std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) { return a.hash < b.hash; });
    }
    return !entries_.empty();
}

std::optional<TextEntry> GameText::Find(u64 key_hash) const {
    const s64 h = static_cast<s64>(key_hash);
    const auto it = std::lower_bound(entries_.begin(), entries_.end(), h, [](const Entry& e, s64 v) { return e.hash < v; });
    if (it == entries_.end() || it->hash != h) return std::nullopt;
    const KcapPackage& pkg = file_.Package();
    const std::string_view key = CString(pkg, it->string);
    if (key.empty()) return std::nullopt;
    u32 at = it->string + static_cast<u32>(key.size()) + 1;
    if (it->layout > 2 && it->layout < 32) at = ((at + 1) & ~1u) + (it->layout - 2u) * 2u;
    std::string_view text = CString(pkg, at);
    if (it->layout > 2 && (text.empty() || static_cast<u8>(text.front()) < 0x20)) {
        // Unknown header length: take the first printable run within the next 32 bytes.
        for (u32 k = it->string + static_cast<u32>(key.size()) + 1; k < at + 32; ++k) {
            const std::string_view t = CString(pkg, k);
            if (t.size() >= 2 && static_cast<u8>(t.front()) >= 0x20) {
                text = t;
                break;
            }
        }
    }
    return TextEntry{key, text};
}

std::optional<TextEntry> GameText::Find(std::string_view key) const { return Find(NameHash(key)); }

}  // namespace sspc::res
