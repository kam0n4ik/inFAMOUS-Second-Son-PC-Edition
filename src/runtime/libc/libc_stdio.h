// SecondSonPC — libSceLibcInternal's standard output and heap trace information (R10 sysrt, docs/R10_SYSRT.md §4).
// Group "sysrt-libc" (experimental: BRIDGE / COMMON_STUB / UNRESOLVED unless --native sysrt-libc or
// SSPC_R10_SYSRT=native).
#pragma once

#include <span>
#include <string>

#include "base/abi.h"

namespace SecondSon::Runtime {
struct Export;
}

namespace SecondSon::Runtime::Libc {

/// sceLibcHeapGetTraceInfo's structure (the oracle's HeapInfoInfo; libc.prx 0x5b8cb/0x5bd68 passes it on its stack,
/// 32 bytes, size = 0x20 and flag = 1 set by the caller, then reads the last three fields).
struct HeapTraceInfo {
    u64 size;
    u32 flag;
    u32 get_segment_info;       // non-zero: libc.prx turns its heap tracing on
    u64* mspace_atomic_id_mask; // libc.prx allocates mspace ids in it with lock cmpxchg (bit i = id i)
    u64* mstate_table;          // libc.prx stores mspace id i's state at [i + 1], looks a chunk's up by a 7-bit index
};
static_assert(sizeof(HeapTraceInfo) == 32);
constexpr size_t kMstateTableEntries = 128;

/// The guest's standard output so far that has not ended in a line yet (diagnostics, self-test).
std::string PendingStdout();
/// The address of the guest's `_Stdout` FILE object (self-test).
void* StdoutObject();

std::span<const Export> LibcStdioExports();

} // namespace SecondSon::Runtime::Libc
