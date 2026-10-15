// SecondSonPC native runtime - the game's 64-bit name hash.
//
// h = h * 101 + lower(c), wrapping at 64 bits. Confirmed on the enum dictionary of the game
// (eboot 0xa5ff70, 4 458 of its 7 084 entries are "<ENUM>_<Value>" names, e.g. "cid_MISSION"), on
// all 2 709 decodable reflection field names (eboot 0xd057a0) and on the string ids stored in the
// script code of the packages (a localized text is referenced by the hash of its key, e.g.
// "u_brief_vor_escape_roof"). See docs/native/missions/MISSION_FORMAT_RESEARCH.md.
#pragma once

#include <string_view>

#include "core/types.h"

namespace sspc {

constexpr u64 NameHash(std::string_view s, u64 h = 0) {
    for (char c : s) {
        const char l = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        h = h * 101u + static_cast<u8>(l);
    }
    return h;
}

}  // namespace sspc
