// SecondSonPC — libSceLibcInternal: vprintf, fflush, _Stdout and sceLibcHeapGetTraceInfo (R10 sysrt; oracles:
// third_party/shadPS4/src/core/libraries/libc_internal/libc_internal_io.cpp internal_fflush,
// libc_internal_memory.cpp sceLibcHeapGetTraceInfo; vprintf and _Stdout have none: COMMON_STUB / UNRESOLVED).
//
// Who calls them [A: tools/r10/import_sites.py, docs/R10_SYSRT.md §4]:
//   libSceFios2.prx 0x38650, its print function: `if (hook) return hook(fmt, ap); r = vprintf(fmt, ap);
//   fflush(&_Stdout); return r;` - _Stdout through an R_X86_64_64 slot (0x5c400). Unresolved, the slot keeps 0
//   and the call is fflush(NULL). Here _Stdout is an object of this module: fflush(stdout).
//   libc.prx 0x5b8dc / 0x5bd79 (its heap start-up): sceLibcHeapGetTraceInfo(&info) with size 0x20, flag 1; then
//   get_segment_info != 0 turns heap tracing on, the mask and the table are kept for the mspaces.
//
// The guest's standard output goes to SecondSonPC.log ("guest stdout: ..."), a line at a time: text without a line
// end waits for the next one or for fflush (of _Stdout or NULL), as a line-buffered stdout. One buffer for all
// threads, under a lock: a line of one call is never split by another thread's.
#include "runtime/libc/libc_stdio.h"

#include <cstring>
#include <mutex>
#include <string_view>

#include "base/log.h"
#include "runtime/exports.h"
#include "runtime/libc/guest_printf.h"

namespace SecondSon::Runtime::Libc {

namespace {

constexpr s32 kEof = -1;
constexpr size_t kMaxPending = 8 * 1024; // a longer text without a line end is written as it is
constexpr u16 kFileModeWritePending = 0x2000; // Dinkumware FILE._Mode: bytes in the buffer not written yet (oracle)

// The guest's _Stdout: a FILE of the console's libc (Dinkumware, ~0x150 bytes in the oracle's OrbisFILE). The game's
// modules only take its address (Fios2: for fflush); nothing reads its fields. Zeroed, room for the whole structure.
alignas(16) u8 g_stdout_file[0x200] = {};

std::mutex g_stdout_mutex;
std::string g_pending;

void EmitLocked(bool all) {
    size_t from = 0;
    for (;;) {
        const size_t nl = g_pending.find('\n', from);
        if (nl == std::string::npos) {
            break;
        }
        SSPC_LOG(App, Info, "guest stdout: {}", std::string_view(g_pending).substr(from, nl - from));
        from = nl + 1;
    }
    g_pending.erase(0, from);
    if (!g_pending.empty() && (all || g_pending.size() > kMaxPending)) {
        SSPC_LOG(App, Info, "guest stdout: {}", g_pending);
        g_pending.clear();
    }
}

SSPC_SYSV_ABI s32 Vprintf(const char* format, SysvVaList* ap) {
    if (format == nullptr || ap == nullptr) {
        return kEof;
    }
    SysvVaList copy = *ap; // (the caller's va_list is indeterminate after the call in C; it is left as it was)
    std::string text;
    const s32 n = FormatGuest(text, format, &copy);
    if (n < 0) {
        return kEof;
    }
    std::scoped_lock lock{g_stdout_mutex};
    g_pending += text;
    EmitLocked(false);
    return n;
}

SSPC_SYSV_ABI s32 Fflush(void* file) {
    if (file == nullptr || file == g_stdout_file) {
        // NULL: every output stream of the library; it has one, _Stdout
        std::scoped_lock lock{g_stdout_mutex};
        EmitLocked(true);
        return 0;
    }
    // A FILE of the library not made here: libSceLibcInternal's stream constructors are not imported by the game, so
    // only one with nothing to write can come (the oracle's check: _Mode & 0x2000). One with pending bytes is refused
    // (EOF, the stream untouched): writing it needs its descriptor and lock.
    u16 mode;
    std::memcpy(&mode, file, sizeof mode);
    if ((mode & kFileModeWritePending) == 0) {
        return 0;
    }
    SSPC_LOG(App, Error, "fflush({}): a FILE with pending output not made by SecondSonPC's libSceLibcInternal: refused",
             file);
    return kEof;
}

alignas(8) u64 g_mspace_atomic_id_mask = 0;
alignas(64) u64 g_mstate_table[kMstateTableEntries] = {};

SSPC_SYSV_ABI void HeapGetTraceInfo(HeapTraceInfo* info) {
    if (info == nullptr) {
        return;
    }
    // no heap tracer: tracing off; the id mask and the state table are this process's own (one owner: libc.prx's
    // mspaces take ids 0..63 from the mask, the table has room for every 7-bit index a chunk can carry)
    info->get_segment_info = 0;
    info->mspace_atomic_id_mask = &g_mspace_atomic_id_mask;
    info->mstate_table = g_mstate_table;
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

const Export kExports[] = {
    {"GMpvxPFW924", "libSceLibcInternal", "libSceLibcInternal", "vprintf", Fn(&Vprintf), nullptr, "sysrt-libc", false,
     true},
    {"MUjC4lbHrK4", "libSceLibcInternal", "libSceLibcInternal", "fflush", Fn(&Fflush), nullptr, "sysrt-libc", false,
     true},
    {"2sWzhYqFH4E", "libSceLibcInternal", "libSceLibcInternal", "_Stdout", g_stdout_file, nullptr, "sysrt-libc", false,
     true},
    {"NWtTN10cJzE", "libSceLibcInternalExt", "libSceLibcInternal", "sceLibcHeapGetTraceInfo", Fn(&HeapGetTraceInfo),
     nullptr, "sysrt-libc", false, true},
};

} // namespace

std::string PendingStdout() {
    std::scoped_lock lock{g_stdout_mutex};
    return g_pending;
}

void* StdoutObject() {
    return g_stdout_file;
}

std::span<const Export> LibcStdioExports() {
    return kExports;
}

} // namespace SecondSon::Runtime::Libc
