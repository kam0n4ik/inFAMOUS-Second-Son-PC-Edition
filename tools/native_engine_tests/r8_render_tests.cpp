// SecondSonPC — the tests of R8-A3 Native Render (the Native Render Layer's working path), owned by that branch
// (docs/NATIVE_ENGINE_R8_CORE.md §3). main.cpp calls R8RenderTests with the eboot mapped (before the CUE setters' jumps
// go into the image); it returns the number of failures and adds its checks to the total through `checks`.
//
// 1. NR4 - one dispatch for the CUE commit 0x14ad0: an entry probe with a slot for the original (as native_draw.cpp's
//    commit probe, R7 C3) and the Native Engine's cue_commit installed below it (recomp_14ad0). The probe decides per
//    call: "native" (the layer's commit - nothing below runs) or hand the call on through the slot (the registry's
//    dispatcher, in every mode). Checked through the eboot's entry: the handed-on calls commit exactly once (two
//    snapshots each, the dirty flags cleared), the native ones never reach the dispatcher or the game's code.
//    (main.cpp's R5-D test left cue_commit's jump on 0x14ad0: the function's pristine bytes are read from the eboot
//    file for the test and the jump is put back after it.)
// 2. NR2 - the layer's native commit (NativeDraw::NativeCommit) reported to the Native Resource State through
//    R7::ResourceHooks: random sequences as R7-R's test (r7_resources_tests.cpp) where each commit is either the game's
//    (recomp_14ad0 through Resources::Commit) or the native one. After each commit every present binding is checked
//    against the bytes and the user data the commit left (the recorded calls / the native register writes): snapshot =
//    shadow, the shape's pointers and the direct values, equal versions = equal content across both kinds of commit;
//    verify mode's counters 0. A version kept from a game commit to a native one (and back) must happen.
// 3. NR5 - the paths' switches: without SSPC_R8_RENDER_* the layer carries nothing new.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <windows.h>

#include "base/abi.h"
#include "engine/guest_probe.h"
#include "engine/render/native_draw.h"
#include "engine/render/native_render_layer.h"
#include "loader/image_file.h"
#include "native_engine/native_function_registry.h"
#include "native_engine/r7/resources/resource_state.h"
#include "native_engine/reconstructed/cue_commit.h"
#include "native_engine/reconstructed/cue_set_buffers.h"

namespace SecondSon::NativeEngine::Tests {

namespace {

namespace CSB = Reconstructed::CueSetBuffers;
namespace CC = Reconstructed::CueCommit;
namespace Res = R7::Resources;
namespace NRL = Engine::Render::R7;
namespace ND = Engine::Render::NativeDraw;
using Engine::Render::kMaxRegWrites;
using Engine::Render::RegWrite;

int g_fail = 0;
int* g_checks = nullptr;

#define R8R_CHECK(cond)                                                                                                \
    do {                                                                                                               \
        ++*g_checks;                                                                                                   \
        if (!(cond)) {                                                                                                 \
            ++g_fail;                                                                                                  \
            std::printf("  FAILED: %s (%s:%d)\n", #cond, __FILE__, __LINE__);                                          \
        }                                                                                                              \
    } while (0)

/// A 14-byte absolute jump at image + rva to `fn`, the bytes put back on destruction.
struct Patch {
    u8* at;
    u8 saved[14];
    Patch(u64 base, u64 rva, const void* fn) : at(reinterpret_cast<u8*>(base + rva)) {
        DWORD old = 0;
        VirtualProtect(at, 14, PAGE_EXECUTE_READWRITE, &old);
        std::memcpy(saved, at, 14);
        const u8 jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
        std::memcpy(at, jmp, 6);
        const u64 target = reinterpret_cast<u64>(fn);
        std::memcpy(at + 6, &target, 8);
        FlushInstructionCache(GetCurrentProcess(), at, 14);
    }
    ~Patch() {
        std::memcpy(at, saved, 14);
        FlushInstructionCache(GetCurrentProcess(), at, 14);
    }
};

// --- the commit's callees, recorded: the user data a GPU would hold, the snapshot arena ------------------------------

u32 g_ud[8][16] = {};
u32 g_ud_mask[8] = {};
u8* g_arena = nullptr;
u64 g_arena_used = 0, g_arena_size = 0;
u64 g_allocs = 0, g_stack_fail = 0;

SSPC_SYSV_ABI u64 RecSetPointer(u64, u64 stage, u64 sgpr, u64 pointer, u64, u64) {
    const u32 st = static_cast<u32>(stage), sg = static_cast<u32>(sgpr);
    if (st < 8 && sg + 1 < 16) {
        g_ud[st][sg] = static_cast<u32>(pointer);
        g_ud[st][sg + 1] = static_cast<u32>(pointer >> 32);
        g_ud_mask[st] |= 3u << sg;
    }
    return 0;
}

SSPC_SYSV_ABI u64 RecSetRegion(u64, u64 stage, u64 sgpr, u64 values, u64 count, u64) {
    const u32 st = static_cast<u32>(stage), sg = static_cast<u32>(sgpr);
    for (u32 j = 0; st < 8 && j < static_cast<u32>(count) && sg + j < 16; ++j) {
        std::memcpy(&g_ud[st][sg + j], reinterpret_cast<const void*>(values + 4ull * j), 4);
        g_ud_mask[st] |= 1u << (sg + j);
    }
    return 0;
}

SSPC_SYSV_ABI u64 RecAlloc(u64 cue, u64 dwords, u64, u64, u64, u64) {
    ++g_allocs;
    const u64 bytes = u64{static_cast<u32>(dwords)} * 4;
    if (g_arena_used + bytes > g_arena_size) {
        return 0;
    }
    if (cue != 0) { // as 0x123d0: the CUE's allocator cursor (+0x20) moves by the snapshot
        u64 cursor;
        std::memcpy(&cursor, reinterpret_cast<const void*>(cue + 0x20), 8);
        cursor += bytes;
        std::memcpy(reinterpret_cast<void*>(cue + 0x20), &cursor, 8);
    }
    const u64 p = reinterpret_cast<u64>(g_arena + g_arena_used);
    g_arena_used += bytes;
    return p;
}

/// The native commit's allocator (NativeDraw::AllocFn): the same arena.
SSPC_SYSV_ABI u64 NativeAlloc(u64 cue, u32 dwords) {
    return RecAlloc(cue, dwords, 0, 0, 0, 0);
}

SSPC_SYSV_ABI void NoPsInputs(u32*, u64, u32, u64, s32) {}

SSPC_SYSV_ABI u64 RecMemcpy(u64 dst, u64 src, u64 n, u64, u64, u64) {
    std::memcpy(reinterpret_cast<void*>(dst), reinterpret_cast<const void*>(src), n);
    return dst;
}

SSPC_SYSV_ABI u64 RecStackFail(u64, u64, u64, u64, u64, u64) {
    ++g_stack_fail;
    return 0;
}

SSPC_SYSV_ABI u64 RecNothing(u64, u64, u64, u64, u64, u64) {
    return 0;
}

/// The commit's callees replaced in the mapped image (0x14ad0's 18; the builders run for real), the stack guard's GOT
/// slot pointed at a guard; all undone on destruction.
struct CommitCallees {
    std::vector<std::unique_ptr<Patch>> patches;
    u64* got;
    u64 got_saved;
    explicit CommitCallees(u64 image_base) : got(reinterpret_cast<u64*>(image_base + 0xd9de00)) {
        static u64 guard = 0x5ec0de5ec0de5ec0ull;
        got_saved = *got;
        *got = reinterpret_cast<u64>(&guard);
        const auto patch = [&](u64 rva, const void* fn) {
            patches.push_back(std::make_unique<Patch>(image_base, rva, fn));
        };
        patch(0x123d0, reinterpret_cast<const void*>(&RecAlloc));
        patch(0x9843a0, reinterpret_cast<const void*>(&RecMemcpy));
        patch(0x984360, reinterpret_cast<const void*>(&RecStackFail));
        patch(0x626400, reinterpret_cast<const void*>(&RecSetPointer));
        patch(0x626520, reinterpret_cast<const void*>(&RecSetRegion));
        for (const u64 rva : {0x623630ull, 0x624cb0ull, 0x625100ull, 0x6251c0ull, 0x625220ull, 0x625300ull,
                              0x625370ull, 0x6253e0ull, 0x625510ull, 0x6256a0ull, 0x625a40ull, 0x625aa0ull,
                              0x638540ull}) {
            patch(rva, reinterpret_cast<const void*>(&RecNothing));
        }
    }
    ~CommitCallees() {
        patches.clear();
        *got = got_saved;
    }
};

// --- a CUE as the commit reads it (r7_resources_tests.cpp's world) ----------------------------------------------------

constexpr u64 kCueBytes = 0x20000, kLayouts = 0x20000, kShader = 0x21000, kWorldBytes = 0x22000;
constexpr u32 kTableOff[CSB::kAllKinds] = {0x10a, 0x22, 0xe2, 0x22, 0xc2, 0xa2, 0xa2};
constexpr u32 kSlots[CSB::kAllKinds] = {16, 64, 20, 64, 16, 16, 16};

/// VS / PS x {pointers and single values, direct descriptors}.
void MakeLayout(u8* t, std::mt19937_64& rng, bool vertex, bool direct) {
    std::memset(t, 0xff, 0x138);
    const u16 dwords = static_cast<u16>(64 + rng() % (0x600 - 64));
    std::memcpy(t, &dwords, 2);
    const auto off = [&](u32 field) {
        const u16 v = static_cast<u16>(rng() % dwords);
        std::memcpy(t + field, &v, 2);
    };
    t[7] = 0;
    if (!direct) {
        t[8] = 2, off(0x16);
        t[0xA] = 4, off(0x1C);
        t[0xC] = 6;
        t[0xD] = 8;
        t[0xE] = 9;
        if (vertex) {
            t[4] = 10;
            t[5] = 12, off(0x18);
            t[0xF] = 14;
        }
    }
    for (u32 k = 0; k < CSB::kAllKinds; ++k) {
        for (u32 s = 0; s < kSlots[k]; ++s) {
            const u32 pick = static_cast<u32>(rng() % 6);
            u16 w;
            if (pick == 0) {
                w = 0xffff;
            } else if (pick == 1 && direct) {
                w = static_cast<u16>(0x8000 | (CSB::kDescriptorBytes[k] == 32 ? 8 : 4));
            } else {
                w = static_cast<u16>(rng() % (dwords - 8));
            }
            std::memcpy(t + kTableOff[k] + 2 * s, &w, 2);
        }
    }
}

struct World {
    std::vector<u8> mem = std::vector<u8>(kWorldBytes);
    u8* lay[2][2] = {}; ///< [vs?][direct?]
    u8* empty = nullptr;
    u8* cue_bytes() {
        return mem.data();
    }
    u64 cue() {
        return reinterpret_cast<u64>(mem.data());
    }
    void put64(u64 off, u64 v) {
        std::memcpy(mem.data() + off, &v, 8);
    }
    u64 get64(u64 off) const {
        u64 v;
        std::memcpy(&v, mem.data() + off, 8);
        return v;
    }
    /// A fresh CUE: shadows random, four layouts, the PS / VS layouts set and dirty, the rest as CUE::Init leaves it.
    void Generate(std::mt19937_64& rng) {
        for (size_t i = 0; i < kCueBytes; i += 8) {
            put64(i, rng());
        }
        std::memset(mem.data() + 0xc090, 0, 0xc838 - 0xc090);
        put64(0xc090, 0x6cb0000000ull);
        for (u32 i = 0; i < 6; ++i) {
            put64(0xc0a8 + 8 * i, reinterpret_cast<u64>(mem.data() + kShader));
        }
        put64(0x78, rng());
        put64(0xc130, rng());
        for (u32 i = 0; i < 0x40; i += 4) {
            const u32 v = static_cast<u32>(rng());
            std::memcpy(mem.data() + 0xc0e0 + i, &v, 4);
        }
        mem[0x88] = 0;
        u8* layouts = mem.data() + kLayouts;
        lay[0][0] = layouts, lay[0][1] = layouts + 0x140, lay[1][0] = layouts + 0x280, lay[1][1] = layouts + 0x3c0;
        empty = layouts + 0x500;
        std::memset(empty, 0xff, 0x138);
        std::memset(empty, 0, 4);
        for (u32 v = 0; v < 2; ++v) {
            for (u32 d = 0; d < 2; ++d) {
                MakeLayout(lay[v][d], rng, v == 1, d == 1);
            }
        }
        for (u32 s = 0; s < 8; ++s) {
            put64(0xc180 + 8 * s, reinterpret_cast<u64>(empty));
        }
        SetShader(1, rng() % 2 != 0);
        SetShader(2, rng() % 2 != 0);
    }
    /// As 0x165b0 / 0x16690 leave it: the stage's layout, its table dirty.
    void SetShader(u32 st, bool direct) {
        put64(0xc180 + 8 * st, reinterpret_cast<u64>(lay[st == 2][direct ? 1 : 0]));
        mem[0xc1d0 + st] = 1;
    }
};

// --- 1. NR4 -----------------------------------------------------------------------------------------------------------

constexpr Engine::ProbeSite kCommitSite{{0x14ad0, 0x1184, 0x8bb6e7985e565584ull}, 20}; ///< native_draw.cpp's
using CommitFn = SSPC_SYSV_ABI void (*)(u64 cue);
CommitFn g_probe_original = nullptr; ///< the probe's slot: the dispatcher once cue_commit is below it
bool g_in_scope = false;             ///< the layer's decision for the next call (R7Commit: InScope and the checks)
u64 g_probe_native = 0, g_probe_handed = 0;

/// native_draw.cpp CommitProbe's dispatch: the native commit (here: nothing - the decision is what is tested) or the
/// slot, never both.
SSPC_SYSV_ABI void TestCommitProbe(u64 cue) {
    if (g_in_scope) {
        ++g_probe_native;
        return;
    }
    ++g_probe_handed;
    g_probe_original(cue);
}

/// The eboot file named by --eboot on the command line (main.cpp's argument).
std::string EbootPath() {
    for (int i = 1; i + 1 < __argc; ++i) {
        if (std::strcmp(__argv[i], "--eboot") == 0) {
            return __argv[i + 1];
        }
    }
    return {};
}

/// The bytes of [rva, rva + size) as the eboot file holds them.
bool FileBytes(const std::string& path, u64 rva, u64 size, std::vector<u8>& out) {
    Loader::ImageFile file;
    std::string error;
    if (path.empty() || !file.Open(path, error)) {
        return false;
    }
    for (const auto& p : file.ProgramHeaders()) {
        if (p.type == Loader::Elf::PT_LOAD && rva >= p.vaddr && rva + size <= p.vaddr + p.filesz) {
            const std::vector<u8> seg = file.ReadSegment(p);
            if (seg.size() < rva - p.vaddr + size) {
                return false;
            }
            out.assign(seg.begin() + static_cast<std::ptrdiff_t>(rva - p.vaddr),
                       seg.begin() + static_cast<std::ptrdiff_t>(rva - p.vaddr + size));
            return true;
        }
    }
    return false;
}

void TestCommitDispatch(u64 image_base, void* module, u64 cases) {
    std::printf("R8 NR4: the CUE commit 0x14ad0 - the layer's entry probe (with a slot) and cue_commit below it:\n");
    const auto impl = module ? reinterpret_cast<CC::Fn>(GetProcAddress(static_cast<HMODULE>(module), "recomp_14ad0"))
                             : nullptr;
    const auto redirect = module ? reinterpret_cast<void (*)(u64, u64)>(
                                       GetProcAddress(static_cast<HMODULE>(module), "recomp_set_redirect"))
                                 : nullptr;
    std::vector<u8> pristine;
    if (impl == nullptr || redirect == nullptr ||
        !FileBytes(EbootPath(), kCommitSite.function.rva, kCommitSite.function.size, pristine)) {
        std::printf("  no recomp_14ad0 / recomp_set_redirect, or the eboot file unreadable: skipped\n");
        return;
    }
    const FunctionSpec& spec = Reconstructed::kCueCommitSpec;
    Slot& slot = *spec.slot;
    // the function as the loader leaves it (main.cpp's R5-D test jumped it to the dispatcher); put back after the test
    u8* const fn_at = reinterpret_cast<u8*>(image_base + kCommitSite.function.rva);
    std::vector<u8> installed(fn_at, fn_at + kCommitSite.function.size);
    DWORD old = 0;
    VirtualProtect(fn_at, kCommitSite.function.size, PAGE_EXECUTE_READWRITE, &old);
    std::memcpy(fn_at, pristine.data(), pristine.size());
    FlushInstructionCache(GetCurrentProcess(), fn_at, kCommitSite.function.size);
    void* const saved_original = slot.original;
    const u8 saved_mode = slot.mode.load();
    const bool saved_below = slot.below_probe;
    const CC::Fn saved_impl = CC::Implementation();

    std::string error;
    void* original = Engine::InstallEntryProbe(image_base, kCommitSite, reinterpret_cast<const void*>(&TestCommitProbe),
                                               error, reinterpret_cast<void**>(&g_probe_original));
    R8R_CHECK(original != nullptr);
    if (original == nullptr) {
        std::printf("  entry probe: %s\n", error.c_str());
    } else {
        g_probe_original = reinterpret_cast<CommitFn>(original);
        CC::UseImplementation(impl, "recompiled (test)", redirect);
        error.clear();
        const bool below = InstallFunction(image_base, spec, Mode::Native, error);
        std::printf("  cue_commit installed below the probe: %s%s\n", below ? "yes" : "NO: ", error.c_str());
        R8R_CHECK(below && InstalledBelowProbe(spec) &&
                  reinterpret_cast<const void*>(g_probe_original) == spec.entry);
        if (below) {
            const CommitCallees callees(image_base);
            std::vector<u8> arena(0x200000);
            g_arena = arena.data();
            g_arena_size = arena.size();
            World w;
            std::mt19937_64 rng{0x14ad0};
            const auto entry = reinterpret_cast<CommitFn>(image_base + kCommitSite.function.rva);
            // (the slot's counters run on from main.cpp's cue_commit test, its deliberate wrong implementation included)
            const u64 verified0 = slot.verified.load(), mismatches0 = slot.mismatches.load();
            for (const Mode mode : {Mode::Native, Mode::Verify, Mode::Guarded, Mode::Bench, Mode::Original}) {
                slot.mode.store(static_cast<u8>(mode));
                slot.sample_period = 3;
                u64 bad_native = 0, bad_handed = 0, native = 0, handed = 0;
                const u64 n0 = g_probe_native, h0 = g_probe_handed;
                for (u64 n = 0; n < cases; ++n) {
                    w.Generate(rng);
                    g_arena_used = 0;
                    g_in_scope = rng() % 3 == 0;
                    const std::vector<u8> before = w.mem;
                    const u64 allocs = g_allocs;
                    entry(w.cue());
                    if (g_in_scope) {
                        ++native;
                        bad_native += g_allocs == allocs && w.mem == before ? 0 : 1; // nothing below ran
                    } else {
                        ++handed;
                        // exactly one commit: the two dirty stages' snapshots, their flags cleared
                        bad_handed += g_allocs == allocs + 2 && w.mem[0xc1d1] == 0 && w.mem[0xc1d2] == 0 ? 0 : 1;
                    }
                }
                const bool counts = g_probe_native - n0 == native && g_probe_handed - h0 == handed;
                std::printf("  %-8s: %llu calls through the eboot's entry - native %llu (something below ran: %llu), "
                            "handed to cue_commit %llu (not exactly one commit: %llu)\n",
                            ModeName(mode), static_cast<unsigned long long>(cases),
                            static_cast<unsigned long long>(native), static_cast<unsigned long long>(bad_native),
                            static_cast<unsigned long long>(handed), static_cast<unsigned long long>(bad_handed));
                R8R_CHECK(counts && bad_native == 0 && bad_handed == 0 && native > 0 && handed > 0);
            }
            std::printf("  cue_commit: verified %llu, mismatches %llu, stack failures %llu\n",
                        static_cast<unsigned long long>(slot.verified.load() - verified0),
                        static_cast<unsigned long long>(slot.mismatches.load() - mismatches0),
                        static_cast<unsigned long long>(g_stack_fail));
            R8R_CHECK(slot.verified.load() > verified0 && slot.mismatches.load() == mismatches0 && g_stack_fail == 0);
        }
    }
    // the image and cue_commit as they were (the probe's record stays: nothing installs on 0x14ad0 after this)
    std::memcpy(fn_at, installed.data(), installed.size());
    FlushInstructionCache(GetCurrentProcess(), fn_at, kCommitSite.function.size);
    slot.original = saved_original;
    slot.mode.store(saved_mode);
    slot.below_probe = saved_below;
    CC::UseImplementation(saved_impl, "recompiled (test)", redirect);
    std::string why;
    CC::Prepare(image_base, why); // (its recording clone again from the module: the probe's saved bytes)
}

// --- 2. NR2 -----------------------------------------------------------------------------------------------------------

struct Expect {
    std::vector<u8> bytes;
    u32 direct_mask;
    u32 direct[16];
    u64 layout;
};

/// The native commit's user-data writes into the GPU model (SPI_SHADER_USER_DATA_PS_0 0x2C0C, _VS_0 0x2C4C).
void ApplyUserData(const RegWrite* writes, u32 n) {
    for (u32 i = 0; i < n; ++i) {
        const u32 r = writes[i].reg;
        for (const auto [base, st] : {std::pair<u32, u32>{0x2C0C, 1}, std::pair<u32, u32>{0x2C4C, 2}}) {
            if (r >= base && r < base + 16) {
                g_ud[st][r - base] = writes[i].value;
                g_ud_mask[st] |= 1u << (r - base);
            }
        }
    }
}

void TestNativeCommitReported(u64 image_base, void* module, u64 cases) {
    std::printf("R8 NR2: the layer's native commit told to the Native Resource State (R7::ResourceHooks), mixed with "
                "the game's commit:\n");
    const auto commit = module ? reinterpret_cast<CC::Fn>(GetProcAddress(static_cast<HMODULE>(module), "recomp_14ad0"))
                               : nullptr;
    const auto redirect = module ? reinterpret_cast<void (*)(u64, u64)>(
                                       GetProcAddress(static_cast<HMODULE>(module), "recomp_set_redirect"))
                                 : nullptr;
    if (commit == nullptr || redirect == nullptr) {
        std::printf("  no recomp_14ad0 / recomp_set_redirect in the module: skipped\n");
        return;
    }
    std::string error;
    for (u32 k = 0; k < CSB::kAllKinds; ++k) {
        const auto kind = static_cast<CSB::Kind>(k);
        R8R_CHECK(CSB::RecordingOriginal(kind) != nullptr || CSB::Prepare(kind, image_base, error));
    }
    const CommitCallees callees(image_base);
    R8R_CHECK(NRL::GetResourceHooks() == nullptr); // (R7-R's test deactivated the state)
    Res::SetMode(Res::StateMode::Verify);
    Res::AssumeTracking(true);
    const bool prepared = Res::Prepare(image_base, redirect, error);
    const NRL::ResourceHooks* hooks = NRL::GetResourceHooks();
    std::printf("  prepare: %s%s; hooks %s\n", prepared ? "done" : "FAILED: ", error.c_str(),
                hooks != nullptr ? "registered" : "MISSING");
    R8R_CHECK(prepared && hooks != nullptr && hooks->fill != nullptr);
    if (!prepared || hooks == nullptr) {
        return;
    }
    ND::CommitEnv env;
    env.alloc = &NativeAlloc;
    env.ps_inputs = &NoPsInputs;
    env.hooks = hooks;
    std::vector<u8> arena(0x400000);
    g_arena = arena.data();
    g_arena_size = arena.size();
    World w;
    u8* const cue_bytes = w.cue_bytes();
    const u64 cue = w.cue();
    const auto reset_fn = reinterpret_cast<SSPC_SYSV_ABI void (*)(u64)>(image_base + 0x13d20); // through its probe
    std::mt19937_64 rng{0x8a3};
    const Res::Stats t0 = Res::Totals();
    u64 game_commits = 0, native_commits = 0, refused = 0, present = 0, version_seen = 0, kept_across = 0;
    u64 bad_bytes = 0, bad_version = 0, bad_user = 0, bad_present = 0, bad_version0 = 0;
    u64 bad_program = 0, bad_epoch = 0, epoch_resets = 0;
    std::string first;
    const auto fail = [&](u64& counter, const std::string& what) {
        if (counter++ == 0 && first.empty()) {
            first = what;
        }
    };
    std::vector<u8> src(16 * 32);
    for (u64 n = 0; n < cases; ++n) {
        w.Generate(rng);
        Res::Forget(cue);
        std::memset(g_ud, 0, sizeof(g_ud));
        std::memset(g_ud_mask, 0, sizeof(g_ud_mask));
        g_arena_used = 0;
        bool committed[8] = {};
        bool epoch_known = false, epoch_prev_known = false;
        u64 epoch_now = 0, epoch_prev = 0;
        std::map<u64, std::pair<Expect, bool>> seen[8]; ///< version -> content, made by a native commit
        for (u32 op = 0; op < 64; ++op) {
            const u32 pick = static_cast<u32>(rng() % 100);
            if (pick < 60) { // a setter: fresh descriptors, or the shadow's own (a redundant call)
                const u32 kind = static_cast<u32>(rng() % CSB::kAllKinds);
                const u32 st = 1 + static_cast<u32>(rng() % 2);
                const u32 count = 1 + static_cast<u32>(rng() % 4);
                const u32 slot = static_cast<u32>(rng() % (kSlots[kind] - count + 1));
                const u32 bytes = CSB::kDescriptorBytes[kind];
                const u64 table = w.get64(0xc180 + 8 * st);
                const bool same = rng() % 2 == 0;
                for (u32 i = 0; i < count; ++i) {
                    u16 e;
                    std::memcpy(&e, reinterpret_cast<const void*>(table + kTableOff[kind] + 2 * (slot + i)), 2);
                    if (same && e != 0xffff && (e & 0x8000) == 0) {
                        std::memcpy(src.data() + i * bytes, cue_bytes + 0x90 + st * 0x1800 + (e & 0x3fff) * 4, bytes);
                    } else {
                        for (u32 j = 0; j < bytes; ++j) {
                            src[i * bytes + j] = static_cast<u8>(rng() % 4);
                        }
                    }
                }
                Res::BeforeSetter(kind, cue, st, slot, count, reinterpret_cast<u64>(src.data()));
                CSB::Recording r;
                CSB::RunRecorded(static_cast<CSB::Kind>(kind), CSB::RecordingOriginal(static_cast<CSB::Kind>(kind)),
                                 true, cue, st, slot, count, reinterpret_cast<u64>(src.data()), r);
                for (u32 c = 0; c < r.calls; ++c) { // the direct descriptors reach the GPU's user data
                    const CSB::Call& call = r.call[c];
                    for (u32 j = 0; j < static_cast<u32>(call.count) && call.sgpr + j < 16; ++j) {
                        std::memcpy(&g_ud[st][call.sgpr + j], call.data + 4 * j, 4);
                        g_ud_mask[st] |= 1u << (call.sgpr + j);
                    }
                }
            } else if (pick < 90) { // a commit: the game's or the layer's
                const bool native = rng() % 2 == 0;
                if (native && ND::CheckNativeCommit(cue) == ND::CommitCheck::Native) {
                    RegWrite writes[kMaxRegWrites];
                    const u32 k = ND::NativeCommit(cue, env, writes);
                    ApplyUserData(writes, k);
                    ++native_commits;
                } else {
                    refused += native ? 1 : 0;
                    Res::Commit(commit, cue);
                    ++game_commits;
                }
                NRL::NativeResourceBindings b;
                hooks->fill(cue, b); // (the draw's view: R7Draw takes it through the hook)
                for (u32 d = 0; d < 2; ++d) {
                    const u32 st = d == 0 ? 2 : 1;
                    const auto& r = b.stage[d];
                    const u64 layout = w.get64(0xc180 + 8 * st);
                    if (r.present) {
                        committed[st] = true;
                    }
                    if (r.present != committed[st]) {
                        fail(bad_present, fmt::format("case {} op {}: stage {} present {} expected {}", n, op, st,
                                                      r.present, committed[st]));
                    }
                    if (!r.present) {
                        continue;
                    }
                    ++present;
                    // the program by value (the test's shader block at kShader + 8), the reset epoch
                    // (CUE::ResetShaders clears the shader objects; +0xc1d9 / +0xc1da: the stage set another way)
                    u32 pgm_lo = 0;
                    const u64 shader_obj = w.get64(st == 2 ? 0xc0b0 : 0xc0a8);
                    const bool alt = cue_bytes[st == 2 ? 0xc1da : 0xc1d9] != 0;
                    if (shader_obj != 0 && !alt) {
                        std::memcpy(&pgm_lo, reinterpret_cast<const void*>(shader_obj + 8), 4);
                    }
                    const u64 want_program = u64{pgm_lo} << 8;
                    if (r.program != want_program) {
                        fail(bad_program, fmt::format("case {} op {}: stage {} program {:#x} expected {:#x}", n, op,
                                                      st, r.program, want_program));
                    }
                    if (!epoch_known) { // the first binding since the sequence's start or a reset: a later epoch
                        if (epoch_prev_known && r.epoch <= epoch_prev) {
                            fail(bad_epoch, fmt::format("case {} op {}: stage {} epoch {} after a reset, before {}", n,
                                                        op, st, r.epoch, epoch_prev));
                        }
                        epoch_now = r.epoch;
                        epoch_known = true;
                    } else if (r.epoch != epoch_now) {
                        fail(bad_epoch, fmt::format("case {} op {}: stage {} epoch {} without a reset, expected {}", n,
                                                    op, st, r.epoch, epoch_now));
                    }
                    if (r.version == 0) {
                        fail(bad_version0, fmt::format("case {} op {}: stage {} version 0", n, op, st));
                        continue;
                    }
                    std::vector<u8> snap(reinterpret_cast<const u8*>(r.snapshot),
                                         reinterpret_cast<const u8*>(r.snapshot) + u64{r.dwords} * 4);
                    if (std::memcmp(snap.data(), reinterpret_cast<const void*>(r.shadow), snap.size()) != 0) {
                        fail(bad_bytes, fmt::format("case {} op {}: stage {} snapshot != shadow ({} commit)", n, op,
                                                    st, native ? "native" : "game"));
                    }
                    for (u32 i = 0; i < r.shape.refs; ++i) {
                        const u32 sg = r.shape.sgpr[i];
                        const u64 want = r.snapshot + u64{r.shape.offset[i]} * 4;
                        const u64 have = g_ud[st][sg] | (u64{g_ud[st][sg + 1]} << 32);
                        if (have != want || (g_ud_mask[st] >> sg & 3) != 3) {
                            fail(bad_user, fmt::format("case {} op {}: stage {} sgpr {} pointer {:#x} / {:#x}", n, op,
                                                       st, sg, have, want));
                        }
                    }
                    for (u32 sg = 0; sg < 16; ++sg) {
                        if ((r.direct_mask >> sg & 1) != 0 &&
                            ((g_ud_mask[st] >> sg & 1) == 0 || g_ud[st][sg] != r.direct[sg])) {
                            fail(bad_user, fmt::format("case {} op {}: stage {} direct sgpr {}", n, op, st, sg));
                        }
                    }
                    auto it = seen[st].find(r.version);
                    if (it != seen[st].end()) {
                        ++version_seen;
                        const Expect& e = it->second.first;
                        if (e.bytes != snap || e.direct_mask != r.direct_mask || e.layout != layout ||
                            std::memcmp(e.direct, r.direct, sizeof(e.direct)) != 0) {
                            fail(bad_version, fmt::format("case {} op {}: stage {} version {:#x} with other content",
                                                          n, op, st, r.version));
                        }
                        kept_across += it->second.second != native ? 1 : 0;
                    } else {
                        Expect e{snap, r.direct_mask, {}, layout};
                        std::memcpy(e.direct, r.direct, sizeof(e.direct));
                        seen[st].emplace(r.version, std::make_pair(std::move(e), native));
                    }
                }
            } else if (pick < 95) { // a shader change (another layout of the stage), dirty
                const u32 st = 1 + static_cast<u32>(rng() % 2);
                w.SetShader(st, rng() % 2 != 0);
                if (rng() % 2) {
                    w.put64(0xc130, rng());
                }
            } else if (pick < 98) { // single values (0x16970 / 0x169a0), sometimes with the dirty byte
                const u32 st = 1 + static_cast<u32>(rng() % 2);
                const u32 v = static_cast<u32>(rng());
                std::memcpy(cue_bytes + (rng() % 2 ? 0xc0e0 : 0xc100) + 4 * st, &v, 4);
                if (rng() % 2) {
                    cue_bytes[0xc1d0 + st] = 1;
                }
            } else { // R::Reset's CUE::ResetShaders through its probe, then the shader pair again
                reset_fn(cue);
                if (epoch_known) {
                    epoch_prev = epoch_now;
                    epoch_prev_known = true;
                    ++epoch_resets;
                }
                epoch_known = false;
                committed[1] = committed[2] = false;
                for (u32 s = 0; s < 8; ++s) {
                    if (w.get64(0xc180 + 8 * s) == 0) {
                        w.put64(0xc180 + 8 * s, reinterpret_cast<u64>(w.empty));
                    }
                }
                cue_bytes[0xc1c9] = 0;
                w.SetShader(1, rng() % 2 != 0);
                w.SetShader(2, rng() % 2 != 0);
            }
        }
    }
    const Res::Stats t = Res::Totals();
    std::printf("  %llu sequences: commits game %llu, native %llu (refused by CheckNativeCommit %llu); bindings "
                "present %llu, a version seen again %llu (across the two kinds of commit %llu)\n",
                static_cast<unsigned long long>(cases), static_cast<unsigned long long>(game_commits),
                static_cast<unsigned long long>(native_commits), static_cast<unsigned long long>(refused),
                static_cast<unsigned long long>(present), static_cast<unsigned long long>(version_seen),
                static_cast<unsigned long long>(kept_across));
    std::printf("  different: snapshot/shadow %llu, version/content %llu, user data %llu, presence %llu, version 0 "
                "%llu%s%s\n",
                static_cast<unsigned long long>(bad_bytes), static_cast<unsigned long long>(bad_version),
                static_cast<unsigned long long>(bad_user), static_cast<unsigned long long>(bad_present),
                static_cast<unsigned long long>(bad_version0), first.empty() ? "" : "; first: ", first.c_str());
    std::printf("  verify: same version %llu violations %llu, shadow %llu violations %llu, fill %llu mismatches %llu, "
                "stale %llu; external commits %llu\n",
                static_cast<unsigned long long>(t.verify_same_version - t0.verify_same_version),
                static_cast<unsigned long long>(t.verify_violations - t0.verify_violations),
                static_cast<unsigned long long>(t.verify_shadow_checked - t0.verify_shadow_checked),
                static_cast<unsigned long long>(t.verify_shadow_violations - t0.verify_shadow_violations),
                static_cast<unsigned long long>(t.verify_fill_checked - t0.verify_fill_checked),
                static_cast<unsigned long long>(t.verify_fill_mismatch - t0.verify_fill_mismatch),
                static_cast<unsigned long long>(t.fills_stale - t0.fills_stale),
                static_cast<unsigned long long>(t.external_commits - t0.external_commits));
    std::printf("  by value: program different %llu, epoch different %llu (%llu resets between bindings)\n",
                static_cast<unsigned long long>(bad_program), static_cast<unsigned long long>(bad_epoch),
                static_cast<unsigned long long>(epoch_resets));
    R8R_CHECK(bad_bytes == 0 && bad_version == 0 && bad_user == 0 && bad_present == 0 && bad_version0 == 0);
    R8R_CHECK(bad_program == 0 && bad_epoch == 0 && epoch_resets > 0);
    R8R_CHECK(native_commits > 0 && game_commits > 0 && present > 0 && version_seen > 0 && kept_across > 0);
    R8R_CHECK(t.external_commits - t0.external_commits == native_commits);
    R8R_CHECK(t.verify_violations == t0.verify_violations && t.verify_shadow_violations == t0.verify_shadow_violations &&
              t.verify_fill_mismatch == t0.verify_fill_mismatch && t.fills_stale == t0.fills_stale &&
              t.verify_fill_checked > t0.verify_fill_checked);
    R8R_CHECK(g_stack_fail == 0);

    // an unobserved commit (0x140d0: user data without a shadow change) moves the snapshot allocator: Fill vouches for
    // nothing until the next observed commit
    {
        cue_bytes[0xc1d0 + 1] = 1;
        Res::Commit(commit, cue);
        NRL::NativeResourceBindings b;
        hooks->fill(cue, b);
        const bool before = b.stage[1].present;
        const u64 moved0 = Res::Totals().fills_moved;
        w.put64(0x20, w.get64(0x20) + 0x40);
        hooks->fill(cue, b);
        const bool refused_moved = !b.stage[0].present && !b.stage[1].present && Res::Totals().fills_moved == moved0 + 1;
        cue_bytes[0xc1d0 + 1] = 1;
        Res::Commit(commit, cue);
        hooks->fill(cue, b);
        const bool again = b.stage[1].present && b.stage[1].version != 0;
        std::printf("  the snapshot allocator moved past the state: %s, after the next commit: %s\n",
                    refused_moved ? "nothing handed to the draw" : "BINDINGS HANDED", again ? "handed" : "NOT HANDED");
        R8R_CHECK(before && refused_moved && again);
    }

    // without the hooks the state misses a native commit: the stage that changed is not handed to a draw as current
    {
        ND::CommitEnv bare = env;
        bare.hooks = nullptr;
        const u32 st = 1; // PS
        cue_bytes[0xc1d0 + st] = 1;
        Res::Commit(commit, cue); // observed: the PS binding current
        cue_bytes[0x90 + st * 0x1800] ^= 0x5a; // a change the state does not see (dword 0: in every snapshot) ...
        const u64 before_external = Res::Totals().external_commits;
        cue_bytes[0xc1d0 + st] = 1; // ... committed natively, unreported
        if (ND::CheckNativeCommit(cue) == ND::CommitCheck::Native) {
            RegWrite writes[kMaxRegWrites];
            ND::NativeCommit(cue, bare, writes);
        }
        const u64 moved0 = Res::Totals().fills_moved;
        NRL::NativeResourceBindings b;
        hooks->fill(cue, b);
        // the unreported commit's snapshot moved the allocator: Fill hands nothing (before the allocator check it
        // handed the previous snapshot, whose bytes are not the shadow's - what NR2's report fixes)
        const bool refused = !b.stage[0].present && !b.stage[1].present && Res::Totals().fills_moved == moved0 + 1;
        std::printf("  a native commit not reported: %s\n",
                    refused ? "Fill hands nothing (the snapshot allocator moved past the state)" : "BINDINGS HANDED");
        R8R_CHECK(refused && Res::Totals().external_commits == before_external);
    }
    Res::Deactivate();
    R8R_CHECK(NRL::GetResourceHooks() == nullptr);
}

// --- 3. NR5 -------------------------------------------------------------------------------------------------------------

void TestPathSwitches() {
    std::printf("R8 NR5: the layer's paths without their switches:\n");
    const bool none = !NRL::Carries(NRL::EngineFn::GBufferMesh) && !NRL::Carries(NRL::EngineFn::ShadowInstanced) &&
                      !NRL::Carries(NRL::EngineFn::Other);
    const bool unset = std::getenv("SSPC_R7_NATIVE_RENDER") == nullptr && std::getenv("SSPC_R8_RENDER_PATHS") == nullptr &&
                       std::getenv("SSPC_R8_RENDER_RESOURCES") == nullptr;
    std::printf("  carries the mesh / shadow draws: %s; resources: %s%s\n", none ? "no" : "YES",
                NRL::ResourcesOn() ? "on" : "off", unset ? "" : " (switches set in the environment)");
    R8R_CHECK(!unset || (none && !NRL::ResourcesOn() && !NRL::Carries(NRL::EngineFn::GBufferInstanced)));
}

} // namespace

int R8RenderTests(u64 image_base, u64 image_size, void* recomp_module, const std::string& recomp_dll, u64 cases,
                  int& checks) {
    (void)image_size, (void)recomp_dll;
    g_fail = 0;
    g_checks = &checks;
    TestPathSwitches();
    TestNativeCommitReported(image_base, recomp_module, std::max<u64>(cases / 500, 200));
    TestCommitDispatch(image_base, recomp_module, std::max<u64>(cases / 1000, 300));
    return g_fail;
}

} // namespace SecondSon::NativeEngine::Tests
