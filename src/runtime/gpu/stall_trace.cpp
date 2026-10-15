// SecondSonPC — the stall trace (P4.12). See stall_trace.h.
#include "runtime/gpu/stall_trace.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "base/log.h"
#include "runtime/gpu/frame_log.h"
#include "runtime/gpu/shader_trace.h"
#include "runtime/kernel/clock.h"

namespace SecondSon::Gpu::StallTrace {

namespace detail {
std::atomic<u32> g_mode{0};
} // namespace detail

namespace {

constexpr u32 kFrames = 8192;   ///< per-frame totals kept (a ring; 4.5 min at 30 FPS)
constexpr u32 kBuckets = 256;   ///< log histogram: 8 buckets per octave of TSC ticks from 16
constexpr u32 kSampleDepth = 8; ///< frames unwound per stack sample

struct Event {
    u64 t0, t1, a;
    u32 b;
    u16 type, c;
};
static_assert(sizeof(Event) == 32);

struct TypeAgg {
    u64 count = 0, ticks = 0, max = 0;
    std::array<u32, kBuckets> hist{};
};

struct ThreadLog {
    Role role = Role::Other;
    DWORD tid = 0;
    HANDLE handle = nullptr; ///< threads 1 and 2: suspend/context rights for the sampler
    char name[64] = {};
    std::array<TypeAgg, kEvents> agg{};
    std::unique_ptr<u16[]> fcount; ///< [frame % kFrames][event]
    std::unique_ptr<u64[]> fticks;
    std::unique_ptr<u32[]> fframe; ///< the frame each per-frame row holds now (a row is reset when reused)
    // deep
    std::unique_ptr<Event[]> ev;
    u64 cap = 0;
    std::atomic<u64> n{0};
    u64 dropped = 0;
};

struct GpuBatchRec {
    u64 value, submit_t0, submit_t1, gpu_t0, gpu_t1;
    u32 frame, pad;
};
static_assert(sizeof(GpuBatchRec) == 48);

struct CalRec {
    u64 tsc_before, tsc_after, gpu, qpc, dev_ns, qpc2, tsc2;
};

struct Sample {
    u64 tsc;
    u32 tid;
    u8 role, n;
    u16 pad;
    u64 pcs[kSampleDepth];
};
static_assert(sizeof(Sample) == 80);

struct Options {
    double win_a = 25.0, win_b = 37.0;
    u64 events = 6u << 20;
    u32 sample_hz = 1000;
    bool records = true;
};

struct State {
    Options opt;
    std::filesystem::path dir;
    double tsc_hz = 0.0;
    double ts_period = 0.0;
    u64 start_tsc = 0;
    u64 win0 = ~0ull, win1 = 0; ///< deep window (TSC)
    std::mutex logs_mutex;
    std::vector<ThreadLog*> logs; ///< never freed (a game thread may still record after the stop)
    std::atomic<u32> frame{0};
    // GPU batches (the polling thread only)
    std::vector<GpuBatchRec> batches;
    u64 batches_dropped = 0;
    u64 gpu_last_end = 0;     ///< GPU ticks: the latest batch end seen
    u32 gpu_last_frame = ~0u;
    std::unique_ptr<u64[]> gpu_gap_ticks, gpu_gap_first_ticks, gpu_busy_ticks; ///< per frame (GPU ticks)
    std::unique_ptr<u32[]> gpu_batches, gpu_frame_tag;
    u64 gpu_timed = 0, gpu_untimed = 0;
    // calibration (the polling thread only)
    CalibrateFn cal_fn = nullptr;
    void* cal_ctx = nullptr;
    std::vector<CalRec> cal;
    u64 cal_next = 0;
    // samples
    std::vector<Sample> samples;
    std::atomic<u64> samples_n{0};
    u64 samples_failed = 0;
    /// (review MEDIUM-3: never a joinable std::thread with static storage - an exit that skips Stop() must not
    /// terminate the process)
    std::thread* sampler = nullptr;
    std::atomic<bool> sampler_stop{false};
    bool stopped = false;
};

State g;
thread_local ThreadLog* t_log = nullptr;

u32 Bucket(u64 ticks) {
    if (ticks < 16) {
        return 0;
    }
    const u32 o = 63 - static_cast<u32>(std::countl_zero(ticks)); // >= 4
    const u32 sub = static_cast<u32>(ticks >> (o - 3)) & 7;
    return std::min<u32>((o - 4) * 8 + sub + 1, kBuckets - 1);
}
/// A bucket's middle (ticks).
double BucketMid(u32 i) {
    if (i == 0) {
        return 8.0;
    }
    const u32 o = (i - 1) / 8 + 4, sub = (i - 1) % 8;
    const double lo = static_cast<double>(u64{8 + sub} << (o - 3));
    return lo + static_cast<double>(u64{1} << (o - 3)) / 2.0;
}

ThreadLog* MakeLog(Role role) {
    auto* l = new ThreadLog();
    l->role = role;
    l->tid = GetCurrentThreadId();
    PWSTR desc = nullptr;
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &desc)) && desc != nullptr) {
        WideCharToMultiByte(CP_UTF8, 0, desc, -1, l->name, sizeof(l->name) - 1, nullptr, nullptr);
        LocalFree(desc);
    }
    l->fcount.reset(new u16[size_t{kFrames} * kEvents]());
    l->fticks.reset(new u64[size_t{kFrames} * kEvents]());
    l->fframe.reset(new u32[kFrames]);
    std::fill_n(l->fframe.get(), kFrames, ~0u);
    if ((detail::g_mode.load(std::memory_order_relaxed) & 3) == 2) {
        l->cap = role == Role::Record ? g.opt.events : role == Role::Cp ? std::max<u64>(g.opt.events / 4, 1u << 18)
                                                                        : u64{1} << 18;
        l->ev.reset(new Event[l->cap]); // (not zeroed: pages are touched as events land)
    }
    if (role == Role::Cp || role == Role::Record) {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &l->handle,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0);
    }
    std::scoped_lock lock{g.logs_mutex};
    g.logs.push_back(l);
    return l;
}

ThreadLog* Log(Ev e) {
    if (t_log == nullptr) {
        const bool game = e == Ev::GameSubmitLock || e == Ev::GameIdleWait || e == Ev::GameSubmitFlip ||
                          e == Ev::GameSubmitFrame || e == Ev::GameComputeWait || e == Ev::GameSubmitDone ||
                          e == Ev::GameDingDong || e == Ev::GameEarlySubmit;
        t_log = MakeLog(game ? Role::Game : Role::Other);
    }
    return t_log;
}

bool InWindow(u64 t0, u64 t1) {
    return t1 >= g.win0 && t0 <= g.win1;
}

void SamplerRun() {
    SetThreadDescription(GetCurrentThread(), L"SecondSonPC:P412Sampler");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    const HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const LONGLONG period_100ns = std::max<LONGLONG>(10'000'000 / std::max<u32>(g.opt.sample_hz, 1), 1000);
    std::vector<std::pair<HANDLE, std::pair<DWORD, Role>>> targets;
    targets.reserve(16);
    while (!g.sampler_stop.load(std::memory_order_acquire)) {
        if (timer != nullptr) {
            LARGE_INTEGER due{};
            due.QuadPart = -period_100ns;
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 100);
        } else {
            Sleep(1);
        }
        const u64 now = __rdtsc();
        if (now < g.win0) {
            continue;
        }
        if (now > g.win1) {
            break;
        }
        {
            std::scoped_lock lock{g.logs_mutex};
            targets.clear();
            for (ThreadLog* l : g.logs) {
                if (l->handle != nullptr) {
                    targets.push_back({l->handle, {l->tid, l->role}});
                }
            }
        }
        for (const auto& [h, who] : targets) {
            const u64 i = g.samples_n.load(std::memory_order_relaxed);
            if (i >= g.samples.size()) {
                break;
            }
            const u64 suspend_t0 = __rdtsc();
            if (SuspendThread(h) == static_cast<DWORD>(-1)) {
                ++g.samples_failed;
                continue;
            }
            // nothing below allocates, logs or takes a lock the target may hold (as CpuSampler)
            Sample& s = g.samples[i];
            s.tsc = __rdtsc();
            s.tid = who.first;
            s.role = static_cast<u8>(who.second);
            s.n = 0;
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (GetThreadContext(h, &ctx)) {
                for (; s.n < kSampleDepth && ctx.Rip != 0; ++s.n) {
                    s.pcs[s.n] = ctx.Rip;
                    DWORD64 image_base = 0;
                    PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
                    if (fe == nullptr) {
                        DWORD64 ret = 0;
                        SIZE_T got = 0;
                        if (ctx.Rsp == 0 ||
                            !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(ctx.Rsp), &ret, 8, &got) ||
                            got != 8) {
                            ++s.n;
                            break;
                        }
                        ctx.Rip = ret;
                        ctx.Rsp += 8;
                    } else {
                        PVOID handler_data = nullptr;
                        DWORD64 establisher = 0;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fe, &ctx, &handler_data, &establisher,
                                         nullptr);
                    }
                }
            }
            ResumeThread(h);
            // (review MEDIUM-4) how long the sampler held the target: the analysis reports the stall it adds
            s.pad = static_cast<u16>(std::min(static_cast<double>(__rdtsc() - suspend_t0) * 1e6 / g.tsc_hz, 65535.0));
            if (s.n == 0) {
                ++g.samples_failed;
                continue;
            }
            g.samples_n.store(i + 1, std::memory_order_release);
        }
    }
    if (timer != nullptr) {
        CloseHandle(timer);
    }
}

double Ms(u64 ticks) {
    return static_cast<double>(ticks) * 1000.0 / g.tsc_hz;
}
double Us(double ticks) {
    return ticks * 1e6 / g.tsc_hz;
}

void WriteLight() {
    std::string j = fmt::format("{{\n \"mode\": \"{}\", \"tsc_hz\": {:.0f}, \"start_tsc\": {}, \"frames_reached\": {},\n",
                                g.win1 != 0 ? "deep" : "light", g.tsc_hz, g.start_tsc, g.frame.load());
    j += fmt::format(" \"gpu\": {{\"batches_timed\": {}, \"batches_untimed\": {}, \"timestamp_period_ns\": {}}},\n",
                     g.gpu_timed, g.gpu_untimed, g.ts_period);
    j += " \"threads\": [\n";
    std::string csv = "n,role,tid,event,count,ms\n";
    std::vector<ThreadLog*> logs;
    {
        std::scoped_lock lock{g.logs_mutex};
        logs = g.logs;
    }
    for (size_t li = 0; li < logs.size(); ++li) {
        const ThreadLog& l = *logs[li];
        j += fmt::format("  {{\"role\": \"{}\", \"tid\": {}, \"name\": \"{}\", \"kept\": {}, \"dropped\": {}, \"events\": [",
                         RoleName(l.role), l.tid, l.name, l.n.load(), l.dropped);
        bool first = true;
        for (size_t e = 0; e < kEvents; ++e) {
            const TypeAgg& a = l.agg[e];
            if (a.count == 0) {
                continue;
            }
            double p[3] = {};
            const double q[3] = {0.5, 0.95, 0.99};
            for (int k = 0; k < 3; ++k) {
                const u64 target = std::max<u64>(static_cast<u64>(q[k] * static_cast<double>(a.count) + 0.5), 1);
                u64 cum = 0;
                for (u32 b = 0; b < kBuckets; ++b) {
                    cum += a.hist[b];
                    if (cum >= target) {
                        p[k] = Us(BucketMid(b));
                        break;
                    }
                }
            }
            j += fmt::format("{}\n   {{\"event\": \"{}\", \"count\": {}, \"total_ms\": {:.3f}, \"mean_us\": {:.2f}, "
                             "\"p50_us\": {:.2f}, \"p95_us\": {:.2f}, \"p99_us\": {:.2f}, \"max_us\": {:.1f}}}",
                             first ? "" : ",", EvName(static_cast<Ev>(e)), a.count, Ms(a.ticks),
                             Us(static_cast<double>(a.ticks)) / static_cast<double>(a.count), p[0], p[1], p[2],
                             Us(static_cast<double>(a.max)));
            first = false;
        }
        j += fmt::format("]}}{}\n", li + 1 < logs.size() ? "," : "");
        for (u32 f = 0; f < kFrames; ++f) {
            const u32 n = l.fframe[f];
            if (n == ~0u) {
                continue;
            }
            for (size_t e = 0; e < kEvents; ++e) {
                const size_t i = size_t{f} * kEvents + e;
                if (l.fcount[i] != 0 || l.fticks[i] != 0) {
                    csv += fmt::format("{},{},{},{},{},{:.4f}\n", n, RoleName(l.role), l.tid, EvName(static_cast<Ev>(e)),
                                       l.fcount[i], Ms(l.fticks[i]));
                }
            }
        }
    }
    j += " ]\n}\n";
    // the GPU's idle gaps between consecutive batches, per frame (the frame a batch was recorded in): before the frame's
    // first batch (the GPU waiting for the frame's start) and inside it
    if (g.gpu_frame_tag) {
        const double ms = g.ts_period / 1e6;
        for (u32 f = 0; f < kFrames; ++f) {
            const u32 n = g.gpu_frame_tag[f];
            if (n == ~0u) {
                continue;
            }
            csv += fmt::format("{},GPU,0,GpuBusy,{},{:.4f}\n", n, g.gpu_batches[f],
                               static_cast<double>(g.gpu_busy_ticks[f]) * ms);
            csv += fmt::format("{},GPU,0,GpuGapInFrame,0,{:.4f}\n", n, static_cast<double>(g.gpu_gap_ticks[f]) * ms);
            csv += fmt::format("{},GPU,0,GpuGapBeforeFrame,0,{:.4f}\n", n,
                               static_cast<double>(g.gpu_gap_first_ticks[f]) * ms);
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(g.dir, ec);
    std::ofstream(g.dir / "p412_light.json", std::ios::trunc) << j;
    std::ofstream(g.dir / "p412_frames.csv", std::ios::trunc) << csv;
}

template <class T>
void Put(std::ofstream& o, const T& v) {
    o.write(reinterpret_cast<const char*>(&v), sizeof(T));
}

void WriteDeep() {
    std::ofstream o(g.dir / "p412_trace.bin", std::ios::binary | std::ios::trunc);
    std::vector<ThreadLog*> logs;
    {
        std::scoped_lock lock{g.logs_mutex};
        logs = g.logs;
    }
    LARGE_INTEGER qf{};
    QueryPerformanceFrequency(&qf);
    // modules (the samples' addresses are symbolized offline)
    std::vector<std::pair<std::pair<u64, u64>, std::string>> modules;
    std::vector<HMODULE> mods(2048);
    DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods.data(), static_cast<DWORD>(mods.size() * sizeof(HMODULE)), &needed)) {
        const size_t count = std::min<size_t>(needed / sizeof(HMODULE), mods.size());
        for (size_t i = 0; i < count; ++i) {
            MODULEINFO mi{};
            char path[MAX_PATH] = {};
            if (GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi)) &&
                GetModuleFileNameA(mods[i], path, MAX_PATH) != 0) {
                modules.push_back({{reinterpret_cast<u64>(mi.lpBaseOfDll), mi.SizeOfImage}, path});
            }
        }
    }
    const u64 nsamples = g.samples_n.load(std::memory_order_acquire);
    o.write("SSPCP412", 8);
    Put(o, u32{1});
    Put(o, static_cast<u32>(logs.size()));
    Put(o, g.tsc_hz);
    Put(o, g.start_tsc);
    Put(o, g.win0);
    Put(o, g.win1);
    Put(o, g.ts_period);
    Put(o, static_cast<u64>(qf.QuadPart));
    Put(o, static_cast<u64>(g.batches.size()));
    Put(o, static_cast<u64>(g.cal.size()));
    Put(o, nsamples);
    Put(o, static_cast<u64>(modules.size()));
    Put(o, g.opt.sample_hz);
    Put(o, u32{g.opt.records ? 1u : 0u});
    for (const ThreadLog* l : logs) {
        Put(o, static_cast<u8>(l->role));
        o.write("\0\0\0", 3);
        Put(o, static_cast<u32>(l->tid));
        o.write(l->name, sizeof(l->name));
        const u64 n = std::min(l->n.load(std::memory_order_acquire), l->cap);
        Put(o, n);
        Put(o, l->dropped);
        if (n != 0) {
            o.write(reinterpret_cast<const char*>(l->ev.get()), static_cast<std::streamsize>(n * sizeof(Event)));
        }
    }
    o.write(reinterpret_cast<const char*>(g.batches.data()),
            static_cast<std::streamsize>(g.batches.size() * sizeof(GpuBatchRec)));
    o.write(reinterpret_cast<const char*>(g.cal.data()), static_cast<std::streamsize>(g.cal.size() * sizeof(CalRec)));
    o.write(reinterpret_cast<const char*>(g.samples.data()), static_cast<std::streamsize>(nsamples * sizeof(Sample)));
    for (const auto& [range, path] : modules) {
        Put(o, range.first);
        Put(o, range.second);
        char p[260] = {};
        std::memcpy(p, path.data(), std::min<size_t>(path.size(), sizeof(p) - 1));
        o.write(p, sizeof(p));
    }
}

} // namespace

const char* EvName(Ev e) {
    static constexpr const char* kNames[] = {
        "PreRound", "Poll", "IdleEmpty", "IdleGpu", "IdleLabel", "IdleFlag", "Drain", "QueueFull", "JournalFull",
        "FlipMarker", "GpuCompleted", "RecordWait", "Record", "GpuSync", "RingWait", "BindWait", "QueueLock", "Submit",
        "PipelineCreate", "ShaderTranslate", "FlipAcquire", "CopyFallback", "VblankWait", "NotReady", "Present",
        "PresentFence", "PresentAcquire", "PresentSubmit", "GameSubmitLock", "GameIdleWait", "GameSubmitFlip",
        "GameSubmitFrame", "GameComputeWait", "GameSubmitDone", "GameDingDong", "FenceHeld", "FenceWrite",
        "GameEarlySubmit", "CpQueueSlice",
    };
    static_assert(std::size(kNames) == kEvents);
    const size_t i = static_cast<size_t>(e);
    return i < kEvents ? kNames[i] : "?";
}

const char* RoleName(Role r) {
    static constexpr const char* kNames[] = {"CP", "Record", "Present", "Game", "Other"};
    const size_t i = static_cast<size_t>(r);
    return i < std::size(kNames) ? kNames[i] : "?";
}

bool Deep() {
    return (detail::g_mode.load(std::memory_order_relaxed) & 3) == 2;
}

bool DeepRecords() {
    return Deep() && g.opt.records;
}

void detail::Record(Ev e, u64 t0, u64 t1, u64 a, u32 b, u16 c) {
    const u32 mode = g_mode.load(std::memory_order_relaxed);
    if ((mode & kForward) != 0 && t1 >= t0 + ShaderTrace::WaitMinTicks() && e != Ev::Record &&
        e != Ev::PipelineCreate && e != Ev::ShaderTranslate && e != Ev::VblankWait) {
        // (P4.14: the shader stall trace's breakdown of long frames; translations and pipelines it traces itself, the
        // present thread's sleep to the next vblank is pacing, not a stall)
        ShaderTrace::Event(ShaderTrace::Kind::Wait, t0, t1, a, b, static_cast<u32>(e));
    }
    if ((mode & 3) == 0) {
        return;
    }
    ThreadLog* l = Log(e);
    const size_t ei = static_cast<size_t>(e);
    const u64 d = t1 >= t0 ? t1 - t0 : 0;
    TypeAgg& ag = l->agg[ei];
    ++ag.count;
    ag.ticks += d;
    ag.max = std::max(ag.max, d);
    ++ag.hist[Bucket(d)];
    const u32 frame = g.frame.load(std::memory_order_relaxed);
    const u32 slot = frame % kFrames;
    if (l->fframe[slot] != frame) {
        std::fill_n(l->fcount.get() + size_t{slot} * kEvents, kEvents, u16{0});
        std::fill_n(l->fticks.get() + size_t{slot} * kEvents, kEvents, u64{0});
        l->fframe[slot] = frame;
    }
    const size_t fi = size_t{slot} * kEvents + ei;
    l->fcount[fi] = static_cast<u16>(std::min<u32>(l->fcount[fi] + 1u, 0xFFFFu));
    l->fticks[fi] += d;
    if (l->cap != 0 && InWindow(t0, t1)) {
        const u64 n = l->n.load(std::memory_order_relaxed);
        if (n < l->cap) {
            l->ev[n] = Event{t0, t1, a, b, static_cast<u16>(e), c};
            l->n.store(n + 1, std::memory_order_release);
        } else {
            ++l->dropped;
        }
    }
}

void SpanEndingNow(Ev e, u64 ns, u64 a) {
    if ((detail::g_mode.load(std::memory_order_relaxed) & 3) != 0) {
        const u64 t1 = Now();
        const u64 ticks = static_cast<u64>(static_cast<double>(ns) * g.tsc_hz / 1e9);
        detail::Record(e, t1 - std::min(t1, ticks), t1, a, 0, 0);
    }
}

void RegisterThread(Role role) {
    if ((detail::g_mode.load(std::memory_order_relaxed) & 3) != 0 && t_log == nullptr) {
        t_log = MakeLog(role);
    }
}

void GpuBatch(u64 value, u64 submit_t0, u64 submit_t1, u64 gpu_t0, u64 gpu_t1, u32 frame) {
    if ((detail::g_mode.load(std::memory_order_relaxed) & 3) == 0) {
        return;
    }
    if (gpu_t0 == 0 || gpu_t1 < gpu_t0) {
        ++g.gpu_untimed;
        return;
    }
    ++g.gpu_timed;
    const u32 slot = frame % kFrames;
    if (g.gpu_frame_tag[slot] != frame) {
        g.gpu_frame_tag[slot] = frame;
        g.gpu_gap_ticks[slot] = g.gpu_gap_first_ticks[slot] = g.gpu_busy_ticks[slot] = 0;
        g.gpu_batches[slot] = 0;
    }
    // completed in submission order on one queue: the gap before this batch (its top-of-pipe timestamp may come before
    // the previous one's end: no gap then); the busy time is the union of the intervals
    const u64 from = std::max(gpu_t0, g.gpu_last_end);
    if (g.gpu_last_end != 0 && gpu_t0 > g.gpu_last_end) {
        (frame != g.gpu_last_frame ? g.gpu_gap_first_ticks : g.gpu_gap_ticks)[slot] += gpu_t0 - g.gpu_last_end;
    }
    if (gpu_t1 > from) {
        g.gpu_busy_ticks[slot] += gpu_t1 - from;
    }
    ++g.gpu_batches[slot];
    g.gpu_last_end = std::max(g.gpu_last_end, gpu_t1);
    g.gpu_last_frame = frame;
    if (Deep() && InWindow(submit_t0, submit_t1)) {
        if (g.batches.size() < g.batches.capacity()) {
            g.batches.push_back({value, submit_t0, submit_t1, gpu_t0, gpu_t1, frame, 0});
        } else {
            ++g.batches_dropped;
        }
    }
}

bool Start(const std::filesystem::path& logs_dir, double timestamp_period) {
    char v[256] = {};
    const DWORD len = GetEnvironmentVariableA("SSPC_P412_STALL_TRACE", v, sizeof(v));
    if (len == 0 || len >= sizeof(v)) {
        return false;
    }
    std::string_view s(v, len);
    u32 mode = 0;
    Options opt;
    if (s == "1" || s == "light") {
        mode = 1;
    } else if (s.starts_with("deep")) {
        mode = 2;
        std::string_view rest = s.substr(4);
        while (!rest.empty()) {
            if (rest.front() == ',') {
                rest.remove_prefix(1);
                continue;
            }
            const size_t comma = rest.find(',');
            const std::string kv(rest.substr(0, comma));
            rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma);
            const auto value = [&](std::string_view key) -> const char* {
                return std::string_view(kv).starts_with(key) ? kv.c_str() + key.size() : nullptr;
            };
            if (const char* w = value("window=")) {
                char* end = nullptr;
                const double a = std::strtod(w, &end);
                const double b = *end == ':' ? std::strtod(end + 1, nullptr) : 0.0;
                if (b > a) {
                    opt.win_a = a;
                    opt.win_b = b;
                }
            } else if (const char* e = value("events=")) {
                opt.events = std::max<u64>(std::strtoull(e, nullptr, 10), 1024);
            } else if (const char* h = value("sample=")) {
                opt.sample_hz = static_cast<u32>(std::min<u64>(std::strtoull(h, nullptr, 10), 10000));
            } else if (kv == "records=0") {
                opt.records = false;
            } else {
                SSPC_LOG(GPU, Warning, "SSPC_P412_STALL_TRACE: unknown option '{}' (ignored)", kv);
            }
        }
    } else {
        SSPC_LOG(GPU, Warning, "SSPC_P412_STALL_TRACE='{}' is not 1, light or deep[,...]: off", s);
        return false;
    }
    g.opt = opt;
    g.dir = logs_dir;
    g.tsc_hz = static_cast<double>(std::max<u64>(Kernel::Clock::TscFrequency(), 1));
    g.ts_period = timestamp_period;
    g.start_tsc = FrameLog::StartTsc() != 0 ? FrameLog::StartTsc() : Kernel::Clock::ReadTsc();
    if (mode == 2) {
        g.win0 = g.start_tsc + static_cast<u64>(opt.win_a * g.tsc_hz);
        g.win1 = g.start_tsc + static_cast<u64>(opt.win_b * g.tsc_hz);
    }
    g.gpu_gap_ticks.reset(new u64[kFrames]());
    g.gpu_gap_first_ticks.reset(new u64[kFrames]());
    g.gpu_busy_ticks.reset(new u64[kFrames]());
    g.gpu_batches.reset(new u32[kFrames]());
    g.gpu_frame_tag.reset(new u32[kFrames]);
    std::fill_n(g.gpu_frame_tag.get(), kFrames, ~0u);
    if (mode == 2) {
        const double secs = opt.win_b - opt.win_a;
        g.batches.reserve(static_cast<size_t>(secs * 2000.0) + 4096); // (~18 batches a frame)
        g.cal.reserve(static_cast<size_t>((secs + 2.0) * 20.0) + 64);
        if (opt.sample_hz != 0) {
            g.samples.resize(static_cast<size_t>(secs * opt.sample_hz * 2.2) + 1024);
        }
    }
    detail::g_mode.fetch_or(mode, std::memory_order_release); // (kForward may be set already)
    if (mode == 2 && opt.sample_hz != 0 && g.sampler == nullptr) {
        g.sampler = new std::thread(SamplerRun); // (joined by Stop; never destroyed while joinable)
    }
    SSPC_LOG(GPU, Warning, "DIAGNOSTIC: SSPC_P412_STALL_TRACE={} - the native backend's waits are traced ({}){}", s,
             mode == 2 ? "deep: events, GPU batches and stack samples in the window" : "light: counters only",
             mode == 2 ? fmt::format(", window {:.1f}..{:.1f} s, {} events for the recording thread, {} samples/s",
                                     opt.win_a, opt.win_b, opt.events, opt.sample_hz)
                       : std::string{});
    return true;
}

void EnableForward() {
    if (g.tsc_hz == 0.0) {
        g.tsc_hz = static_cast<double>(std::max<u64>(Kernel::Clock::TscFrequency(), 1));
    }
    detail::g_mode.fetch_or(kForward, std::memory_order_release);
}

void SetCalibration(CalibrateFn fn, void* ctx) {
    g.cal_fn = fn;
    g.cal_ctx = ctx;
}

void MaybeCalibrate() {
    if (!Deep() || g.cal_fn == nullptr) {
        return;
    }
    const u64 now = __rdtsc();
    // from a second before the window to a second after it, every 50 ms
    const u64 margin = static_cast<u64>(g.tsc_hz);
    if (now < g.cal_next || now + margin < g.win0 || now > g.win1 + margin || g.cal.size() >= g.cal.capacity()) {
        return;
    }
    g.cal_next = now + static_cast<u64>(g.tsc_hz * 0.05);
    CalRec r{};
    r.tsc_before = __rdtsc();
    if (!g.cal_fn(g.cal_ctx, r.gpu, r.qpc, r.dev_ns)) {
        g.cal_fn = nullptr; // (not available: the batches stay in GPU ticks; the analysis says so)
        return;
    }
    r.tsc_after = __rdtsc();
    LARGE_INTEGER q{};
    const u64 a = __rdtsc();
    QueryPerformanceCounter(&q);
    const u64 b = __rdtsc();
    r.qpc2 = static_cast<u64>(q.QuadPart);
    r.tsc2 = a + (b - a) / 2;
    g.cal.push_back(r);
}

void CpFrame(u32 n) {
    if (On()) {
        g.frame.store(n + 1, std::memory_order_relaxed);
    }
}

u32 CurrentFrame() {
    return g.frame.load(std::memory_order_relaxed);
}

void Stop() {
    const u32 all = detail::g_mode.load(std::memory_order_acquire);
    const u32 mode = all & 3;
    if (all == 0 || g.stopped) {
        return;
    }
    if (mode == 0) { // only the forwarding to the shader stall trace (P4.14): no files of this trace
        g.stopped = true;
        detail::g_mode.store(0, std::memory_order_release);
        return;
    }
    g.stopped = true;
    g.sampler_stop.store(true, std::memory_order_release);
    if (g.sampler != nullptr && g.sampler->joinable()) {
        g.sampler->join();
    }
    detail::g_mode.store(0, std::memory_order_release);
    Sleep(5); // (a thread past its On() check finishes its event; the counts are published after the event)
    WriteLight();
    if (mode == 2) {
        WriteDeep();
    }
    u64 kept = 0, dropped = 0;
    size_t threads = 0;
    {
        std::scoped_lock lock{g.logs_mutex};
        threads = g.logs.size();
        for (const ThreadLog* l : g.logs) {
            kept += l->n.load();
            dropped += l->dropped;
        }
    }
    SSPC_LOG(GPU, Info,
             "stall trace (P4.12): {} threads, {} events kept, {} dropped (ring full), {} GPU batches ({} dropped), {} "
             "calibration pairs, {} samples ({} failed) -> {}",
             threads, kept, dropped, g.batches.size(), g.batches_dropped, g.cal.size(), g.samples_n.load(),
             g.samples_failed, (g.dir / (mode == 2 ? "p412_trace.bin" : "p412_light.json")).string());
}

} // namespace SecondSon::Gpu::StallTrace
