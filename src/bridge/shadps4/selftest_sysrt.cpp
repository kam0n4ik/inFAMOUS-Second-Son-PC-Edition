// SecondSonPC — self-test of R10 sysrt (docs/R10_SYSRT.md §6): SecondSonPC's CommonDialog, libSceNet library state and
// libSceLibcInternal stdout / heap trace functions against the bridge's (the oracle) on the same inputs, through the
// exports the game binds (System V), and the adapters of src/bridge/shadps4/sysrt_host.cpp. Where the oracle is not
// the reference (vprintf/_Stdout: no oracle; IPv6 text: the host's inet_ntop) the expected values are FreeBSD's and a
// different oracle result is reported as an INFO line, not a failure.
//   --selftest (all groups) or SSPC_SELFTEST_ONLY=sysrt --selftest (these alone).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "app/config.h"
#include "bridge/shadps4/selftest_vmm.h"
#include "bridge/shadps4/sysrt_host.h"
#include "common/va_ctx.h" // (libc_internal_io.h uses its VA_ARGS without including it)
#include "core/libraries/libc_internal/libc_internal_io.h"
#include "core/libraries/libc_internal/printf.h"
#include "core/libraries/net/net.h"
#include "core/libraries/system/commondialog.h"
#include "core/libraries/system/msgdialog.h"
#include "runtime/exports.h"
#include "runtime/libc/guest_printf.h"
#include "runtime/libc/libc_stdio.h"
#include "runtime/net/net_core.h"
#include "runtime/system/common_dialog.h"

namespace Libraries::LibcInternal { // libc_internal_memory.cpp (the structure is that file's own)
struct HeapInfoInfo;
void PS4_SYSV_ABI sceLibcHeapGetTraceInfo(HeapInfoInfo* info);
} // namespace Libraries::LibcInternal

namespace SecondSon::Bridge {

namespace {

namespace RNet = Runtime::Net;
namespace RLibc = Runtime::Libc;

template <typename F>
F Export(std::string_view name) {
    for (const auto& e : Runtime::Exports()) {
        if (name == e.name) {
            return reinterpret_cast<F>(e.function);
        }
    }
    return nullptr;
}

std::string Hex(const void* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s += fmt::format("{:02x}", static_cast<const u8*>(p)[i]);
    }
    return s;
}

// ------------------------------------------------------------------------------------------------ CommonDialog

void DialogBefore(TestGroup& c) {
    const auto r = static_cast<u32>(Libraries::MsgDialog::sceMsgDialogInitialize());
    c.Check("bridge sceMsgDialogInitialize before any CommonDialog initialisation: NOT_SYSTEM_INITIALIZED", r == 0x80B80001u,
            fmt::format("{:#x}", r));
}

void DialogLifecycle(TestGroup& c) {
    using Init = s32(SSPC_SYSV_ABI*)();
    const auto init = Export<Init>("sceCommonDialogInitialize");
    if (init == nullptr) {
        c.Check("export sceCommonDialogInitialize", false, "missing");
        return;
    }
    c.Check("not initialised at start", !Runtime::CommonDialog::Initialized(), "");
    const s32 first = init();
    c.Check("first sceCommonDialogInitialize = 0", first == 0, fmt::format("{:#x}", static_cast<u32>(first)));
    c.Check("initialised after it", Runtime::CommonDialog::Initialized(), "");
    const auto msg = static_cast<u32>(Libraries::MsgDialog::sceMsgDialogInitialize());
    c.Check("the bridge's MsgDialog sees the system initialised (adapter): sceMsgDialogInitialize = 0", msg == 0,
            fmt::format("{:#x}", msg));
    const auto term = static_cast<u32>(Libraries::MsgDialog::sceMsgDialogTerminate());
    c.Check("bridge sceMsgDialogTerminate = 0", term == 0, fmt::format("{:#x}", term));
    const s32 second = init();
    const auto oracle = static_cast<u32>(Libraries::CommonDialog::sceCommonDialogInitialize());
    c.Check("second call: ALREADY_SYSTEM_INITIALIZED (0x80b80002), as the oracle's second call",
            static_cast<u32>(second) == 0x80B80002u && oracle == 0x80B80002u,
            fmt::format("native {:#x} oracle {:#x}", static_cast<u32>(second), oracle));
    s32 from_threads[4] = {};
    std::vector<std::thread> t;
    for (auto& r : from_threads) {
        t.emplace_back([&r, init] { r = init(); });
    }
    for (auto& x : t) {
        x.join();
    }
    bool all = true;
    for (const s32 r : from_threads) {
        all &= static_cast<u32>(r) == 0x80B80002u;
    }
    c.Check("4 threads after it: all ALREADY_SYSTEM_INITIALIZED", all, "");
}

// ------------------------------------------------------------------------------------------------ libSceNet

using PoolCreateFn = s32(SSPC_SYSV_ABI*)(const char*, s32, s32);
using PoolDestroyFn = s32(SSPC_SYSV_ABI*)(s32);
using NtopFn = const char*(SSPC_SYSV_ABI*)(s32, const void*, char*, u32);
using PtonFn = s32(SSPC_SYSV_ABI*)(s32, const char*, void*);
using ErrnoFn = s32*(SSPC_SYSV_ABI*)();

/// The same pool script on one implementation; the results in order.
std::vector<s32> PoolScript(PoolCreateFn create, PoolDestroyFn destroy) {
    std::vector<s32> r;
    const std::string long_name(32, 'n');
    r.push_back(create(long_name.c_str(), 4096, 0));
    r.push_back(create(std::string(31, 'n').c_str(), 4096, 0));
    r.push_back(destroy(r.back()));
    r.push_back(create("a", 4096, 1));
    r.push_back(create("a", 4095, 0));
    const s32 a = create("a", 4096, 0);
    const s32 b = create(nullptr, 16384, 0);
    r.push_back(a);
    r.push_back(b);
    r.push_back(destroy(a));
    r.push_back(destroy(a));
    r.push_back(destroy(2999));
    r.push_back(destroy(3031));
    r.push_back(destroy(0));
    const s32 again = create("again", 8192, 0);
    r.push_back(again);
    r.push_back(destroy(again));
    r.push_back(destroy(b));
    std::vector<s32> ids;
    for (int i = 0; i < 32; ++i) {
        ids.push_back(create("fill", 4096, 0));
    }
    r.insert(r.end(), ids.begin(), ids.end());
    for (const s32 id : ids) {
        r.push_back(destroy(id));
    }
    return r;
}

void NetPools(TestGroup& c) {
    const auto create = Export<PoolCreateFn>("sceNetPoolCreate");
    const auto destroy = Export<PoolDestroyFn>("sceNetPoolDestroy");
    if (create == nullptr || destroy == nullptr) {
        c.Check("exports sceNetPoolCreate/Destroy", false, "missing");
        return;
    }
    const auto oracle = PoolScript(&Libraries::Net::sceNetPoolCreate, &Libraries::Net::sceNetPoolDestroy);
    const auto native = PoolScript(create, destroy);
    std::string diff;
    for (size_t i = 0; i < std::min(oracle.size(), native.size()); ++i) {
        if (oracle[i] != native[i]) {
            diff += fmt::format(" #{}: oracle {:#x} native {:#x};", i, static_cast<u32>(oracle[i]),
                                static_cast<u32>(native[i]));
        }
    }
    c.Check(fmt::format("pool script ({} steps: name 31/32, flags, size, ids 3000.., EBADF, first free, ENFILE at 32)",
                        native.size()),
            oracle.size() == native.size() && diff.empty(), diff);
    c.Check("ids 3000 and 3001, ENFILE (0x80410117) for the 32nd pool",
            native.size() > 46 && native[5] == 3000 && native[6] == 3001 && native[15] == 3000 &&
                native[45] == 3030 && static_cast<u32>(native[46]) == 0x80410117u,
            native.size() > 46 ? fmt::format("{} {} {:#x}", native[5], native[6], static_cast<u32>(native[46])) : "");
}

struct NtopCase {
    s32 af;
    std::array<u8, 16> bytes;
    u32 size;
    const char* expected; // nullptr: fails
    s32 errno_expected;
};

void NetNtop(TestGroup& c) {
    const auto ntop = Export<NtopFn>("sceNetInetNtop");
    const auto err = Export<ErrnoFn>("sceNetErrnoLoc");
    if (ntop == nullptr || err == nullptr) {
        c.Check("exports sceNetInetNtop/sceNetErrnoLoc", false, "missing");
        return;
    }
    using B = std::array<u8, 16>;
    const NtopCase cases[] = {
        {2, B{255, 255, 255, 255}, 32, "255.255.255.255", 0}, // the game's own call (eboot 0x5fd253)
        {2, B{0, 0, 0, 0}, 32, "0.0.0.0", 0},
        {2, B{192, 168, 1, 20}, 16, "192.168.1.20", 0},
        {2, B{1, 2, 3, 4}, 8, "1.2.3.4", 0},
        {2, B{1, 2, 3, 4}, 7, nullptr, RNet::kEnospc},
        {2, B{1, 2, 3, 4}, 0, nullptr, RNet::kEnospc},
        {1, B{1, 2, 3, 4}, 32, nullptr, RNet::kEafnosupport},
        {30, B{}, 46, nullptr, RNet::kEafnosupport},
        {28, B{}, 46, "::", 0},
        {28, B{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, 46, "::1", 0},
        {28, B{0, 1}, 46, "1::", 0},
        {28, B{0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, 46, "fe80::1", 0},
        {28, B{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0xff, 0, 0, 0x42, 0x83, 0x29}, 46, "2001:db8::ff00:42:8329", 0},
        {28, B{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 1, 2, 3, 4}, 46, "::ffff:1.2.3.4", 0},
        {28, B{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4}, 46, "::1.2.3.4", 0},
        // (FreeBSD's/BIND's rule names the run of 7 zero words, but word 6 is inside the run then: "::2")
        {28, B{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2}, 46, "::2", 0},
        {28, B{0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1}, 46, "1:0:0:1::1", 0},
        {28, B{0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0}, 46, "1:0:1:0:1:0:1:0", 0},
        {28, B{0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0, 0, 0, 8}, 46, "1:2:3:4:5:6:0:8", 0},
        {28, B{0, 1, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 3}, 46, "1:0:0:2::3", 0}, // the longer run
        {28, B{0, 1, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 3, 0, 4}, 46, "1::2:0:0:3:4", 0}, // equal runs: the first
        {28, B{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, 40,
         "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", 0},
        {28, B{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, 39,
         nullptr, RNet::kEnospc},
        {28, B{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, 3, nullptr, RNet::kEnospc},
    };
    int same_as_oracle = 0;
    for (const auto& k : cases) {
        char native_buf[64];
        char oracle_buf[64];
        std::memset(native_buf, 0x5a, sizeof native_buf);
        std::memset(oracle_buf, 0x5a, sizeof oracle_buf);
        *err() = 0;
        *Libraries::Net::sceNetErrnoLoc() = 0;
        const char* n = ntop(k.af, k.bytes.data(), native_buf, k.size);
        const char* o = Libraries::Net::sceNetInetNtop(k.af, k.bytes.data(), oracle_buf, k.size);
        const s32 ne = *err();
        const s32 oe = *Libraries::Net::sceNetErrnoLoc();
        const std::string what =
            fmt::format("InetNtop af {} {} size {}", k.af, Hex(k.bytes.data(), k.af == 2 ? 4 : 16), k.size);
        const bool ok = k.expected != nullptr
                            ? n == native_buf && std::string_view(native_buf) == k.expected && ne == 0
                            : n == nullptr && ne == k.errno_expected &&
                                  static_cast<u8>(native_buf[0]) == 0x5a; // nothing written on failure
        c.Check(what + fmt::format(" -> {}", k.expected ? k.expected : "NULL"), ok,
                fmt::format("got {} errno {}", n ? native_buf : "NULL", ne));
        const bool same = (n == nullptr) == (o == nullptr) && ne == oe &&
                          (n == nullptr || std::string_view(native_buf) == std::string_view(oracle_buf));
        same_as_oracle += same ? 1 : 0;
        if (!same) {
            c.Info(fmt::format("INFO {}: the oracle (host inet_ntop) gives {} errno {}", what, o ? oracle_buf : "NULL",
                               oe));
        }
    }
    *err() = 0;
    const char* nul = ntop(2, nullptr, nullptr, 16);
    c.Check("InetNtop(null src, null dst): NULL, errno EFAULT", nul == nullptr && *err() == RNet::kEfault,
            fmt::format("{}", *err()));
    c.Info(fmt::format("INFO InetNtop: {} of {} cases equal to the oracle", same_as_oracle, std::size(cases)));
}

struct PtonCase {
    s32 af;
    const char* text;
    s32 ret;
    std::array<u8, 16> bytes;
};

void NetPton(TestGroup& c) {
    const auto pton = Export<PtonFn>("sceNetInetPton");
    const auto err = Export<ErrnoFn>("sceNetErrnoLoc");
    if (pton == nullptr || err == nullptr) {
        c.Check("export sceNetInetPton", false, "missing");
        return;
    }
    using B = std::array<u8, 16>;
    const PtonCase cases[] = {
        {2, "1.2.3.4", 1, B{1, 2, 3, 4}},
        {2, "255.255.255.255", 1, B{255, 255, 255, 255}},
        {2, "0.0.0.0", 1, B{}},
        {2, "256.1.1.1", 0, B{}},
        {2, "01.2.3.4", 0, B{}},
        {2, "1.2.3", 0, B{}},
        {2, "1.2.3.4.5", 0, B{}},
        {2, "1..2.3", 0, B{}},
        {2, "", 0, B{}},
        {2, " 1.2.3.4", 0, B{}},
        {2, "1.2.3.4 ", 0, B{}},
        {2, "0x1.2.3.4", 0, B{}},
        {28, "::", 1, B{}},
        {28, "::1", 1, B{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}},
        {28, "1::", 1, B{0, 1}},
        {28, "fe80::1", 1, B{0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}},
        {28, "FE80::A:b", 1, B{0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xa, 0, 0xb}},
        {28, "::ffff:1.2.3.4", 1, B{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 1, 2, 3, 4}},
        {28, "1:2:3:4:5:6:7:8", 1, B{0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0, 7, 0, 8}},
        {28, "1:2:3:4:5:6:1.2.3.4", 1, B{0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 1, 2, 3, 4}},
        {28, "1:2:3:4:5:6:7:8:9", 0, B{}},
        {28, "1::2::3", 0, B{}},
        {28, ":1::", 0, B{}},
        {28, "1:", 0, B{}},
        {28, "12345::", 0, B{}},
        {28, "g::", 0, B{}},
        {28, "1:2:3:4:5:6:7::8", 0, B{}},
        {28, "", 0, B{}},
    };
    int same_as_oracle = 0;
    for (const auto& k : cases) {
        u8 native_out[16];
        u8 oracle_out[16];
        std::memset(native_out, 0xa5, sizeof native_out);
        std::memset(oracle_out, 0xa5, sizeof oracle_out);
        *Libraries::Net::sceNetErrnoLoc() = 0;
        const s32 o = Libraries::Net::sceNetInetPton(k.af, k.text, oracle_out);
        const s32 oe = *Libraries::Net::sceNetErrnoLoc();
        *err() = 0;
        const s32 n = pton(k.af, k.text, native_out);
        const s32 ne = *err();
        const size_t len = k.af == 2 ? 4 : 16;
        const bool ok = n == k.ret && ne == 0 &&
                        (k.ret == 1 ? std::memcmp(native_out, k.bytes.data(), len) == 0
                                    : native_out[0] == 0xa5); // nothing written for malformed text
        const std::string what = fmt::format("InetPton af {} \"{}\" -> {}", k.af, k.text, k.ret);
        c.Check(what, ok, fmt::format("got {} errno {} {}", n, ne, Hex(native_out, len)));
        const bool same = o == n && oe == ne && (n != 1 || std::memcmp(native_out, oracle_out, len) == 0);
        same_as_oracle += same ? 1 : 0;
        if (!same) {
            c.Info(fmt::format("INFO {}: the oracle (host inet_pton) gives {} errno {}", what, o, oe));
        }
    }
    *err() = 0;
    u8 out[16];
    const s32 r1 = pton(2, nullptr, out);
    const s32 e1 = *err();
    *err() = 0;
    const s32 r2 = pton(5, "1.2.3.4", out);
    const s32 e2 = *err();
    const s32 o1 = Libraries::Net::sceNetInetPton(2, nullptr, out);
    const s32 o2 = Libraries::Net::sceNetInetPton(5, "1.2.3.4", out);
    c.Check("InetPton errors: null -> 0x8041010e errno EFAULT, af 5 -> 0x8041012f errno EAFNOSUPPORT, as the oracle",
            static_cast<u32>(r1) == 0x8041010eu && e1 == RNet::kEfault && static_cast<u32>(r2) == 0x8041012fu &&
                e2 == RNet::kEafnosupport && o1 == r1 && o2 == r2,
            fmt::format("{:#x}/{} {:#x}/{}", static_cast<u32>(r1), e1, static_cast<u32>(r2), e2));
    c.Info(fmt::format("INFO InetPton: {} of {} cases equal to the oracle", same_as_oracle, std::size(cases)));
}

void NetErrnoThreads(TestGroup& c) {
    const auto err = Export<ErrnoFn>("sceNetErrnoLoc");
    const auto ntop = Export<NtopFn>("sceNetInetNtop");
    if (err == nullptr || ntop == nullptr) {
        return;
    }
    s32* main_cell = err();
    *main_cell = 0;
    s32* thread_cell = nullptr;
    s32 thread_start = -1, thread_after = -1;
    std::thread([&] {
        thread_cell = err();
        thread_start = *thread_cell;
        u8 a[4] = {1, 2, 3, 4};
        char b[4];
        ntop(2, a, b, sizeof b); // ENOSPC on this thread only
        thread_after = *thread_cell;
    }).join();
    c.Check("sceNetErrnoLoc: a cell per thread, 0 at the thread's start, an error stays on its thread",
            main_cell != nullptr && thread_cell != nullptr && main_cell != thread_cell && thread_start == 0 &&
                thread_after == RNet::kEnospc && *main_cell == 0,
            fmt::format("{} {} {}", thread_start, thread_after, *main_cell));
}

void NetInitAndAdapters(TestGroup& c) {
    using Fn = s32(SSPC_SYSV_ABI*)();
    const auto init = Export<Fn>("sceNetInit");
    const auto term = Export<Fn>("sceNetTerm");
    const auto create = Export<PoolCreateFn>("sceNetPoolCreate");
    const auto destroy = Export<PoolDestroyFn>("sceNetPoolDestroy");
    const auto err = Export<ErrnoFn>("sceNetErrnoLoc");
    if (init == nullptr || term == nullptr || create == nullptr || destroy == nullptr || err == nullptr) {
        c.Check("exports sceNetInit/Term", false, "missing");
        return;
    }
    const s32 i = init();
    const s32 oi = Libraries::Net::sceNetInit();
    c.Check("sceNetInit = 0 (twice: the game counts its own references), as the oracle", i == 0 && init() == 0 && oi == 0,
            fmt::format("{:#x}", static_cast<u32>(i)));
    const s32 pool = create("sysrt-test", 16384, 0);
    const s32 rid = Libraries::Net::sceNetResolverCreate("r", pool, 0);
    c.Check("a SecondSonPC pool is known to the bridge's resolvers (mirrored, same id)", pool == 3000 && rid >= 0,
            fmt::format("pool {} resolver {:#x}", pool, static_cast<u32>(rid)));
    const s32 busy = destroy(pool);
    c.Check("sceNetPoolDestroy while a bridge resolver holds the pool: ENOTEMPTY (0x80410142), pool kept",
            static_cast<u32>(busy) == 0x80410142u, fmt::format("{:#x}", static_cast<u32>(busy)));
    Libraries::Net::sceNetResolverDestroy(rid);
    const s32 freed = destroy(pool);
    c.Check("after the resolver: sceNetPoolDestroy = 0", freed == 0, fmt::format("{:#x}", static_cast<u32>(freed)));
    *err() = 0;
    const s32 bad = Libraries::Net::sceNetResolverCreate("r", pool, 0);
    c.Check("the bridge's errno reaches sceNetErrnoLoc (bridge ResolverCreate on a freed pool: EBADF)",
            static_cast<u32>(bad) == 0x80410109u && *err() == RNet::kEbadf,
            fmt::format("{:#x} errno {}", static_cast<u32>(bad), *err()));
    c.Check("sceNetTerm = 0, as the oracle", term() == 0 && Libraries::Net::sceNetTerm() == 0, "");
}

// ------------------------------------------------------------------------------------------------ libc

/// A guest va_list built by hand: the register save area (6 integer + 8 vector registers) and the stack after it.
class GuestVa {
public:
    GuestVa(u32 gp_start = 0, u32 fp_start = 48) {
        l_.gp_offset = gp_start;
        l_.fp_offset = fp_start;
        next_gp_ = gp_start;
        next_fp_ = fp_start;
    }
    GuestVa& I(u64 v) {
        if (next_gp_ < 48) {
            std::memcpy(rsa_ + next_gp_, &v, 8);
            next_gp_ += 8;
        } else {
            stack_.push_back(v);
        }
        return *this;
    }
    GuestVa& P(const void* p) {
        return I(reinterpret_cast<u64>(p));
    }
    GuestVa& D(double v) {
        if (next_fp_ < 176) {
            std::memcpy(rsa_ + next_fp_, &v, 8);
            next_fp_ += 16;
        } else {
            u64 b;
            std::memcpy(&b, &v, 8);
            stack_.push_back(b);
        }
        return *this;
    }
    /// x87 80-bit long double (16 bytes on the stack, 16-byte aligned).
    GuestVa& LD(u64 mantissa, u16 sign_exponent) {
        if (stack_.size() % 2 != 0) {
            stack_.push_back(0);
        }
        stack_.push_back(mantissa);
        stack_.push_back(sign_exponent);
        return *this;
    }
    RLibc::SysvVaList* List() {
        stack_.push_back(0);
        l_.reg_save_area = rsa_;
        l_.overflow_arg_area = reinterpret_cast<u8*>(stack_.data());
        return &l_;
    }

private:
    alignas(16) u8 rsa_[176] = {};
    std::vector<u64, std::allocator<u64>> stack_;
    u32 next_gp_, next_fp_;
    RLibc::SysvVaList l_{};
};

void Fmt(TestGroup& c, const char* format, GuestVa& va, std::string_view expected, bool oracle_compare = false) {
    RLibc::SysvVaList* l = va.List();
    RLibc::SysvVaList copy = *l;
    std::string out;
    const s32 n = RLibc::FormatGuest(out, format, l);
    c.Check(fmt::format("printf \"{}\" -> \"{}\"", format, expected), out == expected && n == static_cast<s32>(out.size()),
            fmt::format("got \"{}\" ({})", out, n));
    if (oracle_compare) {
        char buf[512] = {};
        Libraries::LibcInternal::vsnprintf_ctx(buf, sizeof buf, format, reinterpret_cast<Common::VaList*>(&copy));
        if (out != buf) {
            c.Info(fmt::format("INFO printf \"{}\": the oracle's formatter (shadPS4 libc_internal/printf.h) gives \"{}\"",
                               format, buf));
        }
    }
}

void LibcFormat(TestGroup& c) {
    {
        GuestVa v;
        v.I(static_cast<u64>(-5)).I(7).I(0xffffffffu);
        Fmt(c, "%d|%i|%u", v, "-5|7|4294967295", true);
    }
    {
        GuestVa v;
        v.I(0x123456789ull).I(static_cast<u64>(-2)).I(0xdeadbeefcafeull).I(0xabcull);
        Fmt(c, "%ld|%lld|%llx|%llX", v, "4886718345|-2|deadbeefcafe|ABC", true);
    }
    {
        GuestVa v;
        v.I(0x1ff).I(0x18000).I(0x1ff).I(0x18000);
        Fmt(c, "%hhd|%hd|%hhu|%hu", v, "-1|-32768|255|32768");
    }
    {
        GuestVa v;
        v.I(42).I(42).I(42).I(42).I(42).I(7).I(0);
        Fmt(c, "%5d|%-5d|%05d|%+d|% d|%.3d|%.0d", v, "   42|42   |00042|+42| 42|007|", true);
    }
    {
        GuestVa v;
        v.I(255).I(255).I(8).I(8).I(0).I(0);
        Fmt(c, "%x|%#x|%#o|%o|%#x|%#o", v, "ff|0xff|010|10|0|0");
    }
    {
        GuestVa v;
        v.P("abc").P("abc").P("ab").P("ab").P(nullptr);
        Fmt(c, "%s|%.2s|%5s|%-5s|%s", v, "abc|ab|   ab|ab   |(null)");
    }
    {
        GuestVa v;
        v.I('O').I('K').P(reinterpret_cast<void*>(0x1234)).P(nullptr).P(reinterpret_cast<void*>(0x7ffff0001000ull));
        Fmt(c, "%c%c|%p|%p|%p", v, "OK|0x1234|0x0|0x7ffff0001000");
    }
    {
        GuestVa v;
        v.I(4).I(1).I(4).I(2).I(2).P("xyz").I(static_cast<u64>(-3)).I(5);
        Fmt(c, "%*d|%-*d|%.*s|%*d", v, "   1|2   |xy|5  ");
    }
    {
        s32 count = -1;
        GuestVa v;
        v.P("ab").P(&count);
        Fmt(c, "%%|%s%n", v, "%|ab");
        c.Check("%n stores the count so far (4)", count == 4, fmt::format("{}", count));
    }
    {
        GuestVa v;
        v.D(1.5).D(3.14159).D(12345.678).D(0.0001).D(1e20);
        Fmt(c, "%f|%.2f|%e|%g|%G", v, "1.500000|3.14|1.234568e+04|0.0001|1E+20");
    }
    {
        GuestVa v;
        v.D(INFINITY).D(-INFINITY).D(NAN).D(INFINITY).D(-1.5);
        Fmt(c, "%f|%F|%e|%+5f|%08.3f", v, "inf|-INF|nan| +inf|-001.500");
    }
    {
        GuestVa v;
        v.D(1.0).D(0.5).D(-0.0).D(255.0 / 256.0);
        Fmt(c, "%a|%A|%a|%a", v, "0x1p+0|0X1P-1|-0x0p+0|0x1.fep-1");
    }
    {
        GuestVa v;
        v.LD(0xA000000000000000ull, 0x4000); // 2.5
        Fmt(c, "%Lf", v, "2.500000");
    }
    {
        GuestVa v; // 7 integers: 6 in registers, 1 on the stack; 9 doubles: 8 in registers, 1 on the stack
        for (int i = 1; i <= 7; ++i) {
            v.I(static_cast<u64>(i));
        }
        for (int i = 1; i <= 9; ++i) {
            v.D(i + 0.5);
        }
        Fmt(c, "%d %d %d %d %d %d %d|%.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f", v,
            "1 2 3 4 5 6 7|1.5 2.5 3.5 4.5 5.5 6.5 7.5 8.5 9.5");
    }
    {
        GuestVa v(40, 160); // the caller's named arguments took 5 integer and 7 vector registers
        v.I(1).I(2).D(0.25).D(0.75);
        Fmt(c, "%d %d %.2f %.2f", v, "1 2 0.25 0.75");
    }
    {
        GuestVa v; // libSceFios2's own format (its print function, 0x38650)
        v.P("[I]").P("info").I(0x1234).P("").P("").P(reinterpret_cast<void*>(0xdead0)).I(3).P("/app0/x.psarc");
        Fmt(c, "%s fios2/%-5s 0x%08llx %s%s pDH: %p, h: %d, path: %s\n", v,
            "[I] fios2/info  0x00001234  pDH: 0xdead0, h: 3, path: /app0/x.psarc\n"); // (two empty %s: two spaces)
    }
    {
        GuestVa v;
        v.I(1);
        Fmt(c, "a%yb|%1$d|%", v, "a%yb|%1$d|%");
    }
}

void LibcStdout(TestGroup& c) {
    using Vprintf = s32(SSPC_SYSV_ABI*)(const char*, RLibc::SysvVaList*);
    using Fflush = s32(SSPC_SYSV_ABI*)(void*);
    const auto vprintf = Export<Vprintf>("vprintf");
    const auto fflush = Export<Fflush>("fflush");
    void* const stdout_object = Export<void*>("_Stdout");
    if (vprintf == nullptr || fflush == nullptr || stdout_object == nullptr) {
        c.Check("exports vprintf/fflush/_Stdout", false, "missing");
        return;
    }
    c.Check("_Stdout is SecondSonPC's FILE object", stdout_object == RLibc::StdoutObject(), "");
    GuestVa v;
    v.I(1);
    RLibc::SysvVaList* l = v.List();
    const RLibc::SysvVaList before = *l;
    const s32 n = vprintf("sysrt self-test %d", l);
    c.Check("vprintf returns the characters written (17), keeps text without a line end, leaves the caller's va_list",
            n == 17 && RLibc::PendingStdout() == "sysrt self-test 1" && std::memcmp(&before, l, sizeof before) == 0,
            fmt::format("{} \"{}\"", n, RLibc::PendingStdout()));
    c.Check("fflush(&_Stdout) = 0 writes it out", fflush(stdout_object) == 0 && RLibc::PendingStdout().empty(), "");
    GuestVa v2;
    v2.P("x");
    vprintf("line %s\nrest", v2.List());
    c.Check("a completed line is written at once, the rest waits", RLibc::PendingStdout() == "rest",
            RLibc::PendingStdout());
    c.Check("fflush(NULL) = 0 writes every stream", fflush(nullptr) == 0 && RLibc::PendingStdout().empty(), "");
    c.Check("vprintf(NULL format) = EOF", vprintf(nullptr, v.List()) == -1, "");
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([t, vprintf] {
            for (int i = 0; i < 50; ++i) {
                GuestVa a;
                a.I(static_cast<u64>(t)).I(static_cast<u64>(i));
                vprintf("sysrt thread %d line %d\n", a.List());
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    c.Check("4 threads x 50 lines: nothing left over", RLibc::PendingStdout().empty(), RLibc::PendingStdout());
    // a FILE of the library not made here (zeroed: no pending output), as the oracle's internal_fflush sees it
    alignas(16) u8 foreign_native[0x200] = {};
    auto* foreign_oracle = new Libraries::LibcInternal::OrbisFILE();
    const s32 fn = fflush(foreign_native);
    const s32 fo = Libraries::LibcInternal::internal_fflush(foreign_oracle);
    delete foreign_oracle;
    c.Check("fflush of a FILE with nothing pending = 0, as the oracle", fn == 0 && fo == 0,
            fmt::format("{} {}", fn, fo));
    const u16 pending = 0x2000;
    std::memcpy(foreign_native, &pending, 2);
    c.Check("fflush of a foreign FILE with pending output: refused (EOF), the stream untouched",
            fflush(foreign_native) == -1 && foreign_native[1] == 0x20 && foreign_native[8] == 0, "");
}

void LibcHeapTrace(TestGroup& c) {
    using Fn = void(SSPC_SYSV_ABI*)(RLibc::HeapTraceInfo*);
    const auto get = Export<Fn>("sceLibcHeapGetTraceInfo");
    if (get == nullptr) {
        c.Check("export sceLibcHeapGetTraceInfo", false, "missing");
        return;
    }
    // as libc.prx prepares it (0x5bd68: size 0x20, flag 1, the rest 0); a non-zero get_segment_info must be cleared
    RLibc::HeapTraceInfo n{0x20, 1, 0xAAAA, nullptr, nullptr};
    RLibc::HeapTraceInfo o = n;
    get(&n);
    Libraries::LibcInternal::sceLibcHeapGetTraceInfo(reinterpret_cast<Libraries::LibcInternal::HeapInfoInfo*>(&o));
    c.Check("fields as the oracle's: size and flag kept, get_segment_info 0 (tracing off), mask and table given",
            n.size == o.size && n.flag == o.flag && n.get_segment_info == 0 && o.get_segment_info == 0 &&
                n.mspace_atomic_id_mask != nullptr && n.mstate_table != nullptr && o.mspace_atomic_id_mask != nullptr &&
                o.mstate_table != nullptr,
            fmt::format("{:#x} {} {} {}", n.size, n.flag, n.get_segment_info, o.get_segment_info));
    bool zero = *n.mspace_atomic_id_mask == 0;
    for (size_t i = 0; i < RLibc::kMstateTableEntries; ++i) {
        zero &= n.mstate_table[i] == 0;
    }
    c.Check("mask 0, all 128 table entries 0 (the oracle has 64: index 64 = id 63 + 1 would be past it)", zero, "");
    RLibc::HeapTraceInfo again{0x20, 1, 0, nullptr, nullptr};
    get(&again);
    c.Check("the same mask and table on every call (one owner of the mspace ids)",
            again.mspace_atomic_id_mask == n.mspace_atomic_id_mask && again.mstate_table == n.mstate_table, "");
    get(nullptr); // no structure: nothing to fill (the oracle would fault)
    c.Check("null structure: returns", true, "");
}

} // namespace

std::vector<TestGroup> TestSysrt(const App::Config& cfg) {
    std::vector<TestGroup> out(3);
    out[0].name = "SysrtDialog";
    out[1].name = "SysrtNet";
    out[2].name = "SysrtLibc";
    // before the adapters: the two pool tables and errnos are independent
    DialogBefore(out[0]);
    NetPools(out[1]);
    NetNtop(out[1]);
    NetPton(out[1]);
    NetErrnoThreads(out[1]);
    App::Config with = cfg;
    with.native_groups = {"sysrt-dialog", "sysrt-net", "sysrt-libc"};
    InstallSysrtHost(with);
    DialogLifecycle(out[0]);
    NetInitAndAdapters(out[1]);
    LibcFormat(out[2]);
    LibcStdout(out[2]);
    LibcHeapTrace(out[2]);
    // (the adapters stay installed: the rest of the self-test does not use libSceNet or the dialogs)
    return out;
}

} // namespace SecondSon::Bridge
