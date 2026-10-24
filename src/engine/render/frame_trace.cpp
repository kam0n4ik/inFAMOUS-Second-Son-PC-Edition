// SecondSonPC — dynamic trace of the engine's render frame (see frame_trace.h).
#include "engine/render/frame_trace.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <windows.h>

#include "base/log.h"
#include "engine/guest_probe.h"
#include "engine/render/dcb_scan.h"
#include "engine/render/pass_graph.h"
#include "engine/render/render_layout.h"
#include "runtime/gpu/critical_compute.h"
#include "runtime/gpu/gnm/gnm_driver.h"
#include "runtime/gpu/r11/label_wait.h"
#include "runtime/gpu/stall_trace.h"

namespace SecondSon::Engine::Render::Trace {

namespace {

namespace L = Layout;

// --- what the probes are installed on (eboot.bin of CUSA00004; tools/native-reconstruction/function_hashes.py) ------
constexpr ProbeSite kSubmitFrameSite{{L::kSubmitFrame, 0xb51, 0x4a5ea347de017770ull}, 20};
constexpr ProbeSite kResetSite{{L::kContextReset, 0x164, 0xa8a9df2ac8bb0624ull}, 20};
constexpr ProbeSite kReserveSite{{L::kDcbReserve, 0xb1, 0x4a5898f412269a7full}, 18};
constexpr ProbeSite kSetterSites[4] = {
    {{L::kSetTextures, 0xc3, 0xec36a84fd3ea6732ull}, 17},
    {{L::kSetRwTextures, 0xc6, 0x10784ab701e6ed41ull}, 17},
    {{L::kSetBuffers, 0xc3, 0x89684355b1fee78cull}, 17},
    {{L::kSetRwBuffers, 0xc6, 0xe44b61ff8aa2933bull}, 17},
};
constexpr ResourceKind kSetterKinds[4] = {ResourceKind::Texture, ResourceKind::RwTexture, ResourceKind::Buffer,
                                          ResourceKind::RwBuffer};
constexpr u32 kSetterDwords[4] = {8, 8, 4, 4};
// Functions whose behaviour the trace assumes (frame layout, pool, submission order).
constexpr GuestFunction kAssumed[] = {
    {0x1f2c60, 0x1f, 0xef1991cc4587b6dfull},  // AcquireContext: pool slot = frame + 0x1dcc0 + i * 0xdef0
    {0x1f2c80, 0x78, 0x732887cdc1f0c5c0ull},  // AcquireContext with the key of another context
    {0x1f2d00, 0x6d, 0x508742f4e89e9133ull},  // AcquireContext with key + state (chunks of a pass)
    {0x11f2c0, 0x69, 0x6f5977e13daedae8ull},  // GetSubmitList: chunks at R+0xcc70 + the current one
    {0x1ef490, 0x4d, 0x8965d7a7e907cae3ull},  // qsort comparator: pass (signed), sub (unsigned), address
    {0x198a70, 0x142, 0xf9148f1bdad6e925ull}, // LaunchPasses: 41 contexts, key {pass, 0}
    {0x1f14e0, 0x6b2, 0xaac6e89cd7ca59b1ull}, // frame overflow check: the only per-frame caller of Reset
};

using SubmitFrameFn = SSPC_SYSV_ABI void (*)(u8* frame, u32 flags);
using ResetFn = SSPC_SYSV_ABI void (*)(u8* context);
using ReserveFn = SSPC_SYSV_ABI u64 (*)(void* cb, u32 dwords, u8* context);
using SetterFn = SSPC_SYSV_ABI void (*)(u8* cue, s32 stage, u32 slot, s32 count, const u32* descriptors);

struct Options {
    u32 every = 300;
    u32 dumps = 8;
    u32 stats_s = 10;
    std::filesystem::path dir = "logs\\p53_render_trace";
};

u64 g_base = 0;
Options g_options;
SubmitFrameFn g_submit = nullptr;
bool g_lifecycle = false; ///< P5.4: SubmitFrame's span goes to the stall trace (SSPC_P54_FRAME_TRACE=1)
bool g_critical = false;  ///< CC-2: the async compute wait counts its blocked waits (SSPC_CC_CRITICAL=1)
bool g_wait_hooked = false; ///< the async compute wait 0x1695e0 is replaced (by the lifecycle trace or CC-2)
bool g_label_wait = false;  ///< R11: the wait sleeps on its label (SSPC_R11_LABEL_WAIT=1, runtime/gpu/r11/label_wait.h)
u32 g_label_spin_us = 20;   ///< ... after spinning this long (SSPC_R11_LABEL_WAIT_SPIN_US)
std::atomic<u64> g_label_waits{0};
void SubmitFrameTimed(u8* frame, u32 flags);
ResetFn g_reset = nullptr;
ReserveFn g_reserve = nullptr;
SetterFn g_setters[4] = {};

// --- counters (any thread) -----------------------------------------------------------------------------------------
struct Counters {
    std::atomic<u64> frames{0};
    std::atomic<u64> slots_seen{0};      ///< pool slots acquired, summed over frames
    std::atomic<u64> slots_fresh{0};     ///< ... whose first chunk differs from the one they ended the last frame with
    std::atomic<u64> slots_stale{0};     ///< ... whose first chunk is the same (the buffer was not restarted)
    std::atomic<u64> slots_first_use{0}; ///< ... not acquired in the last frame
    std::atomic<u64> slots_empty{0};     ///< ... with no command buffer at all (begin = 0: reset, nothing recorded)
    std::atomic<u64> max_chunks{0};
    std::atomic<u64> resets{0};
    std::atomic<u64> resets_pool{0};
    std::atomic<u64> reserves{0};
    std::atomic<u64> ccb_used{0}; ///< contexts whose constant command buffer (G+0x80) held commands at submission
    std::atomic<u64> setter_records{0};
    std::atomic<u64> samples{0};
};
Counters g_count;

std::mutex g_callers_lock;
std::map<u64, u64> g_reset_callers; ///< caller rva -> calls

// --- guest memory reads ----------------------------------------------------------------------------------------------
template <typename T>
T At(u64 address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

bool Readable(u64 address, u64 bytes) {
    u64 at = address;
    while (at < address + bytes) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0) {
            return false;
        }
        at = reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

u64 Frame() {
    return g_base + L::kFrame;
}
u64 PoolSlot(u32 i) {
    return Frame() + L::kFramePool + u64{i} * L::kPoolSlotStride;
}
bool IsPoolSlot(u64 r) {
    const u64 first = PoolSlot(0);
    return r >= first && r < first + u64{L::kPoolSlots} * L::kPoolSlotStride && (r - first) % L::kPoolSlotStride == 0;
}

/// The first chunk of a context's DCB: the oldest finished chunk, or the current one.
u64 FirstChunk(u64 r) {
    const u32 chunks = At<u32>(r + L::kContextChunkCount);
    return chunks != 0 ? At<u64>(r + L::kContextChunks) : At<u64>(r + L::kContextGfx + L::kCbBegin);
}

struct Span {
    u64 start;
    u32 dwords;
};

/// What GetSubmitList (0x11f2c0) hands SubmitFrame for the context.
std::vector<Span> SubmitList(u64 r) {
    std::vector<Span> list;
    const u32 chunks = std::min(At<u32>(r + L::kContextChunkCount), L::kMaxChunks);
    for (u32 i = 0; i < chunks; ++i) {
        const u64 entry = r + L::kContextChunks + u64{i} * L::kChunkStride;
        list.push_back({At<u64>(entry), At<u32>(entry + 8)});
    }
    const u64 begin = At<u64>(r + L::kContextGfx + L::kCbBegin);
    const u64 cmd = At<u64>(r + L::kContextGfx + L::kCbCmdPtr);
    if (cmd > begin && cmd - begin < (u64{1} << 32)) {
        list.push_back({begin, static_cast<u32>((cmd - begin) / 4)});
    }
    return list;
}

bool CurrentChunkEmpty(u64 r) {
    const u64 begin = At<u64>(r + L::kContextGfx + L::kCbBegin);
    const u64 cmd = At<u64>(r + L::kContextGfx + L::kCbCmdPtr);
    return ((cmd - begin) & 0xFFFFFFFCull) == 0;
}

u64 CcbBytes(u64 r) {
    const u64 cb = r + L::kContextGfx + L::kGfxCcb;
    const u64 begin = At<u64>(cb + L::kCbBegin);
    const u64 cmd = At<u64>(cb + L::kCbCmdPtr);
    return cmd >= begin && cmd - begin < (u64{1} << 28) ? cmd - begin : 0;
}

// --- resource bindings recorded by the setter probes -----------------------------------------------------------------
struct SetterRecord {
    u64 cue;
    u32 desc[8];
    u8 kind;
    u8 stage;
    u16 slot;
};
struct ThreadLog {
    std::mutex lock;
    std::vector<SetterRecord> records;
};
std::mutex g_logs_lock;
std::vector<std::unique_ptr<ThreadLog>> g_logs;
thread_local ThreadLog* t_log = nullptr;
std::atomic<bool> g_armed{false};
constexpr size_t kMaxRecordsPerThread = 1u << 20;

ThreadLog* MyLog() {
    if (t_log == nullptr) {
        auto log = std::make_unique<ThreadLog>();
        t_log = log.get();
        std::lock_guard guard(g_logs_lock);
        g_logs.push_back(std::move(log));
    }
    return t_log;
}

template <int K>
SSPC_SYSV_ABI void SetterProbe(u8* cue, s32 stage, u32 slot, s32 count, const u32* descriptors) {
    if (g_armed.load(std::memory_order_relaxed) && count > 0 && descriptors != nullptr) {
        ThreadLog* log = MyLog();
        std::lock_guard guard(log->lock);
        const s32 n = std::min(count, 64);
        for (s32 i = 0; i < n && log->records.size() < kMaxRecordsPerThread; ++i) {
            SetterRecord rec{};
            rec.cue = reinterpret_cast<u64>(cue);
            std::memcpy(rec.desc, descriptors + i * kSetterDwords[K], kSetterDwords[K] * 4);
            rec.kind = static_cast<u8>(K);
            rec.stage = static_cast<u8>(stage);
            rec.slot = static_cast<u16>(slot + i);
            log->records.push_back(rec);
        }
    }
    g_setters[K](cue, stage, slot, count, descriptors);
}

std::vector<SetterRecord> TakeRecords() {
    std::vector<SetterRecord> all;
    std::lock_guard guard(g_logs_lock);
    for (const auto& log : g_logs) {
        std::lock_guard g(log->lock);
        all.insert(all.end(), log->records.begin(), log->records.end());
        log->records.clear();
    }
    return all;
}

// --- Reset / DcbReserve probes ----------------------------------------------------------------------------------------
SSPC_SYSV_ABI void ResetProbe(u8* context) {
    g_count.resets.fetch_add(1, std::memory_order_relaxed);
    if (IsPoolSlot(reinterpret_cast<u64>(context))) {
        g_count.resets_pool.fetch_add(1, std::memory_order_relaxed);
    }
    const u64 caller = reinterpret_cast<u64>(__builtin_return_address(0)) - g_base;
    {
        std::lock_guard guard(g_callers_lock);
        ++g_reset_callers[caller];
    }
    g_reset(context);
}

SSPC_SYSV_ABI u64 ReserveProbe(void* cb, u32 dwords, u8* context) {
    g_count.reserves.fetch_add(1, std::memory_order_relaxed);
    return g_reserve(cb, dwords, context);
}

// --- the frame -----------------------------------------------------------------------------------------------------
struct SlotMemo {
    u64 first = 0;
    bool valid = false;
};
SlotMemo g_memo[L::kPoolSlots]; // main thread (SubmitFrame) only
u64 g_frame_no = 0;
bool g_sample_this = false;
u32 g_dumps_written = 0;
bool g_warned_frame = false;

struct PoolEntry {
    u64 r;
    s32 pass;
    u32 sub;
};

std::string Hex(u64 v) {
    return fmt::format("\"{:#x}\"", v);
}

std::string TargetJson(const TargetDesc& t) {
    return fmt::format("{{\"base\":{},\"w\":{},\"h\":{},\"fmt\":{},\"depth\":{}}}", Hex(t.base), t.width, t.height,
                       t.format, t.depth ? "true" : "false");
}

std::string UseJson(const ResourceUse& u) {
    static constexpr const char* kKinds[] = {"tex", "rwtex", "buf", "rwbuf"};
    return fmt::format("{{\"base\":{},\"a\":{},\"b\":{},\"fmt\":{},\"kind\":\"{}\"}}", Hex(u.base), u.width, u.height,
                       u.format, kKinds[static_cast<u8>(u.kind)]);
}

std::string FunctionRva(u64 pointer) {
    return pointer >= g_base && pointer < g_base + 0x2000000 ? Hex(pointer - g_base) : "null";
}

void WriteSample(const std::vector<ContextRecord>& ordered, const Graph& graph, u32 acquired, u64 unattributed,
                 double scan_ms) {
    std::string out;
    auto it = std::back_inserter(out);
    fmt::format_to(it, "{{\n\"frame\":{},\"acquired\":{},\"contexts\":{},\"unattributed_bindings\":{},"
                       "\"scan_ms\":{:.2f},\n\"context_list\":[\n",
                   g_frame_no, acquired, ordered.size(), unattributed, scan_ms);
    for (size_t i = 0; i < ordered.size(); ++i) {
        const ContextRecord& c = ordered[i];
        static constexpr const char* kKinds[] = {"deform", "pre", "pass", "post"};
        fmt::format_to(it,
                       "{{\"kind\":\"{}\",\"pass\":{},\"sub\":{},\"r\":{},\"chunks\":{},\"bytes\":{},\"ccb\":{},"
                       "\"draws\":{},\"dispatches\":{},\"bindings\":{}}}{}\n",
                       kKinds[static_cast<u8>(c.kind)], c.pass, c.sub, Hex(c.context), c.chunks, c.bytes,
                       c.ccb_bytes, c.scan.draws + c.scan.draws_indirect,
                       c.scan.dispatches, c.uses.size(), i + 1 < ordered.size() ? "," : "");
    }
    fmt::format_to(it, "],\n\"nodes\":[\n");
    for (size_t i = 0; i < graph.nodes.size(); ++i) {
        const Node& n = graph.nodes[i];
        const ScanResult& s = n.scan;
        std::string fns = "null";
        if (n.kind == NodeKind::Pass && n.id >= 0 && n.id < static_cast<s32>(L::kPassCount)) {
            const u64 entry = g_base + L::kPassTable + u64(n.id) * 24;
            fns = fmt::format("[{},{},{}]", FunctionRva(At<u64>(entry)), FunctionRva(At<u64>(entry + 8)),
                              FunctionRva(At<u64>(entry + 16)));
        }
        fmt::format_to(it,
                       "{{\"index\":{},\"name\":\"{}\",\"pass_functions\":{},\"contexts\":{},\"bytes\":{},"
                       "\"packets\":{},\"draws\":{},\"draws_indirect\":{},\"draws_instanced\":{},\"indices\":{},"
                       "\"dispatches\":{},\"depth_test\":{},\"depth_write\":{},\"no_color\":{},\"events\":{},"
                       "\"cache_actions\":{},\"waits\":{},\"dma\":{},\"indirect_buffers\":{},\"malformed\":{},"
                       "\"shaders\":{},\"targets\":[",
                       i, NodeName(n), fns, n.contexts, n.bytes, s.packets, s.draws, s.draws_indirect,
                       s.draws_instanced, s.indices, s.dispatches, s.draws_depth_test, s.draws_depth_write,
                       s.draws_no_color, s.events, s.cache_actions, s.waits, s.dma, s.indirect_buffers, s.malformed,
                       s.shaders.size());
        for (size_t k = 0; k < s.targets.size(); ++k) {
            fmt::format_to(it, "{}{}", k ? "," : "", TargetJson(s.targets[k]));
        }
        fmt::format_to(it, "],\"depth_read\":[");
        for (size_t k = 0; k < s.depth_read.size(); ++k) {
            fmt::format_to(it, "{}{}", k ? "," : "", TargetJson(s.depth_read[k]));
        }
        u32 tex = 0, buf = 0;
        for (const ResourceUse& u : n.reads) {
            (u.kind == ResourceKind::Texture ? tex : buf) += 1;
        }
        fmt::format_to(it, "],\"textures_read\":{},\"buffers_read\":{},\"rw\":[", tex, buf);
        u32 written = 0;
        for (const ResourceUse& u : n.writes) {
            if (u.kind == ResourceKind::RwTexture || u.kind == ResourceKind::RwBuffer) {
                fmt::format_to(it, "{}{}", written++ ? "," : "", UseJson(u));
                if (written >= 64) {
                    break;
                }
            }
        }
        fmt::format_to(it, "],\"texture_sample\":[");
        u32 listed = 0;
        for (const ResourceUse& u : n.reads) {
            if (u.kind == ResourceKind::Texture && listed < 48) {
                fmt::format_to(it, "{}{}", listed++ ? "," : "", UseJson(u));
            }
        }
        // P5.3D: the PM4 the node still holds and the native operations in it (what native apply replaced shows here).
        fmt::format_to(it, "],\"native_ops\":[{},{},{}],\"opcodes\":{{", s.native_ops[1], s.native_ops[2],
                       s.native_ops[3]);
        const auto counts = [&](const auto& a, u32 base, const char* prefix) {
            u32 n = 0;
            for (size_t k = 0; k < a.size(); ++k) {
                if (a[k] != 0) {
                    fmt::format_to(it, "{}\"{}{:x}\":{}", n++ ? "," : "", prefix, base + k, a[k]);
                }
            }
        };
        counts(s.opcode_packets, 0, "0x");
        fmt::format_to(it, "}},\"context_regs\":{{");
        counts(s.context_writes, 0xA000, "0x");
        fmt::format_to(it, "}},\"sh_regs\":{{");
        counts(s.sh_writes, 0x2C00, "0x");
        fmt::format_to(it, "}},\"uconfig_regs\":{{");
        counts(s.uconfig_writes, 0xC000, "0x");
        fmt::format_to(it, "}}}}{}\n", i + 1 < graph.nodes.size() ? "," : "");
    }
    fmt::format_to(it, "],\n\"edges\":[\n");
    for (size_t i = 0; i < graph.edges.size(); ++i) {
        const Edge& e = graph.edges[i];
        fmt::format_to(it, "{{\"from\":\"{}\",\"to\":\"{}\",\"resources\":{},\"sample\":{},\"prev_frame\":{}}}{}\n",
                       NodeName(graph.nodes[e.from]), NodeName(graph.nodes[e.to]), e.resources, Hex(e.sample),
                       e.prev_frame ? "true" : "false", i + 1 < graph.edges.size() ? "," : "");
    }
    fmt::format_to(it, "],\n\"lifetimes\":[\n");
    for (size_t i = 0; i < graph.lifetimes.size(); ++i) {
        const Lifetime& l = graph.lifetimes[i];
        fmt::format_to(it,
                       "{{\"base\":{},\"target\":{},\"first_write\":\"{}\",\"last_write\":\"{}\",\"first_read\":{},"
                       "\"last_read\":{},\"readers\":{}}}{}\n",
                       Hex(l.base), l.target ? "true" : "false", NodeName(graph.nodes[l.first_write]),
                       NodeName(graph.nodes[l.last_write]),
                       l.first_read < 0 ? "null" : fmt::format("\"{}\"", NodeName(graph.nodes[l.first_read])),
                       l.last_read < 0 ? "null" : fmt::format("\"{}\"", NodeName(graph.nodes[l.last_read])), l.readers,
                       i + 1 < graph.lifetimes.size() ? "," : "");
    }
    fmt::format_to(it, "]\n}}\n");

    std::error_code ec;
    std::filesystem::create_directories(g_options.dir, ec);
    const auto file = g_options.dir / fmt::format("frame_{:06}.json", g_frame_no);
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    f << out;
    SSPC_LOG(Engine, Info, "render trace: frame {} -> {} ({} contexts, {} nodes, {} edges, scan {:.1f} ms)",
             g_frame_no, file.string(), ordered.size(), graph.nodes.size(), graph.edges.size(), scan_ms);
}

void Sample(const std::vector<PoolEntry>& pool, u32 acquired) {
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<SetterRecord> records = TakeRecords();
    std::unordered_map<u64, std::vector<ResourceUse>> uses; // by context R
    for (const SetterRecord& rec : records) {
        const ResourceKind kind = kSetterKinds[rec.kind];
        const bool texture = kind == ResourceKind::Texture || kind == ResourceKind::RwTexture;
        uses[rec.cue - L::kContextCue].push_back(texture ? DecodeTexture(rec.desc, kind) : DecodeBuffer(rec.desc, kind));
    }

    std::vector<ContextRecord> ordered;
    const auto add = [&](NodeKind kind, s32 pass, u32 sub, u64 r) {
        ContextRecord c;
        c.kind = kind;
        c.pass = pass;
        c.sub = sub;
        c.context = r;
        c.chunks = At<u32>(r + L::kContextChunkCount);
        c.ccb_bytes = CcbBytes(r);
        if (c.ccb_bytes != 0) {
            g_count.ccb_used.fetch_add(1, std::memory_order_relaxed);
        }
        if (auto u = uses.find(r); u != uses.end()) {
            c.uses = std::move(u->second);
            uses.erase(u);
        }
        ordered.push_back(std::move(c));
    };
    for (u32 k = 0; k < L::kDeformContextCount; ++k) {
        const u64 r = g_base + L::kDeformContexts + u64{k} * L::kContextSize;
        if (!SubmitList(r).empty()) {
            add(NodeKind::Deform, static_cast<s32>(k), 0, r);
        }
    }
    add(NodeKind::Pre, 0, 0, Frame() + L::kFramePreContext);
    std::vector<PoolEntry> sorted;
    for (const PoolEntry& e : pool) {
        if (!CurrentChunkEmpty(e.r)) {
            sorted.push_back(e); // SubmitFrame skips a slot whose current chunk is empty
        }
    }
    std::sort(sorted.begin(), sorted.end(), [](const PoolEntry& a, const PoolEntry& b) {
        if (a.pass != b.pass) {
            return a.pass < b.pass;
        }
        if (a.sub != b.sub) {
            return a.sub < b.sub;
        }
        return a.r < b.r;
    });
    for (const PoolEntry& e : sorted) {
        add(NodeKind::Pass, e.pass, e.sub, e.r);
    }
    add(NodeKind::Post, 0, 0, Frame() + L::kFramePostContext);
    u64 unattributed = 0;
    for (const auto& [r, list] : uses) {
        unattributed += list.size();
    }

    ScanState state;
    for (ContextRecord& c : ordered) {
        for (const Span& s : SubmitList(c.context)) {
            c.bytes += u64{s.dwords} * 4;
            if (s.dwords != 0 && Readable(s.start, u64{s.dwords} * 4)) {
                ScanCommands({reinterpret_cast<const u32*>(s.start), s.dwords}, state, c.scan);
            } else if (s.dwords != 0) {
                ++c.scan.malformed;
            }
        }
    }
    const Graph graph = BuildGraph(ordered);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    g_count.setter_records.fetch_add(records.size(), std::memory_order_relaxed);
    g_count.samples.fetch_add(1, std::memory_order_relaxed);
    WriteSample(ordered, graph, acquired, unattributed, ms);
}

SSPC_SYSV_ABI void SubmitFrameProbe(u8* frame, u32 flags) {
    ++g_frame_no;
    g_count.frames.fetch_add(1, std::memory_order_relaxed);
    const bool ours = reinterpret_cast<u64>(frame) == Frame();
    if (!ours && !g_warned_frame) {
        g_warned_frame = true;
        SSPC_LOG(Engine, Warning, "render trace: SubmitFrame on {:#x}, not the frame object {:#x}; not traced",
                 reinterpret_cast<u64>(frame), Frame());
    }
    std::vector<PoolEntry> pool;
    u32 acquired = 0;
    if (ours) {
        acquired = static_cast<u32>(std::clamp(At<s32>(Frame() + L::kFrameAcquired), 0, s32{L::kPoolSlots}));
        u64 max_chunks = 0;
        for (u32 i = 0; i < acquired; ++i) {
            const u64 r = PoolSlot(i);
            const u64 first = FirstChunk(r);
            max_chunks = std::max<u64>(max_chunks, At<u32>(r + L::kContextChunkCount));
            if (!g_memo[i].valid) {
                g_count.slots_first_use.fetch_add(1, std::memory_order_relaxed);
            } else if (first == 0) {
                g_count.slots_empty.fetch_add(1, std::memory_order_relaxed); // reset, nothing recorded since
            } else if (g_memo[i].first == first) {
                g_count.slots_stale.fetch_add(1, std::memory_order_relaxed);
            } else {
                g_count.slots_fresh.fetch_add(1, std::memory_order_relaxed);
            }
            pool.push_back({r, At<s32>(r + L::kSlotPass), At<u32>(r + L::kSlotSub)});
        }
        for (u32 i = acquired; i < L::kPoolSlots; ++i) {
            g_memo[i].valid = false;
        }
        g_count.slots_seen.fetch_add(acquired, std::memory_order_relaxed);
        u64 prev = g_count.max_chunks.load(std::memory_order_relaxed);
        while (max_chunks > prev && !g_count.max_chunks.compare_exchange_weak(prev, max_chunks)) {
        }
        if (g_sample_this) {
            g_armed.store(false, std::memory_order_relaxed);
        }
    }

    if (g_lifecycle) {
        SubmitFrameTimed(frame, flags);
    } else {
        g_submit(frame, flags);
    }

    if (!ours) {
        return;
    }
    for (u32 i = 0; i < acquired; ++i) {
        g_memo[i] = {FirstChunk(PoolSlot(i)), true};
    }
    if (g_sample_this) {
        g_sample_this = false;
        Sample(pool, acquired);
        ++g_dumps_written;
    }
    if (g_dumps_written < g_options.dumps && g_options.every != 0 && (g_frame_no + 1) % g_options.every == 0) {
        TakeRecords(); // drop what was bound outside a sampled frame
        g_sample_this = true;
        g_armed.store(true, std::memory_order_relaxed);
    }
}

void Reporter(u32 seconds) {
    struct Snap {
        u64 frames, seen, fresh, stale, first, empty, resets, pool_resets, reserves;
    };
    const auto take = [] {
        return Snap{g_count.frames.load(),      g_count.slots_seen.load(),      g_count.slots_fresh.load(),
                    g_count.slots_stale.load(), g_count.slots_first_use.load(), g_count.slots_empty.load(),
                    g_count.resets.load(),      g_count.resets_pool.load(),     g_count.reserves.load()};
    };
    Snap prev = take();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        const Snap s = take();
        std::string callers;
        {
            std::lock_guard guard(g_callers_lock);
            for (const auto& [rva, n] : g_reset_callers) {
                callers += fmt::format(" {:#x}x{}", rva, n);
            }
        }
        SSPC_LOG(Engine, Info,
                 "render trace/{}s: frames {}, pool slots {} (fresh {}, stale {}, empty {}, first use {}), max chunks "
                 "{}, Reset {} (pool {}; callers since start:{}), DCB reserve {}, CCB used {}, samples {}",
                 seconds, s.frames - prev.frames, s.seen - prev.seen, s.fresh - prev.fresh, s.stale - prev.stale,
                 s.empty - prev.empty, s.first - prev.first, g_count.max_chunks.load(), s.resets - prev.resets,
                 s.pool_resets - prev.pool_resets, callers.empty() ? " none" : callers, s.reserves - prev.reserves,
                 g_count.ccb_used.load(), g_count.samples.load());
        prev = s;
    }
}

std::string Env(const char* name) {
    char v[512] = {};
    const DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
    return n > 0 && n < sizeof(v) ? std::string(v, n) : std::string();
}

// --- P5.4 frame lifecycle (SSPC_P54_FRAME_TRACE=1) -------------------------------------------------------------------
// The engine's wait for its async compute queue, 0x1695e0 (the main loop 0x29c8d0 calls it twice a frame, through
// 0x3edef0 and 0x3ee470): `target = q[+0x1b88]; while (labels[q[+0x1b8c]] < target) delay(50)` with the label array
// re-read on every pass. Its first 20 bytes hold a RIP-relative LEA (no entry probe), so it is replaced by the same loop,
// which also records the wait.
constexpr GuestFunction kComputeWait{0x1695e0, 0x4b, 0x8f127063248d809eull};
constexpr GuestFunction kDelay{0x53b2b0, 0xa7, 0x89bfe4b7f4c237fbull}; // delay(us): rdtsc spin, up to 500 us

using DelayFn = SSPC_SYSV_ABI void (*)(u32 microseconds);
DelayFn g_delay = nullptr;

template <typename T>
T Volatile(u64 address) {
    return *reinterpret_cast<const volatile T*>(address);
}

// REA-3 (SSPC_REA_DEFER_WAIT_B=1): the main loop's call 0x29cd56 of 0x3ee470, whose tail call is this wait
constexpr u64 kWaitBReturn = 0x29cd5b;
bool g_defer_b = false;
std::atomic<u64> g_defer_b_count{0}, g_defer_b_met{0};

// REA-3 safe reset (with REA-3 unless SSPC_R9_REA3_SAFE_RESET=0, which keeps the unsafe deferral for A/B). The frame's
// tail 0x196c30 - SubmitFrame 0x1f1e30, the frame allocator's frees, then 0x3ed640 -> 0x169660 - starts the compute
// queue's command buffer anew (cur = mem, start = 0) and zeroes its position label. The buffer is not copied: each
// submission puts an INDIRECT_BUFFER to [mem + start, cur) into the ring (0x63d8c0), 0x3f3ed0's DMAs read their data
// from it, and the next frame's particle update 0x3ee7d0 rewrites it from offset 0 before that frame's first wait.
// With wait B deferred, what was submitted after wait A (0x3edef0's light DMAs) can still be in flight then: its
// commands overwritten, its late position write outliving the reset (0x169020 then reuses memory not read yet) - the
// type-0 packet on queue 1 and the hang after ~16 min. The barrier restores what wait B guaranteed, where the reset
// needs it: the counter label at the last submission's value and the position label at the last submission's end.
// 0x1691e0 writes ((cur - mem) / 4 + 0x16) & ~0xf with cur past its 7-dword counter RELEASE_MEM - after the 7-dword
// position packet and the 64-byte alignment, exactly the start it leaves. At SubmitFrame's return its wait for the
// previous frame's label is behind, so the GPU has usually finished.
constexpr GuestFunction kResetAssumed[] = {
    {0x196c30, 0x15a, 0x3208b3eb8cf47d35ull}, // the frame's tail: SubmitFrame, frees, then the reset 0x3ed640
    {0x3ed640, 0xc, 0xe56ac7198f4de952ull},   // -> 0x169660 on the compute queue
    {0x169660, 0x44, 0x7fadb3a5dafa9a40ull},  // the reset
    {0x1691e0, 0x2fe, 0xb1a4bc684bc8e25full}, // the submission: counter and position RELEASE_MEMs, INDIRECT_BUFFER
    {0x169020, 0x162, 0xccb01dd6297693f6ull}, // the overflow handler: reuses the space below the position label
};
constexpr u32 kPositionGrace = 40; ///< delays of 50 us past the counter (the position write is the next fence)
bool g_safe_reset = false;
// (the main thread's: wait B deferred in this frame - the barrier only completes such a wait, so loading screens and a
// queue being torn down, whose SubmitFrame has no deferred wait, are left as they were)
bool g_reset_pending = false;
u32 g_reset_target = 0;
std::atomic<u64> g_reset_frames{0}, g_reset_waited{0}, g_reset_delays_max{0}, g_reset_position_late{0},
    g_reset_submitted_after{0};

SSPC_SYSV_ABI void ComputeWaitHook(u8* queue) {
    const u64 q = reinterpret_cast<u64>(queue);
    const u32 target = Volatile<u32>(q + L::kQueueSubmitted);
    if (g_defer_b && reinterpret_cast<u64>(__builtin_return_address(0)) == g_base + kWaitBReturn) {
        const s32 index = Volatile<s32>(q + L::kQueueLabelIndex);
        const u64 label = Volatile<u64>(g_base + L::kLabelArray) + static_cast<u64>(static_cast<s64>(index)) * 8;
        if (Volatile<u32>(label) >= target) {
            g_defer_b_met.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        Gpu::Gnm::SetGraphicsGate(label, target);
        g_reset_pending = true;
        g_reset_target = target;
        const u64 n = g_defer_b_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1 || n == 100 || n == 1000 || n == 10000) {
            SSPC_LOG(Engine, Info, "REA-3: {} second compute waits handed to the processor as a graphics gate ({} already "
                     "met); label {:#x} >= {}", n, g_defer_b_met.load(std::memory_order_relaxed), label, target);
        }
        return;
    }
    const u64 t0 = Gpu::StallTrace::Now();
    u64 label = 0;
    u16 waited = 0;
    for (;;) {
        const s32 index = Volatile<s32>(q + L::kQueueLabelIndex);
        label = Volatile<u64>(g_base + L::kLabelArray) + static_cast<u64>(static_cast<s64>(index)) * 8;
        if (Volatile<u32>(label) >= target) {
            break;
        }
        if (waited == 0 && g_critical) {
            Gpu::CriticalCompute::Begin(); // (CC-2: the processor serves the compute queues first meanwhile)
        }
        waited = 1;
        if (g_label_wait) {
            Gpu::LabelWait::Sleep(label, target, g_label_spin_us); // (R11: asleep until the fence write lands)
        } else {
            g_delay(50);
        }
    }
    if (g_label_wait && waited != 0) {
        const u64 n = g_label_waits.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1 || n == 100 || n == 1000 || n == 10000 || n % 100000 == 0) {
            const Gpu::LabelWait::Totals t = Gpu::LabelWait::Read();
            SSPC_LOG(Engine, Info, "R11 label wait: {} blocked compute waits; sleeps {}, met while spinning {}, ended by "
                     "the 1 ms timeout {}, woken by a fence write {}", n, t.sleeps, t.spins_met, t.timeouts, t.wakes);
        }
    }
    if (waited != 0 && g_critical) {
        Gpu::CriticalCompute::End();
    }
    Gpu::StallTrace::Span(Gpu::StallTrace::Ev::GameComputeWait, t0, Gpu::StallTrace::Now(), label, target, waited);
}

/// REA-3 safe reset (see kResetAssumed): on the game thread at SubmitFrame's return, before 0x196c30 resets the compute
/// queue's buffer, when this frame's wait B was deferred. Its span, when it waited: GameComputeWait with c = 2. With
/// SSPC_R9_REA3_SAFE_RESET=0 it only counts the frames whose reset comes while the queue's work is in flight.
void ComputeResetBarrier() {
    if (!g_reset_pending) {
        return;
    }
    g_reset_pending = false;
    const u64 q = g_base + L::kComputeQueue;
    const u32 target = Volatile<u32>(q + L::kQueueSubmitted);
    const u32 end = Volatile<u32>(q + L::kQueueCbStart);
    if (target != g_reset_target) {
        // (nothing between 0x3ee470 and the reset was seen submitting; the current value is waited for, it is the later)
        const u64 m = g_reset_submitted_after.fetch_add(1, std::memory_order_relaxed) + 1;
        if (m <= 5) {
            SSPC_LOG(Engine, Warning, "REA-3 safe reset: the compute queue's counter went from {} at wait B to {} at "
                     "SubmitFrame's return (#{})", g_reset_target, target, m);
        }
    }
    const auto label = [q](u64 index_field) {
        const s32 index = Volatile<s32>(q + index_field);
        return Volatile<u64>(g_base + L::kLabelArray) + static_cast<u64>(static_cast<s64>(index)) * 8;
    };
    const u64 n = g_reset_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!g_safe_reset) {
        if (Volatile<u32>(label(L::kQueueLabelIndex)) < target || Volatile<u32>(label(L::kQueueReadLabel)) != end) {
            g_reset_waited.fetch_add(1, std::memory_order_relaxed);
        }
        if (n == 1 || n == 100 || n == 1000 || n % 10000 == 0) {
            SSPC_LOG(Engine, Info, "REA-3 without its safe reset: {} frames, {} reset the compute buffer with its work "
                     "in flight; second waits {} as gates, {} already met", n,
                     g_reset_waited.load(std::memory_order_relaxed), g_defer_b_count.load(std::memory_order_relaxed),
                     g_defer_b_met.load(std::memory_order_relaxed));
        }
        return;
    }
    const u64 t0 = Gpu::StallTrace::Now();
    u64 delays = 0;
    while (Volatile<u32>(label(L::kQueueLabelIndex)) < target) {
        g_delay(50);
        ++delays;
    }
    u32 grace = 0;
    while (Volatile<u32>(label(L::kQueueReadLabel)) != end && grace < kPositionGrace) {
        g_delay(50);
        ++grace;
    }
    if (grace == kPositionGrace) {
        // (the counter came, the position did not: the reset goes on as the game's own code would)
        const u64 late = g_reset_position_late.fetch_add(1, std::memory_order_relaxed) + 1;
        if (late <= 5) {
            SSPC_LOG(Engine, Error, "REA-3 safe reset: frame {}: the position label {:#x} holds {:#x}, not the end {:#x} "
                     "of the last submission ({} us after its counter {} came): reset without it (#{})", n,
                     label(L::kQueueReadLabel), Volatile<u32>(label(L::kQueueReadLabel)), end, kPositionGrace * 50,
                     target, late);
        }
    }
    delays += grace;
    if (delays != 0) {
        g_reset_waited.fetch_add(1, std::memory_order_relaxed);
        u64 max = g_reset_delays_max.load(std::memory_order_relaxed);
        while (delays > max && !g_reset_delays_max.compare_exchange_weak(max, delays, std::memory_order_relaxed)) {
        }
        Gpu::StallTrace::Span(Gpu::StallTrace::Ev::GameComputeWait, t0, Gpu::StallTrace::Now(),
                              label(L::kQueueLabelIndex), target, 2);
    }
    if (n == 1 || n == 100 || n == 1000 || n % 10000 == 0) {
        SSPC_LOG(Engine, Info, "REA-3 safe reset: {} frames, {} waited for the compute queue before its buffer reset "
                 "(at most {} x 50 us), position late {}, submitted after wait B {}; second waits {} as gates, {} "
                 "already met", n, g_reset_waited.load(std::memory_order_relaxed),
                 g_reset_delays_max.load(std::memory_order_relaxed),
                 g_reset_position_late.load(std::memory_order_relaxed),
                 g_reset_submitted_after.load(std::memory_order_relaxed),
                 g_defer_b_count.load(std::memory_order_relaxed), g_defer_b_met.load(std::memory_order_relaxed));
    }
}

/// SubmitFrame with its span: a = the frame's label address, b = its fence after the call, c = the flags.
void SubmitFrameTimed(u8* frame, u32 flags) {
    const u64 t0 = Gpu::StallTrace::Now();
    g_submit(frame, flags);
    const u64 f = reinterpret_cast<u64>(frame);
    const u64 label = At<u64>(g_base + L::kLabelArray) +
                      static_cast<u64>(static_cast<s64>(At<s32>(f + L::kFrameLabelIndex))) * 8;
    Gpu::StallTrace::Span(Gpu::StallTrace::Ev::GameSubmitFrame, t0, Gpu::StallTrace::Now(), label,
                          At<u32>(f + L::kFrameCounter), static_cast<u16>(flags));
    if (g_defer_b) {
        ComputeResetBarrier();
    }
}

SSPC_SYSV_ABI void LifecycleSubmitProbe(u8* frame, u32 flags) {
    SubmitFrameTimed(frame, flags);
}

u32 EnvU32(const char* name, u32 fallback) {
    const std::string v = Env(name);
    return v.empty() ? fallback : static_cast<u32>(std::strtoul(v.c_str(), nullptr, 10));
}

// --- P5.4 early frame submission (SSPC_P54_EARLY_SUBMIT=1) -----------------------------------------------------------
// SubmitFrame (pinned by kSubmitFrameSite's hash) with its list complete (0x1f26d5): step 4 `if (mode == 0)
// sceGnmSubmitDone()` (call 0x1f26e2, return 0x1f26e7); step 5 `while (labels[frame+0x124] < frame+0x130) delay(50)`;
// 0x28eb20 (eboot data and a condition variable, no GPU memory); step 6 when flags != 0
// sceGnmSubmitAndFlipCommandBuffers(count [rbp-0x38], dcbs rbp-0x1838, sizes rbp-0x838, 0, 0, frame+0x14,
// frame+0x20 * 2 + frame+0x17dc, 1, (s32) [0x1d20384]); nothing writes the list or those fields in between. Its frame
// (push rbp; mov rbp, rsp; five pushes; sub rsp, 0x1888; no other stack adjustment): at the call rbp = the slot of the
// return address + 0x18b8; flags at rbp-0x187c, frame at rbp-0x1870.
constexpr u64 kStep4Return = 0x1f26e7;
constexpr u64 kStep4Rbp = 0x18b8; ///< rbp - the return address' slot
constexpr u64 kListCount = 0x38, kListDcbs = 0x1838, kListSizes = 0x838, kFlags = 0x187c, kFrameArg = 0x1870;
constexpr u32 kListMax = 512; ///< the stack arrays' size

std::atomic<u64> g_early_foreign{0}; ///< sceGnmSubmitDone calls from elsewhere (they take the submission lock)
std::atomic<bool> g_early_refused_logged{false};

bool EarlyFrameSource(const u64* return_slot, Gpu::Gnm::EarlyFrame& out) {
    const u64 ret = *return_slot;
    if (ret != g_base + kStep4Return) {
        // (logged at the 1st, 10th, 100th, ... such call: whether they come during play, when frames are in flight)
        const u64 n = g_early_foreign.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1 || n == 10 || n == 100 || n == 1000 || n == 10000 || n == 100000) {
            SSPC_LOG(Engine, Info, "early frame submission: sceGnmSubmitDone called from {} (not SubmitFrame's step 4 "
                     "{:#x}), call {} from elsewhere: hands on nothing, takes the submission lock as before",
                     Hex(ret - g_base), kStep4Return, n);
        }
        return false;
    }
    const u64 rbp = reinterpret_cast<u64>(return_slot) + kStep4Rbp;
    if (At<u32>(rbp - kFlags) == 0) {
        return false; // no flip: sceGnmSubmitCommandBuffers at step 6, as before
    }
    const u32 count = At<u32>(rbp - kListCount);
    const u64 frame = At<u64>(rbp - kFrameArg);
    if (count == 0 || count > kListMax || frame != Frame() || At<s32>(g_base + L::kSubmitMode) != 0) {
        if (!g_early_refused_logged.exchange(true, std::memory_order_relaxed)) {
            SSPC_LOG(Engine, Error, "early frame submission: SubmitFrame's step 4 with {} DCBs, frame {} (expected "
                     "1..{}, {}), mode {}: left to its own call (logged once)", count, Hex(frame), kListMax,
                     Hex(Frame()), At<s32>(g_base + L::kSubmitMode));
        }
        return false;
    }
    out.count = count;
    out.dcbs = reinterpret_cast<u32* const*>(rbp - kListDcbs);
    out.dcb_sizes = reinterpret_cast<const u32*>(rbp - kListSizes);
    out.vo_handle = At<u32>(frame + L::kFrameVideoOut);
    out.buffer_index = static_cast<u32>(At<s32>(frame + L::kFrameDisplayPair) * 2 + At<s32>(frame + L::kFrameDisplayHalf));
    out.flip_mode = 1;
    out.flip_arg = At<s32>(g_base + L::kSubmitFlipArg);
    return true;
}

} // namespace

bool InstallFromEnvironment(u64 eboot_base) {
    if (Env("SSPC_P53_RENDER_TRACE") != "1") {
        return true;
    }
    g_options.every = EnvU32("SSPC_P53_RENDER_TRACE_EVERY", g_options.every);
    g_options.dumps = EnvU32("SSPC_P53_RENDER_TRACE_DUMPS", g_options.dumps);
    g_options.stats_s = EnvU32("SSPC_P53_RENDER_TRACE_STATS", g_options.stats_s);
    if (const std::string d = Env("SSPC_P53_RENDER_TRACE_DIR"); !d.empty()) {
        g_options.dir = d;
    }

    std::string error;
    for (const GuestFunction& f : kAssumed) {
        if (!VerifyGuestFunction(eboot_base, f, error)) {
            SSPC_LOG(Engine, Error, "render trace not installed: {}", error);
            return false;
        }
    }
    std::vector<const ProbeSite*> sites = {&kSubmitFrameSite, &kResetSite, &kReserveSite};
    for (const ProbeSite& s : kSetterSites) {
        sites.push_back(&s);
    }
    for (const ProbeSite* s : sites) {
        if (!VerifyGuestFunction(eboot_base, s->function, error)) {
            SSPC_LOG(Engine, Error, "render trace not installed: {}", error);
            return false;
        }
    }

    g_base = eboot_base;
    const auto install = [&](const ProbeSite& site, const void* target) {
        void* original = InstallEntryProbe(eboot_base, site, target, error);
        if (original == nullptr) {
            // Verified bytes, but the host refused to write: nothing ran yet, earlier probes may be in - stop rather
            // than run half-probed (as the native job queue does).
            SSPC_LOG(Engine, Critical, "render trace: {}", error);
            std::abort();
        }
        return original;
    };
    g_submit = reinterpret_cast<SubmitFrameFn>(install(kSubmitFrameSite, reinterpret_cast<const void*>(&SubmitFrameProbe)));
    g_reset = reinterpret_cast<ResetFn>(install(kResetSite, reinterpret_cast<const void*>(&ResetProbe)));
    g_reserve = reinterpret_cast<ReserveFn>(install(kReserveSite, reinterpret_cast<const void*>(&ReserveProbe)));
    g_setters[0] = reinterpret_cast<SetterFn>(install(kSetterSites[0], reinterpret_cast<const void*>(&SetterProbe<0>)));
    g_setters[1] = reinterpret_cast<SetterFn>(install(kSetterSites[1], reinterpret_cast<const void*>(&SetterProbe<1>)));
    g_setters[2] = reinterpret_cast<SetterFn>(install(kSetterSites[2], reinterpret_cast<const void*>(&SetterProbe<2>)));
    g_setters[3] = reinterpret_cast<SetterFn>(install(kSetterSites[3], reinterpret_cast<const void*>(&SetterProbe<3>)));

    SSPC_LOG(Engine, Warning,
             "DIAGNOSTICS: SSPC_P53_RENDER_TRACE=1 - entry probes on SubmitFrame 0x1f1e30, Reset 0x11f050, DCB reserve "
             "0x11eed0 and the CUE setters 0x16d10/0x16de0/0x16b70/0x16c40 (read-only); a frame every {} written to {} "
             "(at most {})",
             g_options.every, g_options.dir.string(), g_options.dumps);
    if (g_options.stats_s != 0) {
        std::thread(Reporter, g_options.stats_s).detach();
    }
    return true;
}

bool InstallLifecycleFromEnvironment(u64 eboot_base) {
    if (Env("SSPC_P54_FRAME_TRACE") != "1") {
        return true;
    }
    std::string error;
    for (const GuestFunction& f : {kComputeWait, kDelay, kSubmitFrameSite.function}) {
        if (!VerifyGuestFunction(eboot_base, f, error)) {
            SSPC_LOG(Engine, Error, "frame lifecycle trace not installed: {}", error);
            return false;
        }
    }
    g_base = eboot_base;
    g_delay = reinterpret_cast<DelayFn>(eboot_base + kDelay.rva);
    if (g_submit == nullptr) { // (the render trace's probe on SubmitFrame records the span when it is in)
        void* original =
            InstallEntryProbe(eboot_base, kSubmitFrameSite, reinterpret_cast<const void*>(&LifecycleSubmitProbe), error);
        if (original == nullptr) {
            SSPC_LOG(Engine, Critical, "frame lifecycle trace: {}", error); // (as the render trace: never half-probed)
            std::abort();
        }
        g_submit = reinterpret_cast<SubmitFrameFn>(original);
    }
    g_lifecycle = true;
    if (!InstallJump(eboot_base + kComputeWait.rva, reinterpret_cast<const void*>(&ComputeWaitHook), error)) {
        SSPC_LOG(Engine, Critical, "frame lifecycle trace: {}", error);
        std::abort();
    }
    g_wait_hooked = true;
    SSPC_LOG(Engine, Warning,
             "DIAGNOSTICS: SSPC_P54_FRAME_TRACE=1 - SubmitFrame 0x1f1e30 probed, the async compute wait 0x1695e0 replaced "
             "by the same loop with a span; the events go to the stall trace (SSPC_P412_STALL_TRACE=deep,...)");
    return true;
}

bool InstallCriticalComputeFromEnvironment(u64 eboot_base) {
    // (FrameBreak FP-1's SSPC_FP_WHEN=wait reads the same count: the hook without CC-2's priority)
    // (FrameBreak AQ's batch split while the game waits - SSPC_FB_ASYNC_COMPUTE=1 - reads it too)
    const bool cc2 = Env("SSPC_CC_CRITICAL") == "1";
    if (!cc2 && Env("SSPC_FP_WHEN") != "wait" && Env("SSPC_FB_ASYNC_COMPUTE") != "1" &&
        Env("SSPC_CP_ROUND_NODRAIN") != "2") {
        return true;
    }
    if (!g_wait_hooked) {
        std::string error;
        for (const GuestFunction& f : {kComputeWait, kDelay}) {
            if (!VerifyGuestFunction(eboot_base, f, error)) {
                SSPC_LOG(Engine, Error, "critical compute (CC-2) not installed: {}", error);
                return false;
            }
        }
        g_base = eboot_base;
        g_delay = reinterpret_cast<DelayFn>(eboot_base + kDelay.rva);
        if (!InstallJump(eboot_base + kComputeWait.rva, reinterpret_cast<const void*>(&ComputeWaitHook), error)) {
            SSPC_LOG(Engine, Critical, "critical compute (CC-2): {}", error);
            std::abort();
        }
        g_wait_hooked = true;
    }
    g_critical = true;
    SSPC_LOG(Engine, Warning,
             "EXPERIMENT: {} - the async compute wait 0x1695e0 replaced by the same loop; the game's blocked waits "
             "counted for the command processor", cc2 ? "SSPC_CC_CRITICAL=1" : Env("SSPC_FP_WHEN") == "wait" ? "SSPC_FP_WHEN=wait"
             : Env("SSPC_CP_ROUND_NODRAIN") == "2" ? "SSPC_CP_ROUND_NODRAIN=2" : "SSPC_FB_ASYNC_COMPUTE=1");
    return true;
}

bool InstallDeferComputeWaitFromEnvironment(u64 eboot_base) {
    if (Env("SSPC_REA_DEFER_WAIT_B") != "1") {
        return true;
    }
    const bool safe_reset = Env("SSPC_R9_REA3_SAFE_RESET") != "0";
    std::string error;
    // (the safe reset - or, without it, its count - reads the queue's fields at SubmitFrame's return: its probe goes in
    // here when no trace put it in)
    for (const GuestFunction& f : kResetAssumed) {
        if (!VerifyGuestFunction(eboot_base, f, error)) {
            SSPC_LOG(Engine, Error, "REA-3 deferred compute wait not installed (its safe reset): {}", error);
            return false;
        }
    }
    if (g_submit == nullptr && !VerifyGuestFunction(eboot_base, kSubmitFrameSite.function, error)) {
        SSPC_LOG(Engine, Error, "REA-3 deferred compute wait not installed (its safe reset): {}", error);
        return false;
    }
    if (!g_wait_hooked) {
        for (const GuestFunction& f : {kComputeWait, kDelay}) {
            if (!VerifyGuestFunction(eboot_base, f, error)) {
                SSPC_LOG(Engine, Error, "REA-3 deferred compute wait not installed: {}", error);
                return false;
            }
        }
        g_base = eboot_base;
        g_delay = reinterpret_cast<DelayFn>(eboot_base + kDelay.rva);
        if (!InstallJump(eboot_base + kComputeWait.rva, reinterpret_cast<const void*>(&ComputeWaitHook), error)) {
            SSPC_LOG(Engine, Critical, "REA-3 deferred compute wait: {}", error);
            std::abort();
        }
        g_wait_hooked = true;
    }
    if (g_submit == nullptr) {
        void* original =
            InstallEntryProbe(eboot_base, kSubmitFrameSite, reinterpret_cast<const void*>(&LifecycleSubmitProbe), error);
        if (original == nullptr) {
            SSPC_LOG(Engine, Critical, "REA-3 deferred compute wait: {}", error); // (as the traces: never half-probed)
            std::abort();
        }
        g_submit = reinterpret_cast<SubmitFrameFn>(original);
    }
    g_defer_b = true;
    g_safe_reset = safe_reset;
    SSPC_LOG(Engine, Warning,
             "EXPERIMENT: SSPC_REA_DEFER_WAIT_B=1 - the frame's second async compute wait (0x3ee470 -> 0x1695e0) does not "
             "block the game: the next graphics submission waits for the label on the GPU timeline instead");
    if (safe_reset) {
        SSPC_LOG(Engine, Warning,
                 "EXPERIMENT: REA-3 safe reset - at SubmitFrame 0x1f1e30's return the game waits until the compute "
                 "queue finished its last submission (counter and position labels) before 0x196c30 resets its buffer");
    } else {
        SSPC_LOG(Engine, Warning,
                 "EXPERIMENT: SSPC_R9_REA3_SAFE_RESET=0 - REA-3 without its safe reset: the compute buffer may be reset "
                 "while its work is in flight (the hang after ~16 min; for A/B only)");
    }
    return true;
}

bool InstallLabelWaitFromEnvironment(u64 eboot_base) {
    if (Env("SSPC_R11_LABEL_WAIT") != "1") {
        return true;
    }
    std::string error;
    if (!g_wait_hooked) {
        for (const GuestFunction& f : {kComputeWait, kDelay}) {
            if (!VerifyGuestFunction(eboot_base, f, error)) {
                SSPC_LOG(Engine, Error, "R11 label wait not installed: {}", error);
                return false;
            }
        }
        g_base = eboot_base;
        g_delay = reinterpret_cast<DelayFn>(eboot_base + kDelay.rva);
        if (!InstallJump(eboot_base + kComputeWait.rva, reinterpret_cast<const void*>(&ComputeWaitHook), error)) {
            SSPC_LOG(Engine, Critical, "R11 label wait: {}", error);
            std::abort();
        }
        g_wait_hooked = true;
    }
    g_label_spin_us = std::min<u32>(EnvU32("SSPC_R11_LABEL_WAIT_SPIN_US", 20), 1000);
    g_label_wait = true;
    Gpu::LabelWait::g_on.store(true, std::memory_order_release);
    SSPC_LOG(Engine, Warning,
             "EXPERIMENT: SSPC_R11_LABEL_WAIT=1 - the async compute wait 0x1695e0 replaced by the same loop that spins "
             "{} us with PAUSE, then sleeps on its label (WaitOnAddress, woken by the command processor's fence write, "
             "1 ms at most) instead of the game's rdtsc spin delay(50)", g_label_spin_us);
    return true;
}

bool InstallEarlySubmitFromEnvironment(u64 eboot_base) {
    if (Env("SSPC_P54_EARLY_SUBMIT") != "1") {
        return true;
    }
    // (an entry probe on SubmitFrame - the render or the lifecycle trace - was installed on the verified function and
    // changed its first bytes: the body the source reads is the one verified then)
    std::string error;
    if (g_submit == nullptr && !VerifyGuestFunction(eboot_base, kSubmitFrameSite.function, error)) {
        SSPC_LOG(Engine, Error, "early frame submission not installed: {}", error);
        return false;
    }
    g_base = eboot_base;
    Gpu::Gnm::SetEarlyFrameSource(&EarlyFrameSource);
    SSPC_LOG(Engine, Warning,
             "EXPERIMENT: SSPC_P54_EARLY_SUBMIT=1 - SubmitFrame 0x1f1e30's frame is handed on at its sceGnmSubmitDone "
             "(step 4), before its wait for the previous frame's label; its sceGnmSubmitAndFlipCommandBuffers (step 6) "
             "returns that result; no submission lock");
    return true;
}

} // namespace SecondSon::Engine::Render::Trace
