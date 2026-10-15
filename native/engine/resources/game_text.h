// SecondSonPC native runtime - localized text of the game (lang_<language>_text.xpps, package kind 7).
//
// GAMETEXT object (confirmed on lang_english_text.xpps): +0x40 pointer to the string table, +0x48 count
// (12 091 entries). Entry 0x20 bytes: {u64 NameHash(key), pointer to "key\0text\0", 16 zero bytes},
// sorted by the hash as a signed 64-bit value. Scripts refer to a text by the hash of its key
// (e.g. TRAINER_HINT.text = NameHash("u_hint_move")). Texts are UTF-8; controller button glyphs are
// private-use code points (U+F083 ...). Dialogue lines (entry byte +0x15 = 6) carry four u16 fields
// between the key and the text, 2-byte aligned (e.g. 1, 6, 125, 8 - roles not decoded).
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/types.h"
#include "resources/kcap.h"

namespace sspc::res {

struct TextEntry {
    std::string_view key;
    std::string_view text;  // UTF-8
};

class GameText {
public:
    bool Open(const std::wstring& path, std::string* error);
    std::optional<TextEntry> Find(u64 key_hash) const;
    std::optional<TextEntry> Find(std::string_view key) const;
    size_t Count() const { return entries_.size(); }

private:
    struct Entry {
        s64 hash;
        u32 string;  // data offset of "key\0[header]text\0"
        u8 layout;   // entry byte +0x15: 2 = plain text; larger = (layout - 2) u16 fields before the text
    };
    KcapFile file_;
    std::vector<Entry> entries_;
};

}  // namespace sspc::res
