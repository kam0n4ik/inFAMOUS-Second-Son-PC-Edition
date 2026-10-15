// SecondSonPC — standalone tests of the reconstructed engine code (src/engine), without the game and without the
// bridge: the guest's pthread mutex/condition variable are host SRW locks/condition variables behind the same
// System V entry points the game's PLT stubs have.
//   sspc_engine_tests [--bench]   -> stdout, exit code = failures
//     --bench   the queue's throughput with each wait policy (many tiny jobs, 5 workers + a waiting thread)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "engine/guest_hooks.h"
#include "engine/guest_probe.h"
#include "engine/job_system.h"
#include "engine/particles/smoke_quality.h"
#include "engine/render/dcb_scan.h"
#include "engine/render/native_draw.h"
#include "engine/render/native_render_ops.h"
#include "engine/render/native_state.h"
#include "engine/render/pass_graph.h"
#include "engine/render/render_layout.h"

using namespace SecondSon;
using namespace SecondSon::Engine;
using namespace SecondSon::Engine::Jobs;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        ++g_checks;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            ++g_failures;                                                                                              \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                              \
        }                                                                                                              \
    } while (0)

// --- host stand-ins for the guest's primitives (System V, like the PLT stubs) ---------------------------------------

SSPC_SYSV_ABI s32 MutexInit(void* object, const void*, const char*) {
    auto* lock = new SRWLOCK;
    InitializeSRWLock(lock);
    *static_cast<SRWLOCK**>(object) = lock;
    return 0;
}
SSPC_SYSV_ABI s32 CondInit(void* object, const void*, const char*) {
    auto* cv = new CONDITION_VARIABLE;
    InitializeConditionVariable(cv);
    *static_cast<CONDITION_VARIABLE**>(object) = cv;
    return 0;
}
SSPC_SYSV_ABI s32 MutexLock(void* object) {
    AcquireSRWLockExclusive(*static_cast<SRWLOCK**>(object));
    return 0;
}
SSPC_SYSV_ABI s32 MutexUnlock(void* object) {
    ReleaseSRWLockExclusive(*static_cast<SRWLOCK**>(object));
    return 0;
}
SSPC_SYSV_ABI s32 CondWait(void* cond, void* mutex) {
    SleepConditionVariableSRW(*static_cast<CONDITION_VARIABLE**>(cond), *static_cast<SRWLOCK**>(mutex), INFINITE, 0);
    return 0;
}
SSPC_SYSV_ABI void Delay(u32 us) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
    while (std::chrono::steady_clock::now() < end) {
    }
}
SSPC_SYSV_ABI void CountJob(u64 arg) {
    reinterpret_cast<std::atomic<u32>*>(arg)->fetch_add(1, std::memory_order_relaxed);
}

/// A JobManager and its iterator lists in host memory.
struct Fixture {
    alignas(16) u8 manager[Field::kSize] = {};
    GuestIterator* next_iterators = nullptr;
    GuestIterator* prev_iterators = nullptr;

    explicit Fixture(WaitPolicy policy = WaitPolicy::Guest) {
        Environment env;
        env.manager = manager;
        env.next_iterators = &next_iterators;
        env.prev_iterators = &prev_iterators;
        env.mutex_init = &MutexInit;
        env.cond_init = &CondInit;
        env.mutex_lock = &MutexLock;
        env.mutex_unlock = &MutexUnlock;
        env.cond_wait = &CondWait;
        env.delay_us = &Delay;
        Options o;
        o.wait = policy;
        Configure(env, o);
    }
    template <typename T>
    T& F(size_t off) {
        return *reinterpret_cast<T*>(manager + off);
    }
    bool Quit() {
        return std::atomic_ref<u8>(F<u8>(Field::kQuit)).load(std::memory_order_acquire) != 0;
    }
    void Lock() {
        if (F<u8>(Field::kInit) == 0) {
            F<u8>(Field::kInit) = 1;
            MutexInit(manager + Field::kMutex, nullptr, nullptr);
            CondInit(manager + Field::kCond, nullptr, nullptr);
        }
        MutexLock(manager + Field::kMutex);
    }
    void Unlock() {
        MutexUnlock(manager + Field::kMutex);
    }
    void Broadcast() {
        WakeAllConditionVariable(*reinterpret_cast<CONDITION_VARIABLE**>(manager + Field::kCond));
    }
    /// What the guest's submit (eboot 0x243a40) and kick (0x244770) do: state 2, append, broadcast.
    void Submit(GuestBatch* b) {
        b->state = 2;
        Lock();
        b->next = nullptr;
        b->prev = F<GuestBatch*>(Field::kTail);
        if (b->prev != nullptr) {
            b->prev->next = b;
        } else {
            F<GuestBatch*>(Field::kHead) = b;
        }
        F<GuestBatch*>(Field::kTail) = b;
        Broadcast();
        Unlock();
    }
};

/// A batch with `n` entries running CountJob on per-entry counters.
struct Batch {
    std::vector<u8> storage;
    std::vector<std::atomic<u32>> runs;
    explicit Batch(int n) : storage(sizeof(GuestBatch) + n * sizeof(GuestEntry)), runs(n) {
        auto* b = Get();
        b->state = 0;
        b->count = n;
        for (int i = 0; i < n; ++i) {
            GuestEntry& e = b->Entries()[i];
            e = {};
            e.fn = reinterpret_cast<u64>(&CountJob);
            e.arg = reinterpret_cast<u64>(&runs[i]);
        }
    }
    GuestBatch* Get() {
        return reinterpret_cast<GuestBatch*>(storage.data());
    }
};

void TestHash() {
    std::printf("hash\n");
    CHECK(HashBytes("", 0) == 0xcbf29ce484222325ull);
    CHECK(HashBytes("a", 1) == 0xaf63dc4c8601ec8cull);
    CHECK(HashBytes("foobar", 6) == 0x85944171f73967e8ull);
}

void TestOrder() {
    std::printf("order: entries in submission order, batches unlinked when every entry is taken\n");
    Fixture fx;
    Batch a(3), b(1);
    fx.Submit(a.Get());
    fx.Submit(b.Get());
    GuestEntry* e[5] = {};
    for (auto& x : e) {
        x = TryNextJob();
    }
    CHECK(e[0] == &a.Get()->Entries()[0] && e[1] == &a.Get()->Entries()[1] && e[2] == &a.Get()->Entries()[2]);
    CHECK(e[3] == &b.Get()->Entries()[0]);
    CHECK(e[4] == nullptr);
    CHECK(a.Get()->state == 3 && b.Get()->state == 3);
    CHECK(e[0]->state == 1 && e[3]->state == 1);
    CHECK(fx.F<GuestBatch*>(Field::kHead) == nullptr && fx.F<GuestBatch*>(Field::kTail) == nullptr);
    CHECK(fx.F<s32>(Field::kCursor) == 0);
    CHECK(a.Get()->next == nullptr && a.Get()->prev == nullptr);
}

void TestIterators() {
    std::printf("iterators: unlinking the head moves the iterators that point into it\n");
    Fixture fx;
    Batch a(1), b(2);
    fx.Submit(a.Get());
    fx.Submit(b.Get());
    GuestIterator to_a_next{&a.Get()->next, nullptr};
    GuestIterator other{&b.Get()->next, &to_a_next};
    GuestIterator to_a_prev{&a.Get()->prev, nullptr};
    fx.next_iterators = &other;
    fx.prev_iterators = &to_a_prev;
    CHECK(TryNextJob() == &a.Get()->Entries()[0]);
    CHECK(to_a_next.slot == fx.manager + Field::kHead); // a had no previous batch
    CHECK(to_a_prev.slot == &b.Get()->prev);            // a's next is b
    CHECK(other.slot == &b.Get()->next);                // untouched
    CHECK(fx.F<GuestBatch*>(Field::kHead) == b.Get() && b.Get()->prev == nullptr);
    // b is the last batch: a prev-iterator on it goes to the tail field.
    GuestIterator to_b_prev{&b.Get()->prev, nullptr};
    fx.prev_iterators = &to_b_prev;
    fx.next_iterators = nullptr;
    CHECK(TryNextJob() == &b.Get()->Entries()[0]);
    CHECK(TryNextJob() == &b.Get()->Entries()[1]);
    CHECK(to_b_prev.slot == fx.manager + Field::kTail);
}

void TestWaitHelps() {
    std::printf("wait: no workers - the waiting thread runs every entry itself\n");
    Fixture fx;
    Batch a(4);
    fx.Submit(a.Get());
    const Stats before = Snapshot();
    WaitBatch(fx.manager, a.Get());
    const Stats after = Snapshot();
    for (auto& r : a.runs) {
        CHECK(r.load() == 1);
    }
    for (int i = 0; i < 4; ++i) {
        CHECK(a.Get()->Entries()[i].state == 2);
    }
    CHECK(a.Get()->state == 4);
    CHECK(after.helped - before.helped == 4);
    CHECK(after.waits - before.waits == 1);
    // Waiting again, or for an empty batch: returns at once.
    WaitBatch(fx.manager, a.Get());
    Batch empty(0);
    WaitBatch(fx.manager, empty.Get());
    CHECK(Snapshot().waits == after.waits);
}

void TestQuit() {
    std::printf("quit: a sleeping worker leaves NextJob with nullptr\n");
    Fixture fx;
    std::atomic<bool> got_null{false};
    std::thread w([&] { got_null = NextJob() == nullptr; });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    fx.Lock();
    fx.F<u8>(Field::kQuit) = 1;
    fx.Broadcast();
    fx.Unlock();
    w.join();
    CHECK(got_null.load());
}

/// 5 workers (like JobWorker1..5) + this thread submitting and waiting: every entry runs exactly once.
double Stress(WaitPolicy policy, int batches, int max_entries, bool quiet) {
    Fixture fx(policy);
    std::vector<std::thread> workers;
    std::atomic<int> stopped{0};
    for (int i = 0; i < 5; ++i) {
        workers.emplace_back([&] {
            while (!fx.Quit()) { // the guest worker loop (eboot 0x243680)
                if (GuestEntry* e = NextJob(); e != nullptr) {
                    reinterpret_cast<JobFn>(e->fn)(e->arg);
                    std::atomic_ref<u32>(e->state).store(2, std::memory_order_release);
                }
            }
            stopped.fetch_add(1);
        });
    }
    std::mt19937 rng(12345);
    std::vector<std::unique_ptr<Batch>> all;
    all.reserve(batches);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < batches; ++i) {
        all.push_back(std::make_unique<Batch>(1 + static_cast<int>(rng() % max_entries)));
        fx.Submit(all.back()->Get());
        if (i % 3 == 2) { // waits lag behind submissions, as frames do
            WaitBatch(fx.manager, all[i - 2]->Get());
            WaitBatch(fx.manager, all[i - 1]->Get());
            WaitBatch(fx.manager, all[i]->Get());
        }
    }
    for (auto& b : all) {
        WaitBatch(fx.manager, b->Get());
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    // Shutdown like eboot 0x243770: set quit, broadcast until every worker is out.
    fx.Lock();
    std::atomic_ref<u8>(fx.F<u8>(Field::kQuit)).store(1, std::memory_order_release);
    fx.Unlock();
    while (stopped.load() != 5) {
        fx.Lock();
        fx.Broadcast();
        fx.Unlock();
        Sleep(1);
    }
    for (auto& w : workers) {
        w.join();
    }
    u64 bad = 0, entries = 0;
    for (auto& b : all) {
        bad += b->Get()->state != 4;
        for (auto& r : b->runs) {
            bad += r.load() != 1;
            ++entries;
        }
    }
    if (!quiet) {
        std::printf("  %s: %d batches, %llu entries in %.3f s, wrong %llu\n", WaitPolicyName(policy), batches,
                    static_cast<unsigned long long>(entries), seconds, static_cast<unsigned long long>(bad));
    }
    CHECK(bad == 0);
    CHECK(fx.F<GuestBatch*>(Field::kHead) == nullptr);
    return seconds;
}

void TestStress() {
    std::printf("stress: 5 workers + a waiting thread, every entry exactly once\n");
    for (WaitPolicy p : {WaitPolicy::Guest, WaitPolicy::Pause, WaitPolicy::Yield}) {
        Stress(p, 20000, 16, false);
    }
}

void Bench() {
    std::printf("bench: 100000 batches of 1..8 tiny entries, 5 runs each\n");
    for (WaitPolicy p : {WaitPolicy::Guest, WaitPolicy::Pause, WaitPolicy::Yield}) {
        std::vector<double> t;
        for (int i = 0; i < 5; ++i) {
            t.push_back(Stress(p, 100000, 8, true));
        }
        std::sort(t.begin(), t.end());
        std::printf("  %-5s median %.3f s (min %.3f, max %.3f)\n", WaitPolicyName(p), t[2], t[0], t[4]);
    }
}

// --- P5.3A: entry probe, command buffer walk, pass graph -------------------------------------------------------------

using AddFn = SSPC_SYSV_ABI s32 (*)(s32, s32);
AddFn g_add_original = nullptr;
int g_add_probe_calls = 0;
SSPC_SYSV_ABI s32 AddProbe(s32 a, s32 b) {
    ++g_add_probe_calls;
    return g_add_original(a, b) + 1000;
}

void TestEntryProbe() {
    // mov eax, edi; add eax, esi; 10 x nop; ret - a System V add(a, b) whose 14-byte prologue moves to the trampoline.
    u8 code[15] = {0x89, 0xF8, 0x01, 0xF0};
    std::memset(code + 4, 0x90, 10);
    code[14] = 0xC3;
    auto* page = static_cast<u8*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    CHECK(page != nullptr);
    std::memcpy(page, code, sizeof(code));
    const u64 base = reinterpret_cast<u64>(page);
    CHECK(reinterpret_cast<AddFn>(page)(2, 3) == 5);

    u8 t[TrampolineSize(14)] = {};
    CHECK(BuildTrampoline(code, 14, 0x1122334455667788ull, t) == 28);
    CHECK(std::memcmp(t, code, 14) == 0 && t[14] == 0xFF && t[15] == 0x25 && t[16] == 0 && t[19] == 0);
    u64 resume = 0;
    std::memcpy(&resume, t + 20, 8);
    CHECK(resume == 0x1122334455667788ull);

    std::string error;
    const ProbeSite wrong{{0, sizeof(code), HashBytes(code, sizeof(code)) ^ 1}, 14};
    CHECK(InstallEntryProbe(base, wrong, reinterpret_cast<const void*>(&AddProbe), error) == nullptr);
    CHECK(!error.empty());
    CHECK(reinterpret_cast<AddFn>(page)(2, 3) == 5); // untouched
    const ProbeSite short_prologue{{0, sizeof(code), HashBytes(code, sizeof(code))}, 13};
    CHECK(InstallEntryProbe(base, short_prologue, reinterpret_cast<const void*>(&AddProbe), error) == nullptr);

    const ProbeSite site{{0, sizeof(code), HashBytes(code, sizeof(code))}, 14};
    void* original = InstallEntryProbe(base, site, reinterpret_cast<const void*>(&AddProbe), error);
    CHECK(original != nullptr);
    g_add_original = reinterpret_cast<AddFn>(original);
    CHECK(g_add_original(20, 22) == 42);                         // the trampoline is the original function
    CHECK(reinterpret_cast<AddFn>(page)(2, 3) == 1005);          // callers of the entry reach the probe
    CHECK(reinterpret_cast<AddFn>(page)(-7, 7) == 1000 && g_add_probe_calls == 2);
}

void TestSmokeScale() {
    using Particles::ScaleSpawn;
    using Particles::SmokeDensity;
    CHECK(SmokeDensity("high") == 1.0f && SmokeDensity("medium") == 0.65f && SmokeDensity("low") == 0.35f);
    CHECK(SmokeDensity("verylow") == 0.20f && SmokeDensity("0.5") == 0.5f && SmokeDensity("0") == 0.0f);
    CHECK(!SmokeDensity("bogus") && !SmokeDensity("1.5") && !SmokeDensity("") && !SmokeDensity("-0.1"));
    CHECK(!SmokeDensity("nan") && !SmokeDensity("inf") && !SmokeDensity(" 0.5") && !SmokeDensity("0.5x"));
    CHECK(Particles::ParticleLevel("auto") == -1 && Particles::ParticleLevel("full") == 0 &&
          Particles::ParticleLevel("half") == 1 && Particles::ParticleLevel("quarter") == 2 &&
          !Particles::ParticleLevel("Half") && !Particles::ParticleLevel(""));
    // without an installed probe the setting is kept for Install, the density in effect stays the original
    Particles::SetSmokeQuality("low");
    CHECK(Particles::CurrentDensity() == 1.0f);
    Particles::SetSmokeQuality("high");

    float carry = 0.5f;
    CHECK(ScaleSpawn(7, 1.0f, carry, false) == 7 && carry == 0.5f); // the original density: untouched
    CHECK(ScaleSpawn(0, 0.35f, carry, true) == 0);
    // Long-run rate is exact: a continuous emitter (10 per frame, 1 per frame) and bursts of 30.
    for (const auto [added, density] : {std::pair{10u, 0.35f}, std::pair{1u, 0.20f}, std::pair{30u, 0.65f}}) {
        carry = 0.5f;
        u64 kept = 0;
        bool over = false;
        for (int i = 0; i < 1000; ++i) {
            const u32 k = ScaleSpawn(added, density, carry, false);
            over |= k > added;
            kept += k;
        }
        const double want = 1000.0 * added * density;
        CHECK(!over && std::abs(static_cast<double>(kept) - want) <= 1.0);
    }
    // An emitter with nothing alive always shows a particle; density 0 (diagnostics) spawns nothing.
    carry = 0.0f;
    CHECK(ScaleSpawn(1, 0.20f, carry, true) == 1 && carry < 0.0f);
    carry = 0.0f;
    CHECK(ScaleSpawn(3, 0.0f, carry, true) == 0);

    // The engine's name hash: reflection-table names and emitter ids seen in the village and on the beach.
    using Particles::EngineNameHash;
    using Particles::SmokeEmitterName;
    CHECK(EngineNameHash("PARTICLE_COUNT_RATIO") == 0xe8c7ff5f056cdb5aull);
    CHECK(EngineNameHash("ALPHA") == 0x260578ae6ull && EngineNameHash("lifetime") == 0x298973b4039f6bull);
    CHECK(EngineNameHash("smoke01") == 0x00007014345e5a98ull && EngineNameHash("thick_smoke") == 0xdb15fac831af4989ull);
    CHECK(EngineNameHash("flames_small") == 0xa66d04e778429f20ull);
    std::string name;
    CHECK(SmokeEmitterName(0x00007014345e5a98ull, &name) && name == "smoke01");
    CHECK(SmokeEmitterName(0xdb15fac831af4989ull, &name) && name == "thick_smoke");
    CHECK(SmokeEmitterName(EngineNameHash("smoke_near")) && SmokeEmitterName(EngineNameHash("smoke")));
    CHECK(SmokeEmitterName(EngineNameHash("campfire_smoke_01"))); // longer than the exact recovery: the dictionary
    CHECK(!SmokeEmitterName(0xa66d04e778429f20ull) && !SmokeEmitterName(0x8d939ba84d2e8e69ull)); // flames_*
    CHECK(!SmokeEmitterName(EngineNameHash("embers"), &name) && name == "embers");
    CHECK(!SmokeEmitterName(EngineNameHash("thick_steam")) && !SmokeEmitterName(EngineNameHash("light_fire")));
    CHECK(!SmokeEmitterName(EngineNameHash("dust_cloud")) && !SmokeEmitterName(EngineNameHash("foam_out")));
}

struct Pm4Writer {
    std::vector<u32> dw;
    void Packet(u8 op, std::initializer_list<u32> body) {
        dw.push_back(0xC0000000u | (static_cast<u32>(body.size() - 1) << 16) | (u32{op} << 8));
        dw.insert(dw.end(), body);
    }
    void Context(u32 reg, u32 value) {
        Packet(0x69, {reg, value});
    }
};

void TestDcbScan() {
    using namespace Render;
    Pm4Writer w;
    // Colour target 0: 1920x1080 at 0x1234500, format 0xA, number type 4.
    w.Packet(0x69, {Reg::CbColor0Base, 0x12345, 1920 / 8 - 1, 1920 * 1080 / 64 - 1, 0, (0xAu << 2) | (4u << 8)});
    w.Context(Reg::CbTargetMask, 0xF);
    w.Context(Reg::CbColorControl, 1u << 4);
    // Depth 1920x1080 at 0x20000, test + write.
    w.Context(Reg::DbZInfo, 3);
    w.Context(Reg::DbZReadBase, 0x200);
    w.Context(Reg::DbZWriteBase, 0x200);
    w.Context(Reg::DbDepthSize, (1920 / 8 - 1) | ((1080 / 8 - 1) << 11));
    w.Context(Reg::DbDepthControl, 0x6);
    w.Packet(0x76, {0x008, 0x100000, 0}); // SPI_SHADER_PGM_LO/HI_PS: program at 0x10000000
    w.Packet(0x2F, {4});                  // NUM_INSTANCES
    w.Packet(0x2D, {36, 2});              // DRAW_INDEX_AUTO 36 vertices
    w.Packet(0x10, {0, 0, 0});            // NOP
    w.Packet(0x2F, {1});
    w.Context(Reg::CbTargetMask, 0);      // depth-only, test without write
    w.Context(Reg::DbDepthControl, 0x2);
    w.Packet(0x27, {100, 0, 0, 30, 0});   // DRAW_INDEX_2: 30 indices
    w.Packet(0x76, {0x20C, 0x200000, 0}); // COMPUTE_PGM_LO/HI: 0x20000000
    w.Packet(0x15, {8, 8, 1, 1});         // DISPATCH_DIRECT
    w.Packet(0x58, {0, 0, 0, 0, 0, 0});   // ACQUIRE_MEM
    w.dw.push_back(0xC0050000u | (0x10u << 8)); // NOP claiming 6 body dwords, truncated

    ScanState state;
    ScanResult r;
    ScanCommands(w.dw, state, r);
    CHECK(r.draws == 2 && r.draws_instanced == 1 && r.indices == 36 * 4 + 30);
    CHECK(r.dispatches == 1 && r.cache_actions == 1 && r.malformed == 1);
    CHECK(r.draws_depth_test == 2 && r.draws_depth_write == 1 && r.draws_no_color == 1);
    const TargetDesc color{0x1234500, 1920, 1080, 0xA | (4u << 8), false};
    const TargetDesc depth{0x20000, 1920, 1080, 3, true};
    CHECK(r.targets.size() == 2 && r.targets[0] == color && r.targets[1] == depth);
    CHECK(r.depth_read.size() == 1 && r.depth_read[0] == depth);
    CHECK(r.shaders.size() == 2 && r.shaders[0] == 0x10000000 && r.shaders[1] == 0x20000000);
    CHECK(state.num_instances == 1 && state.context[Reg::CbTargetMask] == 0);
}

void TestPassGraph() {
    using namespace Render;
    const u32 tsharp[8] = {0x12345, 0x01u | (0xAu << 20) | (7u << 26), 1919u | (1079u << 14), 0, 0, 0, 0, 0};
    const ResourceUse tex = DecodeTexture(tsharp, ResourceKind::Texture);
    CHECK(tex.base == 0x10001234500ull && tex.width == 1920 && tex.height == 1080 && tex.format == (0xA | (7u << 8)));
    const u32 vsharp[4] = {0x5000, 0x2u | (16u << 16), 64, 0};
    const ResourceUse buf = DecodeBuffer(vsharp, ResourceKind::RwBuffer);
    CHECK(buf.base == 0x200005000ull && buf.width == 16 && buf.height == 64);

    std::vector<ContextRecord> ctx(5);
    ctx[0].kind = NodeKind::Pre;
    ctx[0].scan.targets.push_back({0xA000, 64, 64, 1, false}); // pre writes A
    ctx[1].kind = NodeKind::Pass;
    ctx[1].pass = 1;
    ctx[1].uses.push_back({0xA000, 64, 64, 1, ResourceKind::Texture}); // pass 1 reads A ...
    ctx[2].kind = NodeKind::Pass;
    ctx[2].pass = 1;
    ctx[2].sub = 1;
    ctx[2].uses.push_back({0xB000, 4, 16, 0, ResourceKind::RwBuffer}); // ... and writes B (second context)
    ctx[3].kind = NodeKind::Pass;
    ctx[3].pass = 2;
    ctx[3].uses.push_back({0xB000, 4, 16, 0, ResourceKind::Buffer});  // pass 2 reads B
    ctx[3].uses.push_back({0xC000, 8, 8, 1, ResourceKind::Texture});  // and C, written later (previous frame's)
    ctx[3].uses.push_back({0xD000, 8, 8, 1, ResourceKind::Texture});  // and D, never written (static)
    ctx[4].kind = NodeKind::Post;
    ctx[4].scan.targets.push_back({0xC000, 8, 8, 1, false});

    const Graph g = BuildGraph(ctx);
    CHECK(g.nodes.size() == 4 && g.nodes[1].contexts == 2 && NodeName(g.nodes[1]) == "pass 1");
    CHECK(NodeName(g.nodes[0]) == "pre" && NodeName(g.nodes[3]) == "post");
    CHECK(g.edges.size() == 3);
    const auto has = [&](u32 from, u32 to, bool prev) {
        return std::any_of(g.edges.begin(), g.edges.end(),
                           [&](const Edge& e) { return e.from == from && e.to == to && e.prev_frame == prev; });
    };
    CHECK(has(0, 1, false) && has(1, 2, false) && has(3, 2, true));
    CHECK(g.lifetimes.size() == 3);
    const auto life = std::find_if(g.lifetimes.begin(), g.lifetimes.end(), [](const Lifetime& l) { return l.base == 0xA000; });
    CHECK(life != g.lifetimes.end() && life->target && life->first_write == 0 && life->last_read == 1 &&
          life->readers == 1);
    const auto b = std::find_if(g.lifetimes.begin(), g.lifetimes.end(), [](const Lifetime& l) { return l.base == 0xB000; });
    CHECK(b != g.lifetimes.end() && !b->target && b->first_write == 1 && b->first_read == 2 && b->readers == 1);
}

// --- P5.3B: the engine's draw described from its objects, carried as a marker, verified against the PM4 view --------

void TestNativeDrawShadow() {
    using namespace Render;
    namespace L = Render::Layout;
    // A stand-in eboot image large enough for the frame object; the pre-context R is the draw's context.
    const u64 size = L::kFrame + L::kFramePool + 0x10000;
    auto* image = static_cast<u8*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    CHECK(image != nullptr);
    const u64 base = reinterpret_cast<u64>(image);
    const u64 r = base + L::kFrame + L::kFramePreContext;
    const u64 cue = r + L::kContextCue;
    const auto put64 = [](u64 at, u64 v) { std::memcpy(reinterpret_cast<void*>(at), &v, 8); };
    put64(cue + L::kCueDcb, r + L::kContextGfx);

    // Shader objects: register block at +8 (sceGnmSetVsShader / SetPsShader350 order).
    static u32 vs_obj[16] = {0, 0, 0x1000, 0, 0x2C0000, 0x55, 0x2, 0x4, 0x10};
    static u32 ps_obj[16] = {0, 0, 0x2000, 0, 0x2A, 0x3, 0, 0x4, 0x2, 0x2, 0, 0, 0x10, 0xF};
    put64(cue + 0xC0B0, reinterpret_cast<u64>(vs_obj));
    put64(cue + 0xC0A8, reinterpret_cast<u64>(ps_obj));
    std::memcpy(reinterpret_cast<void*>(cue + 0xC168), "\0\0\0\0", 4); // no VS modifier

    // User-data layouts: VS table 0x40 dwords, whole-table pointer in SGPR 2, vertex table at +0x10 in SGPR 6.
    static u8 vs_layout[0x40] = {};
    static u8 ps_layout[0x40] = {};
    std::memset(vs_layout, 0xFF, sizeof(vs_layout));
    std::memset(ps_layout, 0xFF, sizeof(ps_layout));
    vs_layout[0] = 0x40;
    vs_layout[1] = 0;
    vs_layout[7] = 2;
    vs_layout[5] = 6;
    vs_layout[0x18] = 0x10;
    vs_layout[0x19] = 0;
    ps_layout[0] = 0x20;
    ps_layout[1] = 0;
    ps_layout[7] = 0;
    put64(cue + L::kCueLayout + 2 * 8, reinterpret_cast<u64>(vs_layout));
    put64(cue + L::kCueLayout + 1 * 8, reinterpret_cast<u64>(ps_layout));
    auto* vs_shadow = reinterpret_cast<u32*>(cue + L::kCueShadow + 2 * L::kCueShadowStride);
    auto* ps_shadow = reinterpret_cast<u32*>(cue + L::kCueShadow + 1 * L::kCueShadowStride);
    for (u32 i = 0; i < 0x40; ++i) {
        vs_shadow[i] = 0xA0000 + i;
        ps_shadow[i] = 0xB0000 + i;
    }

    DrawOp op;
    CHECK(NativeDraw::BuildDrawOp(base, r, true, 36, 0x123400, op));
    CHECK(op.pass == -2 && op.indexed && op.index_count == 36 && op.index_base == 0x123400);
    CHECK(op.shaders && op.has_ps && op.vs[0] == 0x1000 && op.vs[4] == 0x2 && op.vs[6] == 0x10);
    CHECK(op.ps[0] == 0x2000 && op.ps[11] == 0xF);
    CHECK(op.table[kStageVs].present && op.table[kStageVs].dwords == 0x40 && op.table[kStageVs].refs == 2);
    CHECK(op.table[kStageVs].ref[0].sgpr == 2 && op.table[kStageVs].ref[1].sgpr == 6 &&
          op.table[kStageVs].ref[1].offset == 0x10);
    CHECK(op.table[kStagePs].refs == 1 && op.table[kStagePs].dwords == 0x20);
    DrawOp other;
    CHECK(!NativeDraw::BuildDrawOp(base, r + 8, true, 36, 0, other)); // not a context of the frame

    // Through the command buffer and back.
    u32 marker[kDrawMarkerDwords + 4] = {};
    CHECK(EncodeDrawMarker(op, kVerify, marker) == kDrawMarkerDwords);
    CHECK(((marker[0] >> 16) & 0x3FFF) + 2 == kDrawMarkerDwords); // a NOP of exactly this length
    CHECK(IsMarker({marker, kDrawMarkerDwords}));
    DrawOp back;
    u16 flags = 0;
    CHECK(DecodeDrawMarker({marker, kDrawMarkerDwords}, back, flags) && flags == kVerify);
    CHECK(back.index_count == 36 && back.table[kStageVs].hash == op.table[kStageVs].hash &&
          back.table[kStageVs].ref[1].offset == 0x10 && std::memcmp(back.ps, op.ps, sizeof(op.ps)) == 0);
    CHECK(!DecodeDrawMarker({marker, kDrawMarkerDwords - 1}, back, flags));
    CHECK(!back.canary);
    DrawOp spoiled = op;
    spoiled.canary = true;
    EncodeDrawMarker(spoiled, kVerify, marker);
    DrawOp spoiled_back;
    CHECK(DecodeDrawMarker({marker, kDrawMarkerDwords}, spoiled_back, flags) && spoiled_back.canary);

    // The PM4 path's view: the commit copied the shadows and put pointers in user data.
    std::vector<u32> vs_snapshot(vs_shadow, vs_shadow + 0x40), ps_snapshot(ps_shadow, ps_shadow + 0x20);
    u32 vs_ud[16] = {}, ps_ud[16] = {};
    const u64 vsp = reinterpret_cast<u64>(vs_snapshot.data()), psp = reinterpret_cast<u64>(ps_snapshot.data());
    vs_ud[2] = static_cast<u32>(vsp);
    vs_ud[3] = static_cast<u32>(vsp >> 32);
    vs_ud[6] = static_cast<u32>(vsp + 0x40);
    vs_ud[7] = static_cast<u32>((vsp + 0x40) >> 32);
    ps_ud[0] = static_cast<u32>(psp);
    ps_ud[1] = static_cast<u32>(psp >> 32);
    DrawView view;
    view.indexed = true;
    view.index_count = 36;
    view.index_base = 0x123400;
    std::memcpy(view.vs, op.vs, sizeof(view.vs));
    std::memcpy(view.ps, op.ps, sizeof(view.ps));
    view.user_data[kStageVs] = vs_ud;
    view.user_data[kStagePs] = ps_ud;
    CHECK(VerifyDraw(back, view, &HashGuestMemory) == 0);

    DrawView bad = view;
    bad.index_count = 35;
    bad.ps[11] = 0x3;
    CHECK(VerifyDraw(back, bad, &HashGuestMemory) == (kMismatchIndexCount | kMismatchPsRegs));
    vs_snapshot[5] ^= 1; // a resource the shader sees differs from the engine's
    CHECK(VerifyDraw(back, view, &HashGuestMemory) == kMismatchVsTable);
    vs_snapshot[5] ^= 1;
    vs_ud[6] += 4; // the vertex table pointer disagrees with the whole-table pointer
    CHECK(VerifyDraw(back, view, &HashGuestMemory) == kMismatchVsTable);

    // P5.3C: the native commit of the same CUE (VS/PS mode), every part dirty.
    for (const u64 flag : {0xC1C9ull, 0xC1CAull, 0xC1D1ull, 0xC1D2ull}) {
        *reinterpret_cast<u8*>(cue + flag) = 1;
    }
    CHECK(NativeDraw::CheckNativeCommit(cue) == NativeDraw::CommitCheck::Native);
    *reinterpret_cast<u32*>(cue + 0xC1E0) = 0x20; // ES/GS mode: the game's commit must run
    CHECK(NativeDraw::CheckNativeCommit(cue) == NativeDraw::CommitCheck::StageMode);
    *reinterpret_cast<u32*>(cue + 0xC1E0) = 0;
    ps_obj[2 + 0xC] = 0; // (char)ps[0xe] = 0 PS inputs: no SPI_PS_INPUT_CNTL writes
    static std::vector<u32> arena(0x1000);
    static u32 arena_used = 0;
    arena_used = 0;
    NativeDraw::CommitEnv env;
    env.alloc = [](u64, u32 dwords) SSPC_SYSV_ABI -> u64 {
        const u64 at = reinterpret_cast<u64>(arena.data() + arena_used);
        arena_used += dwords;
        return at;
    };
    RegWrite writes[kMaxRegWrites];
    const u32 n = NativeDraw::NativeCommit(cue, env, writes);
    CHECK(n == 7 + 12 + 4 + 2); // VS + PS registers, VS table (2 pointers), PS table (1 pointer)
    CHECK(arena_used == 0x40 + 0x20 && arena[5] == vs_shadow[5] && arena[0x40 + 3] == ps_shadow[3]);
    CHECK(*reinterpret_cast<u8*>(cue + 0xC1C9) == 0 && *reinterpret_cast<u8*>(cue + 0xC1D2) == 0);
    CHECK((*reinterpret_cast<u16*>(cue + 0xC1E4) & 6) == 6);
    // The register file after the writes, as the processor applies them, is the draw the engine describes.
    u32 file[0x10000] = {};
    for (u32 i = 0; i < n; ++i) {
        file[writes[i].reg] = writes[i].value;
    }
    DrawOp after;
    CHECK(NativeDraw::BuildDrawOp(base, r, false, 3, 0, after));
    DrawView applied;
    applied.index_count = 3;
    for (u32 i = 0; i < kVsRegs; ++i) {
        applied.vs[i] = file[kVsRegIndex[i]];
    }
    for (u32 i = 0; i < kPsRegs; ++i) {
        applied.ps[i] = file[kPsRegIndex[i]];
    }
    applied.user_data[kStageVs] = file + 0x2C4C;
    applied.user_data[kStagePs] = file + 0x2C0C;
    CHECK(VerifyDraw(after, applied, &HashGuestMemory) == 0);
    CHECK(NativeDraw::NativeCommit(cue, env, writes) == 0); // nothing dirty any more
    u32 marker2[RegistersMarkerDwords(kMaxRegWrites)] = {};
    CHECK(EncodeRegistersMarker({writes, 3}, kApply, marker2) == RegistersMarkerDwords(3));
    RegWrite decoded[kMaxRegWrites];
    u32 decoded_n = 0;
    CHECK(IsMarker({marker2, RegistersMarkerDwords(3)}) &&
          DecodeRegistersMarker({marker2, RegistersMarkerDwords(3)}, decoded, decoded_n, flags) && decoded_n == 3 &&
          flags == kApply && decoded[2].reg == writes[2].reg);
    VirtualFree(image, 0, MEM_RELEASE);
}

// P5.3D native render state: stand-ins for the game's Gnm helpers - a target is bound when its base dword is not 0.
SSPC_SYSV_ABI u32 FakeRtBase(const void* t) {
    return static_cast<const u32*>(t)[0];
}
SSPC_SYSV_ABI u32 FakeRtFmaskBase(const void* t) {
    return static_cast<const u32*>(t)[9];
}
SSPC_SYSV_ABI void FakeRtSetFmask(void* t, u32 base) {
    static_cast<u32*>(t)[9] = base;
}
SSPC_SYSV_ABI u32 FakeDtZBase(const void* d) {
    return static_cast<const u32*>(d)[2];
}
SSPC_SYSV_ABI u32 FakeDtHtile(const void* d) {
    return static_cast<const u32*>(d)[9];
}

void TestNativeState() {
    namespace S = Render::NativeState;
    using Render::RegWrite;
    S::Env env;
    env.rt_base = &FakeRtBase;
    env.rt_fmask_base = &FakeRtFmaskBase;
    env.rt_set_fmask = &FakeRtSetFmask;
    env.dt_z_base = &FakeDtZBase;
    env.dt_htile = &FakeDtHtile;
    env.db_reg_2de = 489;
    const auto find = [](const RegWrite* w, u32 n, u32 reg) -> const RegWrite* {
        const RegWrite* hit = nullptr;
        for (u32 i = 0; i < n; ++i) {
            if (w[i].reg == reg) {
                hit = &w[i]; // the last write wins, as in the register file
            }
        }
        return hit;
    };
    const auto f = [](u32 bits) {
        float v;
        std::memcpy(&v, &bits, 4);
        return v;
    };

    // Viewport 0x11b60 over 1920x1080.
    RegWrite vw[32];
    S::Writer v{vw, 32};
    S::Viewport(0, 0, 1920, 1080, v);
    CHECK(!v.overflow && v.n == 2 + 6 + 1 + 2 + 1 + 4 && v.pm4_dwords == 28);
    CHECK(vw[0].reg == 0xA0B4 && f(vw[0].value) == 0.0f && f(vw[1].value) == 1.0f);
    CHECK(vw[2].reg == 0xA10F && f(vw[2].value) == 960.0f && f(vw[3].value) == 960.0f && f(vw[4].value) == -540.0f &&
          f(vw[5].value) == 540.0f && f(vw[6].value) == 1.0f && f(vw[7].value) == 0.0f);
    CHECK(vw[8].reg == 0xA206 && vw[8].value == 0x43F);
    CHECK(vw[9].reg == 0xA00C && vw[9].value == 0 && vw[10].value == (1080u << 16 | 1920u));
    CHECK(vw[11].reg == 0xA08D && vw[11].value == (32u << 16 | 60u)); // floor(960/16+.5)=60, floor(540/16+.5)=34 -> 32
    const float x_adj = (f(0x46FFFFFEu) - 960.0f + 960.0f - 960.0f) / 960.0f; // the smaller of the two distances
    CHECK(vw[12].reg == 0xA2FA && f(vw[14].value) == x_adj && f(vw[13].value) == 1.0f && f(vw[12].value) > 1.0f);

    // SetRenderTargets 0x11f910: two colour targets (the second one unbound), depth target 0, the previous set had 4.
    static u8 set[0x800] = {};
    const u64 ts = reinterpret_cast<u64>(set);
    const auto put = [&](u32 at, u32 value) { std::memcpy(set + at, &value, 4); };
    const auto putf = [&](u32 at, float value) { std::memcpy(set + at, &value, 4); };
    put(0x0, 1280);
    put(0x4, 720);
    putf(0x14, -1.0f); // the normalised rectangle: the whole target
    putf(0x18, -1.0f);
    putf(0x1C, 1.0f);
    putf(0x20, 1.0f);
    put(0x24, 0xFF);   // CB_TARGET_MASK
    put(0x77C, 2);
    put(0x784, 0);
    for (u32 i = 0; i < 16; ++i) {
        put(0x28 + i * 4, 0x100 + i); // target 0: base 0x100, extent dword 0x10F
    }
    put(0x28 + 4 * 4, 0x2000);         // CB_COLOR_INFO: FMASK compression ...
    put(0x28 + 7 * 4, 5);              // ... CMASK present
    put(0x28 + 9 * 4, 0);              // ... no FMASK: 0x52e0 derives one
    put(0x28 + 0xD0, 0);               // target 1: base 0 - not bound
    for (u32 i = 0; i < 13; ++i) {
        put(0x74 + i * 4, 0x200 + i);  // depth target: Z base 0x202, extent dword 0x20C
    }
    S::StateBlock st;
    st.color_mask = 0xF;
    RegWrite tw[Render::kMaxStateWrites];
    S::Writer t{tw, Render::kMaxStateWrites};
    S::SetRenderTargets(st, ts, nullptr, env, t);
    CHECK(!t.overflow);
    CHECK(t.pm4_dwords == 18 + 3 + 24 + 3 + 3 + 28 + 4 + 3 * 2);
    CHECK(tw[0].reg == 0xA318 && tw[0].value == 0x100 && tw[13].reg == 0xA318 + 13);
    CHECK(find(tw, t.n, 0xA318 + 9)->value == 0x100); // the derived FMASK base = the colour base
    CHECK(find(tw, t.n, 0xA31C)->value == 0x2000);     // FMASK compression bit 14 cleared (it was 0)
    CHECK(find(tw, t.n, Render::kStateCbExtent0)->value == 0x10F);
    CHECK(find(tw, t.n, 0xA31C + 15)->value == 0 && find(tw, t.n, Render::kStateCbExtent0 + 1) == nullptr);
    CHECK(find(tw, t.n, 0xA010)->value == 0x200 && find(tw, t.n, 0xA017)->value == 0x207 &&
          find(tw, t.n, 0xA00F)->value == 0x20B && find(tw, t.n, 0xA002)->value == 0x208 &&
          find(tw, t.n, 0xA2AF)->value == 0x20A && find(tw, t.n, Render::kStateDbExtent)->value == 0x20C);
    CHECK(find(tw, t.n, 0xA2DE)->value == 489 && find(tw, t.n, 0xA08E)->value == 0xFF);
    CHECK(find(tw, t.n, 0xA081)->value == (1u << 31) && find(tw, t.n, 0xA082)->value == (720u << 16 | 1280u));
    CHECK(find(tw, t.n, 0xA31C + 30)->value == 0 && find(tw, t.n, 0xA31C + 45)->value == 0); // slots 2, 3 off
    CHECK(st.color_mask == 3 && st.targets == ts && st.rect[2] == 1280 && st.rect[3] == 720);

    // No target set: every bound colour target off, no depth.
    S::Writer none{tw, Render::kMaxStateWrites};
    S::SetRenderTargets(st, 0, nullptr, env, none);
    CHECK(none.n == 2 + 2 + 1 && none.pm4_dwords == 3 * 2 + 6 && st.color_mask == 0 && st.targets == 0);
    CHECK(find(tw, none.n, Render::kStateDbExtent)->value == 0);

    // TransitionTo 0x120020: only what differs and is in the block's mask; the same target set is not rebound.
    static u32 block[14] = {};
    const u64 bp = reinterpret_cast<u64>(block);
    for (u32 i = 0; i < 6; ++i) {
        block[i] = 0x10 + i;
    }
    block[6] = 0b100101; // PA_SU_SC_MODE_CNTL, DB_RENDER_CONTROL, DB_STENCIL_CONTROL
    block[7] = 0x01020304;
    std::memcpy(block + 8, &ts, 8);
    S::StateBlock sb;
    sb.valid = 0b000011;
    sb.reg[0] = 0x10; // already PA_SU_SC_MODE_CNTL's value: not written again
    sb.targets = ts;
    S::Writer x{tw, Render::kMaxStateWrites};
    S::TransitionTo(sb, bp, env, x);
    CHECK(x.n == 4 && tw[0].reg == 0xA000 && tw[0].value == 0x12 && tw[1].reg == 0xA10B && tw[1].value == 0x15 &&
          tw[2].reg == 0xA10C && tw[3].reg == 0xA10D && tw[3].value == 0x01020304);
    CHECK(x.pm4_dwords == 3 + 3 + 4 && sb.valid == 0b100111 && sb.reg[2] == 0x12 && sb.stencil == 0x01020304);
    S::Writer again{tw, Render::kMaxStateWrites};
    S::TransitionTo(sb, bp, env, again);
    CHECK(again.n == 0 && again.pm4_dwords == 0);

    // Primitive type, index size; the state marker with a pseudo register.
    RegWrite pw[2];
    S::Writer p{pw, 2};
    S::SetPrimitiveType(0x44, p);
    S::SetIndexSize(1, 2, p);
    CHECK(p.n == 2 && pw[0].reg == 0xC242 && pw[0].value == 4 && pw[1].reg == 0xA29F && pw[1].value == 1 &&
          p.pm4_dwords == 3 + 2);
    S::Writer q{pw, 2};
    S::SetIndexSize(0, 1, q);
    CHECK(pw[0].value == (1u << 10 | 1u << 6));

    // The one-packet setters the material and mesh code call directly.
    const auto leaf = [&](u64 rva) -> const S::LeafSpec& {
        for (const S::LeafSpec& s : S::kLeaves) {
            if (s.rva == rva) {
                return s;
            }
        }
        return S::kLeaves[0];
    };
    RegWrite lw[2];
    S::Writer l1{lw, 2};
    S::LeafWrite(leaf(0x6228a0), 3, 0xFFFFFFFF, l1); // CB_BLEND3_CONTROL
    CHECK(l1.n == 1 && lw[0].reg == 0xA1E3 && lw[0].value == 0xFFFF1FFF && l1.pm4_dwords == 3);
    S::Writer l2{lw, 2};
    S::LeafWrite(leaf(0x6229a0), 0x11223344, 0, l2); // stencil reference/mask, front and back
    CHECK(l2.n == 2 && lw[0].reg == 0xA10C && lw[1].reg == 0xA10D && lw[1].value == 0x11223344 && l2.pm4_dwords == 4);
    S::Writer l3{lw, 2};
    S::LeafWrite(leaf(0x622b30), 0xF, 0xCC, l3); // CB_COLOR_CONTROL mode 7, ROP 0xCC
    CHECK(lw[0].reg == 0xA202 && lw[0].value == (7u << 4 | 0xCCu << 16));
    S::Writer l4{lw, 2};
    S::LeafWrite(leaf(0x626cc0), 3, 0, l4);
    CHECK(lw[0].reg == 0xA293 && lw[0].value == (1u << 16 | 0x6020000));
    S::Writer l5{lw, 2};
    S::LeafWrite(leaf(0x6250b0), 4, 0, l5);
    CHECK(lw[0].reg == 0xC24D && lw[0].value == 4 && l5.pm4_dwords == 2);
    S::Writer l6{lw, 2};
    S::LeafWrite(leaf(0x6221a0), 0xA003, 0x40, l6);
    CHECK(l6.n == 1 && lw[0].reg == 0xA003 && lw[0].value == 0x40 && !l6.overflow);
    S::Writer l7{lw, 2};
    S::LeafWrite(leaf(0x6221a0), 0x2C00, 1, l7); // not a context register: no native write
    CHECK(l7.overflow && l7.n == 0);
    S::Writer l8{lw, 2};
    S::LeafWrite(leaf(0x6270e0), 0xFFFFFFFF, 0, l8);
    CHECK(lw[0].reg == 0xA205 && lw[0].value == 0x393FFF);
    const RegWrite m[2] = {{0xA318, 7}, {Render::kStateDbExtent, 9}};
    u32 marker[Render::RegistersMarkerDwords(2)] = {};
    CHECK(Render::EncodeRegistersMarker(m, Render::kApply, marker, Render::OpKind::State) ==
          Render::RegistersMarkerDwords(2));
    RegWrite back[Render::kMaxStateWrites];
    u32 back_n = 0;
    u16 flags = 0;
    CHECK(Render::MarkerKind(marker) == Render::OpKind::State &&
          !Render::DecodeRegistersMarker(marker, back, back_n, flags) && // not a commit's register marker
          Render::DecodeRegistersMarker(marker, back, back_n, flags, Render::OpKind::State, Render::kMaxStateWrites) &&
          back_n == 2 && Render::IsStatePseudo(back[1].reg) && !Render::IsStatePseudo(back[0].reg));
}

} // namespace

int main(int argc, char** argv) {
    const bool bench = argc > 1 && std::strcmp(argv[1], "--bench") == 0;
    TestHash();
    TestOrder();
    TestIterators();
    TestWaitHelps();
    TestQuit();
    TestStress();
    TestEntryProbe();
    TestSmokeScale();
    TestDcbScan();
    TestPassGraph();
    TestNativeDrawShadow();
    TestNativeState();
    if (bench) {
        Bench();
    }
    std::printf("TOTAL %d checks, %d failures - %s\n", g_checks, g_failures, g_failures == 0 ? "PASS" : "FAIL");
    return g_failures;
}
