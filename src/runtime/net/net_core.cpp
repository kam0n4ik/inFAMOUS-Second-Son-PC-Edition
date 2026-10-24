// SecondSonPC — libSceNet's library state (R10 sysrt; oracle: third_party/shadPS4/src/core/libraries/net/net.cpp).
// What the game calls (docs/R10_SYSRT.md §2): sceNetInit (eboot 0x5ed272: 0 or 0x804101c8 expected, reference counted
// by the game), sceNetPoolCreate (eboot 0x23699f: "...", 16 KiB, flags 0; the id goes to sceHttpInit) and
// sceNetInetNtop (eboot 0x5fd253: AF_INET, the address sceNetCtlGetInfo gave or 255.255.255.255 when it failed, a
// 32-byte buffer). With them: sceNetTerm, sceNetPoolDestroy, sceNetInetPton and sceNetErrnoLoc, which share the
// library's state (pools, errno).
//   Pools: ids 3000..3030, the first free one, as the oracle (name <= 31 bytes, flags 0, size >= 4 KiB); nothing is
//          allocated (no SecondSonPC function takes memory from a pool).
//   Errno: one cell per host thread (= guest thread), written only by the guest's library functions.
//   Text:  own conversions, not the host's: FreeBSD's inet_ntop/inet_pton (libc/inet, the console's libc family) —
//          Windows formats some IPv6 addresses otherwise (a single zero group as "::", embedded IPv4).
#include "runtime/net/net_core.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "base/log.h"
#include "runtime/exports.h"

namespace SecondSon::Runtime::Net {

namespace {

thread_local s32 g_errno = 0;

constexpr s32 kMinPoolSize = 4 * 1024;
constexpr size_t kDebugNameMax = 31;

struct Pool {
    bool in_use = false;
    s32 size = 0;
    char name[kDebugNameMax + 1] = {};
};
std::mutex g_mutex; // pools and hooks
std::array<Pool, kMaxPools> g_pools{};
BridgeHooks g_hooks{};

BridgeHooks Hooks() {
    std::scoped_lock lock{g_mutex};
    return g_hooks;
}

// ---- address text (FreeBSD lib/libc/inet/inet_ntop.c, inet_pton.c)

/// Formats a.b.c.d into dst (size bytes); false (ENOSPC) if it does not fit with its terminator.
bool Ntop4(const u8* src, char* dst, size_t size) {
    char tmp[sizeof "255.255.255.255"];
    const int n = std::snprintf(tmp, sizeof tmp, "%u.%u.%u.%u", src[0], src[1], src[2], src[3]);
    if (n < 0 || static_cast<size_t>(n) >= size) {
        return false;
    }
    std::memcpy(dst, tmp, static_cast<size_t>(n) + 1);
    return true;
}

bool Ntop6(const u8* src, char* dst, size_t size) {
    char tmp[sizeof "ffff:ffff:ffff:ffff:ffff:ffff:255.255.255.255"];
    u32 words[8];
    for (int i = 0; i < 8; ++i) {
        words[i] = (static_cast<u32>(src[2 * i]) << 8) | src[2 * i + 1];
    }
    // the longest run of zero words (the first of equal ones), if 2 words or longer, becomes "::"
    struct Run {
        int base = -1;
        int len = 0;
    } best, cur;
    for (int i = 0; i < 8; ++i) {
        if (words[i] == 0) {
            if (cur.base == -1) {
                cur = {i, 1};
            } else {
                ++cur.len;
            }
        } else if (cur.base != -1) {
            if (best.base == -1 || cur.len > best.len) {
                best = cur;
            }
            cur.base = -1;
        }
    }
    if (cur.base != -1 && (best.base == -1 || cur.len > best.len)) {
        best = cur;
    }
    if (best.base != -1 && best.len < 2) {
        best.base = -1;
    }
    char* tp = tmp;
    char* const end = tmp + sizeof tmp;
    for (int i = 0; i < 8; ++i) {
        if (best.base != -1 && i >= best.base && i < best.base + best.len) {
            if (i == best.base) {
                *tp++ = ':';
            }
            continue;
        }
        if (i != 0) {
            *tp++ = ':';
        }
        // IPv4-compatible (::a.b.c.d, not ::1) and IPv4-mapped (::ffff:a.b.c.d) addresses end in dotted decimal
        if (i == 6 && best.base == 0 &&
            (best.len == 6 || (best.len == 7 && words[7] != 0x0001) || (best.len == 5 && words[5] == 0xffff))) {
            if (!Ntop4(src + 12, tp, static_cast<size_t>(end - tp))) {
                return false;
            }
            tp += std::strlen(tp);
            break;
        }
        tp += std::snprintf(tp, static_cast<size_t>(end - tp), "%x", words[i]);
    }
    if (best.base != -1 && best.base + best.len == 8) {
        *tp++ = ':';
    }
    *tp++ = '\0';
    if (static_cast<size_t>(tp - tmp) > size) {
        return false;
    }
    std::memcpy(dst, tmp, static_cast<size_t>(tp - tmp));
    return true;
}

/// 1: four decimal octets (no leading zeros, each <= 255) stored in dst; 0: not an IPv4 address.
int Pton4(const char* src, u8* dst) {
    u8 tmp[4] = {};
    u8* tp = tmp;
    int saw_digit = 0, octets = 0;
    for (char ch; (ch = *src++) != '\0';) {
        if (ch >= '0' && ch <= '9') {
            const u32 v = *tp * 10u + static_cast<u32>(ch - '0');
            if (saw_digit && *tp == 0) {
                return 0;
            }
            if (v > 255) {
                return 0;
            }
            *tp = static_cast<u8>(v);
            if (!saw_digit) {
                if (++octets > 4) {
                    return 0;
                }
                saw_digit = 1;
            }
        } else if (ch == '.' && saw_digit) {
            if (octets == 4) {
                return 0;
            }
            *++tp = 0;
            saw_digit = 0;
        } else {
            return 0;
        }
    }
    if (octets < 4) {
        return 0;
    }
    std::memcpy(dst, tmp, 4);
    return 1;
}

int HexDigit(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

int Pton6(const char* src, u8* dst) {
    u8 tmp[16] = {};
    u8* tp = tmp;
    u8* const endp = tmp + 16;
    u8* colonp = nullptr;
    if (*src == ':' && *++src != ':') {
        return 0; // a leading ':' only as "::"
    }
    const char* curtok = src;
    int seen_xdigits = 0;
    u32 val = 0;
    for (char ch; (ch = *src++) != '\0';) {
        if (const int d = HexDigit(ch); d >= 0) {
            val = (val << 4) | static_cast<u32>(d);
            if (++seen_xdigits > 4) {
                return 0;
            }
            continue;
        }
        if (ch == ':') {
            curtok = src;
            if (seen_xdigits == 0) {
                if (colonp != nullptr) {
                    return 0;
                }
                colonp = tp;
                continue;
            }
            if (*src == '\0') {
                return 0;
            }
            if (tp + 2 > endp) {
                return 0;
            }
            *tp++ = static_cast<u8>(val >> 8);
            *tp++ = static_cast<u8>(val);
            seen_xdigits = 0;
            val = 0;
            continue;
        }
        if (ch == '.' && tp + 4 <= endp && Pton4(curtok, tp) > 0) {
            tp += 4;
            seen_xdigits = 0;
            break; // the dotted quad ends the address
        }
        return 0;
    }
    if (seen_xdigits != 0) {
        if (tp + 2 > endp) {
            return 0;
        }
        *tp++ = static_cast<u8>(val >> 8);
        *tp++ = static_cast<u8>(val);
    }
    if (colonp != nullptr) {
        // shift what follows "::" to the end
        const auto n = tp - colonp;
        if (tp == endp) {
            return 0;
        }
        for (ptrdiff_t i = 1; i <= n; ++i) {
            endp[-i] = colonp[n - i];
            colonp[n - i] = 0;
        }
        tp = endp;
    }
    if (tp != endp) {
        return 0;
    }
    std::memcpy(dst, tmp, 16);
    return 1;
}

s32 Fail(s32 e) {
    g_errno = e;
    return kErrorBase | e;
}

// ---- the exports

SSPC_SYSV_ABI s32 Init() {
    if (const auto hooks = Hooks(); hooks.init != nullptr) {
        hooks.init();
    }
    SSPC_LOG(Kernel, Info, "sceNetInit");
    return 0;
}

SSPC_SYSV_ABI s32 Term() {
    SSPC_LOG(Kernel, Info, "sceNetTerm");
    return 0;
}

SSPC_SYSV_ABI s32* ErrnoLoc() {
    return &g_errno;
}

SSPC_SYSV_ABI s32 PoolCreate(const char* name, s32 size, s32 flags) {
    // (errors are returned, the library's errno is not set: as the oracle)
    if (name != nullptr && strnlen(name, kDebugNameMax + 1) > kDebugNameMax) {
        return kErrorBase | kEnametoolong;
    }
    if (flags != 0 || size < kMinPoolSize) {
        return kErrorBase | kEinval;
    }
    std::scoped_lock lock{g_mutex};
    for (size_t i = 0; i < g_pools.size(); ++i) {
        if (g_pools[i].in_use) {
            continue;
        }
        Pool& p = g_pools[i];
        p = {};
        p.in_use = true;
        p.size = size;
        if (name != nullptr) {
            std::memcpy(p.name, name, strnlen(name, kDebugNameMax));
        }
        const s32 id = kFirstPoolId + static_cast<s32>(i);
        if (g_hooks.pool_created != nullptr) {
            const s32 bridge_id = g_hooks.pool_created(id, name, size, flags);
            if (bridge_id != id) {
                SSPC_LOG(Kernel, Error, "sceNetPoolCreate: pool {} is {:#x} in the bridge's socket layer", id,
                         static_cast<u32>(bridge_id));
            }
        }
        SSPC_LOG(Kernel, Info, "sceNetPoolCreate(\"{}\", {}, {:#x}) = {}", p.name, size, flags, id);
        return id;
    }
    return kErrorBase | kEnfile;
}

SSPC_SYSV_ABI s32 PoolDestroy(s32 pool_id) {
    std::scoped_lock lock{g_mutex};
    const auto index = static_cast<size_t>(pool_id - kFirstPoolId);
    if (pool_id < kFirstPoolId || index >= g_pools.size() || !g_pools[index].in_use) {
        return kErrorBase | kEbadf;
    }
    if (g_hooks.pool_destroy != nullptr) {
        if (const s32 r = g_hooks.pool_destroy(pool_id); r != 0) {
            return r; // (a resolver of the bridge still holds it: ENOTEMPTY)
        }
    }
    g_pools[index] = {};
    SSPC_LOG(Kernel, Info, "sceNetPoolDestroy({})", pool_id);
    return 0;
}

SSPC_SYSV_ABI const char* InetNtop(s32 af, const void* src, char* dst, u32 size) {
    if (src == nullptr || dst == nullptr) {
        g_errno = kEfault;
        return nullptr;
    }
    bool ok;
    if (af == kAfInet) {
        ok = Ntop4(static_cast<const u8*>(src), dst, size);
    } else if (af == kAfInet6) {
        ok = Ntop6(static_cast<const u8*>(src), dst, size);
    } else {
        g_errno = kEafnosupport;
        return nullptr;
    }
    if (!ok) {
        g_errno = kEnospc;
        return nullptr;
    }
    return dst;
}

SSPC_SYSV_ABI s32 InetPton(s32 af, const char* src, void* dst) {
    if (src == nullptr || dst == nullptr) {
        return Fail(kEfault);
    }
    if (af == kAfInet) {
        return Pton4(src, static_cast<u8*>(dst)); // 1, or 0 for malformed text
    }
    if (af == kAfInet6) {
        return Pton6(src, static_cast<u8*>(dst));
    }
    return Fail(kEafnosupport);
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

#define NET(nid, name, fn) {nid, "libSceNet", "libSceNet", name, Fn(&fn), nullptr, "sysrt-net", false, true}
const Export kExports[] = {
    NET("Nlev7Lg8k3A", "sceNetInit", Init),
    NET("cTGkc6-TBlI", "sceNetTerm", Term),
    NET("HQOwnfMGipQ", "sceNetErrnoLoc", ErrnoLoc),
    NET("dgJBaeJnGpo", "sceNetPoolCreate", PoolCreate),
    NET("K7RlrTkI-mw", "sceNetPoolDestroy", PoolDestroy),
    NET("9vA2aW+CHuA", "sceNetInetNtop", InetNtop),
    NET("8Kcp5d-q1Uo", "sceNetInetPton", InetPton),
};
#undef NET

} // namespace

void SetBridgeHooks(const BridgeHooks& hooks) {
    std::scoped_lock lock{g_mutex};
    g_hooks = hooks;
}

s32* ErrnoLocation() {
    return &g_errno;
}

void SetErrno(s32 value) {
    g_errno = value;
}

std::span<const Export> NetCoreExports() {
    return kExports;
}

} // namespace SecondSon::Runtime::Net
