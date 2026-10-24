// SecondSonPC — libSceNet's library state: start-up, memory pools, errno, address text (R10 sysrt, docs/R10_SYSRT.md).
// Group "sysrt-net" (experimental: BRIDGE unless --native sysrt-net or SSPC_R10_SYSRT=native).
#pragma once

#include <span>

#include "base/abi.h"

namespace SecondSon::Runtime {
struct Export;
}

namespace SecondSon::Runtime::Net {

// libSceNet's errno values (FreeBSD's, as the console's libSceNet; the oracle's net_error.h) and its error codes.
constexpr s32 kEbadf = 9;
constexpr s32 kEfault = 14;
constexpr s32 kEinval = 22;
constexpr s32 kEnfile = 23;
constexpr s32 kEnospc = 28;
constexpr s32 kEafnosupport = 47;
constexpr s32 kEnametoolong = 63;
constexpr s32 kEnotempty = 66;
constexpr s32 kErrorBase = static_cast<s32>(0x80410100); // SCE_NET_ERROR_<E> = base | errno
constexpr s32 kAfInet = 2;
constexpr s32 kAfInet6 = 28;
constexpr s32 kFirstPoolId = 3000;
constexpr int kMaxPools = 31;

/// The bridge's socket layer (sockets, epoll, resolvers: imported, never called offline) stays the bridge's while
/// these functions are SecondSonPC's. It keeps its own host start-up and pool table; SecondSonPC's functions tell it
/// (src/bridge/shadps4/sysrt_host.cpp), and its errno reaches SecondSonPC's through SetErrno below.
struct BridgeHooks {
    void (*init)() = nullptr;                                                // sceNetInit
    s32 (*pool_created)(s32 id, const char* name, s32 size, s32 flags) = nullptr; // returns the bridge's id for it
    s32 (*pool_destroy)(s32 id) = nullptr; // 0: released there (or unknown there); else the error that keeps it
};
void SetBridgeHooks(const BridgeHooks& hooks);

/// The calling thread's libSceNet errno (what sceNetErrnoLoc returns): 0 when the thread starts.
s32* ErrnoLocation();
/// Sets it (the bridge's functions of the library report their errno here).
void SetErrno(s32 value);

std::span<const Export> NetCoreExports();

} // namespace SecondSon::Runtime::Net
