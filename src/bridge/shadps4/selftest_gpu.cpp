// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — self-test of SecondSonPC's GPU interfaces (Phase 4, P4.1; docs/PHASE4_P41_GPU_FOUNDATION.md §8):
//   Pm4Decode   — the PM4 decoder on synthetic packets, its opcode table against the bridge's (ABI of the header);
//   Pm4Stats    — the statistics on a synthetic command stream: counts, frames, the row limit, unknown packets, files;
//   GpuOwners   — the guest GPU's owner registry: one owner per resource, refusals, the backend chosen once, a race;
//   GpuMemory(contract) — SecondSonPC's GPU memory contract over the bridge's manager: only the owner registers, never
//                 beside the bridge's renderer, notifications forwarded in order, nothing after unregistering;
//   GpuVulkan   — the Vulkan foundation: create/destroy twice, a resource round trip, a bad device index, a missing
//                 extension, failures injected at the instance, the device and the resources — nothing left alive.
// The statistics run on their own instance (EnableForTest): the bridge's hooks stay off, so its GPU thread never
// reaches them.
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "app/config.h"
#include "bridge/shadps4/gpu_memory_host.h"
#include "bridge/shadps4/selftest_threads.h"
#include "core/gpu_memory.h"
#include "core/libraries/kernel/memory.h"
#include "runtime/gpu/backend.h"
#include "runtime/gpu/gpu_memory.h"
#include "runtime/gpu/inspector.h"
#include "runtime/gpu/pm4.h"
#include "runtime/gpu/pm4_stats.h"
#include "runtime/gpu/vulkan/vk_foundation.h"
#include "runtime/gpu/vulkan/vk_image_cache.h"
#include "video_core/amdgpu/pm4_cmds.h"

// Options structures are filled naming only the fields a case changes.
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"

namespace SecondSon::Bridge {

namespace {

namespace Pm4 = Gpu::Pm4;

/// A type-3 header with `body` dwords after it.
u32 H3(u8 opcode, u32 body) {
    return (3u << 30) | ((body - 1) << 16) | (u32{opcode} << 8);
}

void TestDecode(TestGroup& g) {
    Pm4::Packet p;
    Pm4::DecodeError e{};
    {
        const u32 s[] = {H3(Pm4::Op::Nop, 1), 0x68750776u};
        g.Check("NOP with the flip marker: type 3, 2 dwords, the marker recognised",
                Pm4::Decode(s, p, e) && p.type == 3 && p.opcode == Pm4::Op::Nop && p.dwords == 2 &&
                    Pm4::ClassifyNop(std::span<const u32>(s, 2)) == Pm4::NopMarker::PatchedFlip,
                fmt::format("dwords {}", p.dwords));
    }
    {
        const u32 one[] = {H3(Pm4::Op::Nop, 1), 0x1234};
        g.Check("NOP with one unknown payload dword: 'one-dword' (the size hints after target setups)",
                Pm4::ClassifyNop(std::span<const u32>(one, 2)) == Pm4::NopMarker::OneDword, "");
    }
    {
        const u32 s[] = {H3(Pm4::Op::SetContextReg, 4), 0x0005, 1, 2, 3};
        Pm4::RegSpace space{};
        g.Check("SET_CONTEXT_REG of 3 registers: 5 dwords, the context space",
                Pm4::Decode(s, p, e) && p.dwords == 5 && Pm4::SetRegSpace(p.opcode, space) &&
                    space == Pm4::RegSpace::Context,
                "");
    }
    {
        const u32 s[] = {0x80000000u};
        g.Check("type-2 padding: 1 dword", Pm4::Decode(s, p, e) && p.type == 2 && p.dwords == 1, "");
    }
    {
        // P4.2 regression: count 0x3FFF is the header-only form (the one-dword NOP filler), not 0x4001 dwords
        const AmdGpu::PM4Type3Header bridge{AmdGpu::PM4ItOpcode::Nop, 0x3FFF};
        const u32 s[] = {bridge.raw, H3(Pm4::Op::Nop, 1), 0};
        g.Check("a header-only packet (count 0x3FFF): 1 dword, as the bridge's NumWords",
                Pm4::Decode(s, p, e) && p.type == 3 && p.dwords == 1 && bridge.NumWords() + 1 == 1, "");
    }
    {
        const u32 s[] = {(1u << 16) | 0x1234u, 7, 8};
        g.Check("type-0: base register and 1 + count + 1 dwords",
                Pm4::Decode(s, p, e) && p.type == 0 && p.base == 0x1234 && p.dwords == 3, "");
    }
    {
        const u32 s[] = {H3(Pm4::Op::DrawIndex2, 5), 0, 0};
        g.Check("a packet longer than the stream: Truncated, nothing read beyond",
                !Pm4::Decode(s, p, e) && e == Pm4::DecodeError::Truncated, "");
        const u32 r[] = {0x40000000u};
        g.Check("type 1 (reserved): refused", !Pm4::Decode(r, p, e) && e == Pm4::DecodeError::ReservedType, "");
        g.Check("an empty stream: Empty", !Pm4::Decode({}, p, e) && e == Pm4::DecodeError::Empty, "");
    }
    g.Check("names and categories: DRAW_INDEX_2 is a draw, 0xFE is unknown",
            Pm4::OpcodeName(Pm4::Op::DrawIndex2) == "DRAW_INDEX_2" &&
                Pm4::Classify(Pm4::Op::DrawIndex2) == Pm4::Category::Draw && Pm4::OpcodeName(0xFE).empty() &&
                Pm4::Classify(0xFE) == Pm4::Category::Unknown,
            "");

    // ABI: every opcode of the bridge's table is known to SecondSonPC, and a header built by the bridge decodes to the
    // same opcode and length.
    using AmdGpu::PM4ItOpcode;
    const PM4ItOpcode bridge_ops[] = {
        PM4ItOpcode::Nop, PM4ItOpcode::SetBase, PM4ItOpcode::ClearState, PM4ItOpcode::IndexBufferSize,
        PM4ItOpcode::DispatchDirect, PM4ItOpcode::DispatchIndirect, PM4ItOpcode::AtomicGds, PM4ItOpcode::Atomic,
        PM4ItOpcode::OcclusionQuery, PM4ItOpcode::SetPredication, PM4ItOpcode::RegRmw, PM4ItOpcode::CondExec,
        PM4ItOpcode::PredExec, PM4ItOpcode::DrawIndirect, PM4ItOpcode::DrawIndexIndirect, PM4ItOpcode::IndexBase,
        PM4ItOpcode::DrawIndex2, PM4ItOpcode::ContextControl, PM4ItOpcode::IndexType, PM4ItOpcode::DrawIndirectMulti,
        PM4ItOpcode::DrawIndexAuto, PM4ItOpcode::NumInstances, PM4ItOpcode::DrawIndexMultiAuto,
        PM4ItOpcode::IndirectBufferConst, PM4ItOpcode::StrmoutBufferUpdate, PM4ItOpcode::DrawIndexOffset2,
        PM4ItOpcode::WriteData, PM4ItOpcode::DrawIndexIndirectMulti, PM4ItOpcode::MemSemaphore,
        PM4ItOpcode::WaitRegMem, PM4ItOpcode::IndirectBuffer, PM4ItOpcode::CopyData, PM4ItOpcode::CommandProcessorDma,
        PM4ItOpcode::PfpSyncMe, PM4ItOpcode::SurfaceSync, PM4ItOpcode::CondWrite, PM4ItOpcode::EventWrite,
        PM4ItOpcode::EventWriteEop, PM4ItOpcode::EventWriteEos, PM4ItOpcode::ReleaseMem, PM4ItOpcode::PreambleCntl,
        PM4ItOpcode::DmaData, PM4ItOpcode::ContextRegRmw, PM4ItOpcode::AcquireMem, PM4ItOpcode::Rewind,
        PM4ItOpcode::LoadShReg, PM4ItOpcode::LoadConfigReg, PM4ItOpcode::LoadContextReg, PM4ItOpcode::SetConfigReg,
        PM4ItOpcode::SetContextReg, PM4ItOpcode::SetContextRegIndirect, PM4ItOpcode::SetShReg,
        PM4ItOpcode::SetShRegOffset, PM4ItOpcode::SetQueueReg, PM4ItOpcode::SetUconfigReg, PM4ItOpcode::LoadConstRam,
        PM4ItOpcode::WriteConstRam, PM4ItOpcode::DumpConstRam, PM4ItOpcode::IncrementCeCounter,
        PM4ItOpcode::IncrementDeCounter, PM4ItOpcode::WaitOnCeCounter, PM4ItOpcode::WaitOnDeCounterDiff,
        PM4ItOpcode::GetLodStats, PM4ItOpcode::DrawIndexIndirectCountMulti};
    std::string bad;
    for (const PM4ItOpcode op : bridge_ops) {
        const AmdGpu::PM4Type3Header hdr{op, 4};
        std::vector<u32> s(hdr.NumWords() + 1, 0);
        s[0] = hdr.raw;
        const u8 code = static_cast<u8>(static_cast<u32>(op));
        if (!Pm4::Decode(s, p, e) || p.opcode != code || p.dwords != hdr.NumWords() + 1 ||
            Pm4::Classify(code) == Pm4::Category::Unknown || Pm4::OpcodeName(code).empty()) {
            bad += fmt::format(" {:#04x}", code);
        }
    }
    g.Check(fmt::format("the bridge's {} opcodes: known to SecondSonPC, headers built by the bridge decode alike",
                        std::size(bridge_ops)),
            bad.empty(), bad);
}

void TestStats(TestGroup& g, const std::filesystem::path& dir) {
    namespace S = Gpu::Pm4Stats;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    S::Test::EnableForTest({.dir = dir, .report_seconds = 0, .snapshot_seconds = 0, .max_frames = 4,
                            .exit_step = false});
    g.Check("the bridge's hooks stay off while the self-test has its own capture", !S::Enabled(), "");
    std::vector<u32> frame;
    const auto put = [&](std::initializer_list<u32> w) { frame.insert(frame.end(), w); };
    put({H3(Pm4::Op::SetContextReg, 4), 0x0200, 1, 2, 3});
    put({H3(Pm4::Op::SetShReg, 3), 0x0010, 4, 5});
    put({H3(Pm4::Op::DrawIndex2, 5), 0x1000, 0, 0, 300, 0});
    put({H3(Pm4::Op::NumInstances, 1), 2});
    put({H3(Pm4::Op::DispatchDirect, 4), 4, 2, 1, 1});
    put({H3(0xFE, 2), 0xAAAA, 0xBBBB});
    put({H3(Pm4::Op::SetContextReg, 4), 0x03FF, 7, 8, 9}); // one register inside the first 1024, two beyond
    put({H3(Pm4::Op::EventWriteEop, 5), 4, 0x1000, 2u << 29, 1, 0});              // type 4, data_sel 2
    put({H3(Pm4::Op::DmaData, 6), 3u << 29, 0x2000, 0, 0x3000, 0, 0x100});      // memory/L2 -> memory, 256 B
    put({H3(Pm4::Op::Nop, 1), 0x68750776u});
    for (int f = 0; f < 6; ++f) {
        std::span<const u32> s(frame);
        while (!s.empty()) {
            S::OnPacket(Pm4::Engine::Draw, s.data(), s.size());
            Pm4::Packet p;
            Pm4::DecodeError e{};
            Pm4::Decode(s, p, e);
            s = s.subspan(p.dwords);
        }
        S::OnSpan(S::Span::Idle, true);
        S::OnSpan(S::Span::Idle, false);
    }
    const u32 truncated[] = {H3(Pm4::Op::DrawIndex2, 5), 0};
    S::OnPacket(Pm4::Engine::Draw, truncated, 2);
    const auto t = S::Test::Take();
    g.Check("6 frames of 10 packets: packets, draws, dispatches, registers, flip markers counted",
            t.packets == 60 && t.draws == 6 && t.dispatches == 6 && t.context_regs == 36 && t.sh_regs == 12 &&
                t.frames == 6 && t.nop_markers[static_cast<size_t>(Pm4::NopMarker::PatchedFlip)] == 6 &&
                S::Test::OpcodeCount(Pm4::Engine::Draw, Pm4::Op::DrawIndex2) == 6,
            fmt::format("packets {} draws {} dispatches {} ctx {} sh {} frames {}", t.packets, t.draws, t.dispatches,
                        t.context_regs, t.sh_regs, t.frames));
    g.Check("an unknown opcode and a truncated packet are kept apart (one kind each), not guessed",
            t.unknown_kinds == 2 && t.decode_errors == 1, fmt::format("kinds {} errors {}", t.unknown_kinds,
                                                                      t.decode_errors));
    const bool written = S::WriteNow();
    std::ifstream csv(dir / "gpu_frames.csv");
    int lines = 0;
    for (std::string line; std::getline(csv, line);) {
        ++lines;
    }
    std::ifstream txt(dir / "gpu_stats.txt");
    const std::string text((std::istreambuf_iterator<char>(txt)), std::istreambuf_iterator<char>());
    g.Check("files written; frame rows stop at the limit (4 of 5 frames between flip markers, the 5th counted as "
            "beyond)",
            written && lines == 5 && text.find("DRAW_INDEX_2") != std::string::npos &&
                text.find("opcode 0xfe") != std::string::npos && text.find("1 frames beyond") != std::string::npos,
            fmt::format("csv lines {}", lines));
    g.Check("fields broken down: register writes beyond the first 1024, end-of-pipe event and data select, DMA "
            "source/destination",
            text.find("beyond the first 1024: 12 writes") != std::string::npos &&
                text.find("EVENT_WRITE_EOP: 4=6") != std::string::npos &&
                text.find("EVENT_WRITE_EOP data_sel: 2=6") != std::string::npos &&
                text.find("3->0=6") != std::string::npos,
            "see " + (dir / "gpu_stats.txt").string());
    S::Test::Reset();
    g.Check("Reset: nothing left of the capture (no counts, nothing to write)",
            S::Test::OpcodeCount(Pm4::Engine::Draw, Pm4::Op::DrawIndex2) == 0 && !S::WriteNow(), "");
}

/// The snapshot protocol with a command thread of its own: a reader asking while that thread sleeps (idle) gets the
/// counts as of its sleep at once; a reader asking while it works gets them at its next frame's end.
void TestStatsThreaded(TestGroup& g, const std::filesystem::path& dir) {
    namespace S = Gpu::Pm4Stats;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    S::Test::EnableForTest({.dir = dir, .report_seconds = 0, .snapshot_seconds = 0, .exit_step = false}, true);
    const u32 frame[] = {H3(Pm4::Op::DrawIndexAuto, 2), 3, 0, H3(Pm4::Op::Nop, 1), 0x68750776u};
    const auto feed = [&](int frames) {
        for (int f = 0; f < frames; ++f) {
            S::OnPacket(Pm4::Engine::Draw, frame, 5);
            S::OnPacket(Pm4::Engine::Draw, frame + 3, 2);
        }
    };
    // phases: 1 the command thread sleeps (idle), 2 the reader read, 3 the command thread works, 4 the reader is done
    std::atomic<int> phase{0};
    std::thread cp([&] {
        S::OnSpan(S::Span::Idle, false);
        feed(3);
        S::OnSpan(S::Span::Idle, true); // asleep: the counts so far are published
        phase = 1;
        while (phase.load() < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        S::OnSpan(S::Span::Idle, false);
        phase = 3;
        for (int f = 0; f < 500 && phase.load() < 4; ++f) { // working: frames until the reader is answered
            feed(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    while (phase.load() != 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto read_text = [&] {
        std::ifstream txt(dir / "gpu_stats.txt");
        return std::string((std::istreambuf_iterator<char>(txt)), std::istreambuf_iterator<char>());
    };
    auto t0 = std::chrono::steady_clock::now();
    const bool w1 = S::WriteNow();
    const double ms1 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const std::string a = read_text();
    g.Check("a reader while the command thread sleeps: answered at once with the counts as of its sleep",
            w1 && ms1 < 200 && a.find("snapshot current") != std::string::npos &&
                a.find("Totals: 6 packets,") != std::string::npos,
            fmt::format("{:.0f} ms", ms1));
    phase = 2;
    while (phase.load() < 3) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    t0 = std::chrono::steady_clock::now();
    const bool w2 = S::WriteNow();
    const double ms2 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    phase = 4;
    cp.join();
    const std::string b = read_text();
    g.Check("a reader while it works: answered at its next frame's end (more frames than before)",
            w2 && ms2 < 450 && b.find("snapshot current") != std::string::npos &&
                b.find("Totals: 6 packets,") == std::string::npos,
            fmt::format("{:.0f} ms", ms2));
    S::Test::Reset();
}

void TestOwners(TestGroup& g) {
    using Gpu::BackendKind;
    using Gpu::Resource;
    Gpu::Test::ResetRegistry();
    std::string err;
    g.Check("nobody owns the guest GPU before a claim", Gpu::Owner(Resource::CommandStream) == BackendKind::None, "");
    g.Check("the bridge claims the command stream", Gpu::Claim(Resource::CommandStream, BackendKind::Bridge, &err),
            err);
    err.clear();
    g.Check("SecondSonPC's backend cannot take it; the refusal names the owner",
            !Gpu::Claim(Resource::CommandStream, BackendKind::Sspc, &err) && err.find("bridge") != std::string::npos &&
                Gpu::Owner(Resource::CommandStream) == BackendKind::Bridge,
            err);
    g.Check("the owner claiming again is fine", Gpu::Claim(Resource::CommandStream, BackendKind::Bridge), "");
    g.Check("a release by someone else changes nothing",
            !Gpu::Release(Resource::CommandStream, BackendKind::Sspc) &&
                Gpu::Owner(Resource::CommandStream) == BackendKind::Bridge,
            "");
    g.Check("the owner releases; then the other backend may claim",
            Gpu::Release(Resource::CommandStream, BackendKind::Bridge) &&
                Gpu::Claim(Resource::CommandStream, BackendKind::Sspc) &&
                Gpu::Release(Resource::CommandStream, BackendKind::Sspc),
            "");
    g.Check("invalid claims: no owner, no such resource",
            !Gpu::Claim(Resource::WriteTracking, BackendKind::None) &&
                !Gpu::Claim(Resource::Count, BackendKind::Bridge) && !Gpu::Release(Resource::Count, BackendKind::Bridge),
            "");
    g.Check("the backend is chosen once: the same again is fine, another is refused",
            Gpu::SelectBackend(BackendKind::Bridge) && Gpu::SelectBackend(BackendKind::Bridge) &&
                !Gpu::SelectBackend(BackendKind::Sspc, &err) && Gpu::SelectedBackend() == BackendKind::Bridge,
            err);
    // a race: 8 threads, two backends, the same resource — one of them gets it, every claim of the other fails
    Gpu::Test::ResetRegistry();
    std::atomic<int> won[2] = {0, 0};
    std::atomic<int> lost[2] = {0, 0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&, i] {
            const int side = i % 2;
            const BackendKind k = side == 0 ? BackendKind::Bridge : BackendKind::Sspc;
            std::string refused; // the refusal is reported here, not logged
            for (int n = 0; n < 1000; ++n) {
                (Gpu::Claim(Resource::WriteTracking, k, &refused) ? won[side] : lost[side]).fetch_add(1);
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    const BackendKind owner = Gpu::Owner(Resource::WriteTracking);
    const int ow = owner == BackendKind::Bridge ? 0 : 1;
    g.Check("8 threads claim one resource for two backends: one owner, all of its claims succeed, none of the other's",
            owner != BackendKind::None && won[ow] == 4000 && lost[ow] == 0 && won[1 - ow] == 0 && lost[1 - ow] == 4000,
            fmt::format("bridge {}/{} sspc {}/{}", won[0].load(), lost[0].load(), won[1].load(), lost[1].load()));
    Gpu::Test::ResetRegistry();
}

class Recorder final : public Gpu::MemoryListener {
public:
    std::vector<std::tuple<char, u64, u64>> ev;
    void OnUnmap(u64 a, u64 s) override {
        ev.emplace_back('U', a, s);
    }
    void OnRegister(u64 a, u64 s) override {
        ev.emplace_back('R', a, s);
    }
    void OnMap(u64 a, u64 s) override {
        ev.emplace_back('M', a, s);
    }
    void OnGuestWrite(u64 a, u64 s) override {
        ev.emplace_back('W', a, s);
    }
    std::string Text() const {
        return EventsText(ev);
    }
    static std::string EventsText(const std::vector<std::tuple<char, u64, u64>>& events) {
        std::string t;
        for (const auto& [k, a, s] : events) {
            t += fmt::format(" {}({:#x},{:#x})", k, a, s);
        }
        return t;
    }
};

void TestMemoryContract(TestGroup& g) {
    using Gpu::BackendKind;
    using Gpu::Resource;
    InstallGpuGuestMemory();
    Gpu::GuestMemory* gm = Gpu::GetGuestMemory();
    g.Check("the contract is installed over the selected manager", gm != nullptr, "");
    if (gm == nullptr) {
        return;
    }
    constexpr u64 kMax = 1ull << 40;
    g.Check("40-bit GPU ranges: whole space valid, past the end and wrapping invalid",
            Gpu::GuestMemory::IsValidGpuMapping(0, kMax) && Gpu::GuestMemory::IsValidGpuMapping(kMax - 0x1000, 0x1000) &&
                !Gpu::GuestMemory::IsValidGpuMapping(kMax, 0) && !Gpu::GuestMemory::IsValidGpuMapping(kMax - 1, 2) &&
                !Gpu::GuestMemory::IsValidGpuMapping(~0ull - 0x10, 0x100),
            "");
    Gpu::Test::ResetRegistry();
    Recorder rec;
    std::string err;
    g.Check("no claim: SecondSonPC's listener is refused", !gm->SetListener(&rec, BackendKind::Sspc, &err), err);
    Gpu::Claim(Resource::MemoryListener, BackendKind::Sspc);
    err.clear();
    const bool beside = gm->SetListener(&rec, BackendKind::Sspc, &err);
    auto& core = Core::GetGpuMemory();
    Core::GpuMemoryListener* const renderer = core.SetListener(nullptr); // the bridge's rasterizer of this process
    g.Check("with the claim but the bridge's renderer registered: refused, the renderer stays the listener",
            !beside && renderer != nullptr, err);
    err.clear();
    g.Check("the slot free, but the claim is SecondSonPC's: the bridge's kind is refused",
            !gm->SetListener(&rec, BackendKind::Bridge, &err), err);
    err.clear();
    const bool registered = gm->SetListener(&rec, BackendKind::Sspc, &err);
    g.Check("the owner registers its listener", registered, err);
    bool renderer_back = false;
    if (registered) {
        namespace K = Libraries::Kernel;
        void* addr = nullptr;
        const s32 rm = K::sceKernelMapNamedFlexibleMemory(&addr, 0x10000, 3, 0, "gpu contract test");
        const auto map_ev = rec.ev;
        rec.ev.clear();
        const u64 a = reinterpret_cast<u64>(addr);
        const auto is = [&](const std::tuple<char, u64, u64>& e, char k) {
            return std::get<0>(e) == k && std::get<1>(e) <= a && std::get<1>(e) + std::get<2>(e) >= a + 0x10000;
        };
        bool map_ok = rm == 0 && !map_ev.empty() && is(map_ev.back(), 'M');
        for (size_t i = 0; i + 1 < map_ev.size(); ++i) {
            map_ok &= std::get<0>(map_ev[i]) != 'M'; // the map comes last
        }
        g.Check("map: forwarded to SecondSonPC's listener, OnMap of the range last",
                map_ok, fmt::format("rc {} at {:#x}:{}", rm, a, Recorder::EventsText(map_ev)));
        const s32 ru = rm == 0 ? K::sceKernelMunmap(addr, 0x10000) : -1;
        const auto un_ev = rec.ev;
        rec.ev.clear();
        g.Check("munmap: OnUnmap of the range first, then the register of the freed pages",
                ru == 0 && un_ev.size() >= 2 && is(un_ev.front(), 'U') && std::get<0>(un_ev[1]) == 'R',
                fmt::format("rc {}:{}", ru, Recorder::EventsText(un_ev)));
        Recorder second;
        const bool switched = gm->SetListener(&second, BackendKind::Sspc, &err);
        void* s2 = nullptr;
        const s32 rs = switched ? K::sceKernelMapNamedFlexibleMemory(&s2, 0x10000, 3, 0, "gpu contract test 2") : -1;
        if (rs == 0) {
            K::sceKernelMunmap(s2, 0x10000);
        }
        g.Check("the owner switches its listener: the next map and unmap reach the new one only",
                switched && rs == 0 && !second.ev.empty() && rec.ev.empty(),
                fmt::format("rc {}, new{}, old{}", rs, second.Text(), rec.Text()));
        second.ev.clear();
        g.Check("the owner unregisters", gm->SetListener(nullptr, BackendKind::Sspc, &err), err);
        // The bridge's manager calls its listener at every map: the adapter left a placeholder, not an empty slot.
        Core::GpuMemoryListener* const placeholder = core.SetListener(renderer); // the bridge's renderer back
        renderer_back = true;
        void* again = nullptr;
        const s32 ra = K::sceKernelMapNamedFlexibleMemory(&again, 0x10000, 3, 0, "gpu contract test 3");
        if (ra == 0) {
            K::sceKernelMunmap(again, 0x10000);
        }
        g.Check("after unregistering nothing reaches SecondSonPC's listeners; the slot held a placeholder, never "
                "nothing",
                ra == 0 && rec.ev.empty() && second.ev.empty() && placeholder != nullptr && placeholder != renderer,
                fmt::format("rc {}{}{}", ra, rec.Text(), second.Text()));
    }
    if (!renderer_back) {
        core.SetListener(renderer); // the bridge's renderer back
    }
    Gpu::Release(Resource::MemoryListener, BackendKind::Sspc);
    Gpu::Test::ResetRegistry();
}

void TestVulkan(TestGroup& g) {
    namespace V = Gpu::Vk;
    const s64 live0 = V::LiveObjects();
    g.Check("no Vulkan object of SecondSonPC alive before", live0 == 0, fmt::format("{}", live0));
    V::Report report;
    std::string err;
    auto f = V::Foundation::Create({}, report, err);
    if (!f && report.loader.empty()) {
        g.Info("no Vulkan loader on this machine: the foundation's checks are skipped (" + err + ")");
        return;
    }
    g.Check("level A: instance, a suitable device and its graphics queue",
            f != nullptr && f->Selected().suitable && f->Selected().graphics_family != ~0u && V::LiveObjects() == 2,
            f ? fmt::format("{} ({}), live {}", f->Selected().name, f->Selected().type, V::LiveObjects()) : err);
    if (f) {
        g.Info(fmt::format("GpuVulkan: {} ({}, {} MiB device-local), queues: {}", f->Selected().name,
                           f->Selected().type, f->Selected().device_local_bytes >> 20, fmt::join(report.queues, ", ")));
        std::string detail;
        g.Check("a buffer and an image created, the image cleared and read back, both destroyed",
                f->CheckResources(0x80402010u, detail) && V::LiveObjects() == 2, detail);
        f.reset();
    }
    g.Check("destroyed: nothing alive", V::LiveObjects() == 0, fmt::format("{}", V::LiveObjects()));
    {
        V::Report r2;
        auto f2 = V::Foundation::Create({}, r2, err);
        g.Check("created and destroyed a second time in the same process", f2 != nullptr, err);
        f2.reset();
        g.Check("... nothing alive", V::LiveObjects() == 0, "");
    }
    {
        V::Report r;
        err.clear();
        auto bad = V::Foundation::Create({.device_index = 999}, r, err);
        g.Check("a device index out of range: refused with the count, nothing alive",
                !bad && err.find("999") != std::string::npos && V::LiveObjects() == 0, err);
    }
    {
        V::Report r;
        err.clear();
        auto bad = V::Foundation::Create({.extra_device_extensions = {"VK_SSPC_no_such_extension"}}, r, err);
        g.Check("a required extension no device has: refused naming it, nothing alive",
                !bad && err.find("VK_SSPC_no_such_extension") != std::string::npos && V::LiveObjects() == 0, err);
    }
    for (const auto [point, name] : {std::pair{V::FailPoint::Instance, "instance"},
                                     std::pair{V::FailPoint::Device, "device"}}) {
        V::Report r;
        err.clear();
        auto bad = V::Foundation::Create({.fail_at = point}, r, err);
        g.Check(fmt::format("the {} creation fails: refused, everything created before destroyed", name),
                !bad && err.find("injected") != std::string::npos && V::LiveObjects() == 0, err);
    }
    {
        V::Report r;
        err.clear();
        auto f3 = V::Foundation::Create({.fail_at = V::FailPoint::Resources}, r, err);
        std::string detail;
        const bool res = f3 != nullptr && f3->CheckResources(0x01020304u, detail);
        const bool partial_clean = f3 != nullptr && V::LiveObjects() == 2;
        f3.reset();
        g.Check("a resource allocation fails half-way: the step fails, its objects are destroyed, the device stays",
                !res && detail.find("injected") != std::string::npos && partial_clean && V::LiveObjects() == 0,
                detail);
    }
}

/// The native backend's image cache rules (P4.5; vk_image_cache.h), without a device.
void TestImageCacheRules(TestGroup& g) {
    namespace V = Gpu::Vk;
    using V::InvalKind;
    constexpr u64 K = 64 * 1024;
    // invalidation: an image over [base, base + size)
    const auto inval = [](InvalKind kind, u64 base, u64 size, bool gpu, bool uploaded, u64 a, u64 n) {
        return V::ClassifyInvalidation(kind, base, size, gpu, uploaded, a, a + n);
    };
    const auto none = [](const V::InvalidationEffect& e) {
        return !e.retire && !e.dirty && !e.memory_newer && !e.maybe_newer;
    };
    g.Check("invalidation: a write beside the image changes nothing",
            none(inval(InvalKind::Exact, 0x100000, 0x40000, true, false, 0x140000, 0x1000)) &&
                none(inval(InvalKind::Unmap, 0x100000, 0x40000, true, false, 0x0, 0x100000)),
            "");
    g.Check("invalidation: an unmap/remap over it retires it, texture or GPU-written (R4)",
            inval(InvalKind::Unmap, 0x100000, 0x40000, true, false, 0x13F000, 0x2000).retire &&
                inval(InvalKind::Unmap, 0x100000, 0x40000, false, true, 0x100000, 0x1000).retire,
            "");
    g.Check("invalidation: an uploaded texture is uploaded again after any write over it; never-uploaded: nothing",
            inval(InvalKind::Coarse, 0x100000, 0x40000, false, true, 0x100000, K).dirty &&
                inval(InvalKind::BufferWrite, 0x100000, 0x40000, false, true, 0x120000, 16).dirty &&
                none(inval(InvalKind::Exact, 0x100000, 0x40000, false, false, 0x100000, 16)),
            "");
    g.Check("invalidation: a GPU-written image whose memory is written exactly (CPU write seen at a met wait, DMA) "
            "takes memory's texels (address reuse, R3)",
            inval(InvalKind::Exact, 0x100000, 0x40000, true, false, 0x120010, 4).memory_newer, "");
    g.Check("invalidation: a shader's buffer write marks a GPU-written image only at its base (the bridge's "
            "InvalidateMemoryFromGPU)",
            inval(InvalKind::BufferWrite, 0x100000, 0x40000, true, false, 0x100000, 256).memory_newer &&
                none(inval(InvalKind::BufferWrite, 0x100000, 0x40000, true, false, 0x110000, 256)),
            "");
    const auto inner = inval(InvalKind::Coarse, 0x100000, 0x40000, true, false, 0x110000, K);
    const auto shared = inval(InvalKind::Coarse, 0x108000, 0x10000, true, false, 0x100000, K);
    g.Check("invalidation: the tracker's 64 KiB block wholly inside a GPU-written image -> memory newer; a block it "
            "shares with a neighbour -> maybe newer only (its texels kept)",
            inner.memory_newer && !inner.maybe_newer && !shared.memory_newer && shared.maybe_newer, "");

    // ExpandImage sources
    V::CachedImage have;
    have.gpu_written = true;
    have.layout.supported = true;
    have.layout.pixel_format = 97; // R16G16B16A16_SFLOAT
    have.layout.num_bits = 64;
    have.layout.size = {256, 256, 1};
    have.layout.pitch = 256;
    have.layout.tile_mode = static_cast<Gpu::TileMode>(14);
    Gpu::ImageLayout want = have.layout;
    want.levels = 9;
    g.Check("ExpandImage: a GPU-written image serves a request for more levels of the same texels (R5)",
            V::CanExpandFrom(have, want), "");
    want.levels = 1;
    want.layers = 1;
    g.Check("ExpandImage: nothing more asked for: not an expansion", !V::CanExpandFrom(have, want), "");
    want.levels = 9;
    V::CachedImage stale = have;
    stale.memory_newer = true;
    V::CachedImage maybe = have;
    maybe.maybe_newer = true;
    V::CachedImage uploaded = have;
    uploaded.gpu_written = false;
    g.Check("ExpandImage: not from an image whose memory is (maybe) newer, nor from an uploaded texture",
            !V::CanExpandFrom(stale, want) && !V::CanExpandFrom(maybe, want) && !V::CanExpandFrom(uploaded, want), "");
    Gpu::ImageLayout other = want;
    other.size = {128, 256, 1};
    Gpu::ImageLayout msaa = want;
    msaa.num_samples = 4;
    g.Check("ExpandImage: another size or MSAA: no source", !V::CanExpandFrom(have, other) && !V::CanExpandFrom(have, msaa),
            "");
    Gpu::ImageLayout bc = have.layout;
    bc.pixel_format = 131; // BC1_RGB_UNORM: 64-bit blocks
    bc.props.is_block = true;
    bc.size = {1024, 1024, 1};
    bc.pitch = 1024;
    g.Check("ExpandImage: a block-compressed view of uncompressed texels (one 64-bit texel per BC1 block, R6)",
            V::CanExpandFrom(have, bc), fmt::format("{}x{} vs {}x{}", have.layout.BlockDim()[0], have.layout.BlockDim()[1],
                                                    bc.BlockDim()[0], bc.BlockDim()[1]));

    // the equal-address rule
    Gpu::ImageLayout rgba = have.layout;
    rgba.pixel_format = 37; // R8G8B8A8_UNORM
    rgba.num_bits = 32;
    Gpu::ImageLayout srgb = rgba;
    srgb.pixel_format = 43; // R8G8B8A8_SRGB
    Gpu::ImageLayout more = rgba;
    more.levels = 4;
    g.Check("equal-address rule: an image serves a view of a compatible format; not a larger request",
            V::ImageServes(rgba, srgb) && !V::ImageServes(rgba, more) && V::ImageServes(more, rgba), "");

    // the subresource rule: a slice rendered as a target of its own goes into the GPU-written array (First Light's
    // 4-slice screen mask kept the main menu's slices: silhouettes)
    V::CachedImage slice0, slice1, array4;
    for (V::CachedImage* c : {&slice0, &slice1, &array4}) {
        c->gpu_written = true;
        c->layout = rgba;
    }
    slice1.layout.layers = 2;
    array4.layout.layers = 4;
    g.Check("subresource rule: the GPU-written array serves a slice's target over the slice's own image",
            V::ServesBetter(array4, &slice0, slice0.layout) && V::ServesBetter(array4, &slice1, slice1.layout) &&
                V::ServesBetter(array4, nullptr, slice0.layout),
            "");
    V::CachedImage uploaded4 = array4;
    uploaded4.gpu_written = false;
    V::CachedImage superseded4 = array4;
    superseded4.superseded = true;
    V::CachedImage other4 = array4;
    other4.layout.size = {128, 256, 1};
    g.Check("subresource rule: not an uploaded or superseded array, not another size, not a smaller or equal image",
            !V::ServesBetter(uploaded4, &slice0, slice0.layout) && !V::ServesBetter(superseded4, &slice0, slice0.layout) &&
                !V::ServesBetter(other4, &slice0, slice0.layout) && !V::ServesBetter(slice0, &slice1, slice1.layout) &&
                !V::ServesBetter(array4, &array4, array4.layout) && !V::ServesBetter(slice1, &array4, slice0.layout),
            "");

    // eviction of GPU-written images (R7)
    using E = V::GpuEviction;
    const auto ev = [](V::GpuEvictionCandidate c) { return V::ClassifyGpuEviction(c); };
    g.Check("GPU eviction: in flight -> kept, even when stale",
            ev({.in_flight = true, .memory_newer = true, .age_frames = 1000}) == E::Keep, "");
    g.Check("GPU eviction: memory newer / superseded -> dropped once unused for kGpuEvictStaleFrames (nothing lost)",
            ev({.memory_newer = true, .age_frames = V::kGpuEvictStaleFrames - 1}) == E::Keep &&
                ev({.memory_newer = true, .age_frames = V::kGpuEvictStaleFrames}) == E::Drop &&
                ev({.superseded = true, .age_frames = 500}) == E::Drop,
            "");
    g.Check("GPU eviction: clean -> written back once unused for kGpuEvictCleanFrames, never dropped with its texels",
            ev({.age_frames = V::kGpuEvictCleanFrames - 1}) == E::Keep &&
                ev({.age_frames = V::kGpuEvictCleanFrames}) == E::WriteBack,
            "");
    g.Check("GPU eviction: maybe newer in memory, or no write-back path -> kept (no old texels over the CPU's bytes)",
            ev({.maybe_newer = true, .age_frames = 1000}) == E::Keep &&
                ev({.no_write_path = true, .age_frames = 1000}) == E::Keep,
            "");
    g.Check("GPU eviction: CPU writes to its memory not seen (untracked) -> kept, never written back (review M1b)",
            ev({.untracked = true, .age_frames = 1000}) == E::Keep, "");
    g.Check("GPU eviction: another GPU-written image over its memory used later -> dropped (that one holds the latest "
            "texels); one used earlier -> kept, not written back over it (review M1a)",
            ev({.newer_overlap = true, .age_frames = V::kGpuEvictStaleFrames}) == E::Drop &&
                ev({.older_overlap = true, .age_frames = 1000}) == E::Keep,
            "");
}

/// P4.8: the Performance Inspector's effect catalog and the target rules (classification never by pipeline number).
void TestInspectorCatalog(TestGroup& g) {
    namespace I = Gpu::Inspector;
    I::Catalog c;
    std::string first;
    const u32 bad = c.Parse("# comment\n"
                            "vs 7d22e10251c65380 Smoke confirmed  smoke VS\n"
                            "ps 1a4f64edc772a286 Smoke confirmed\n"
                            "cs a99b7ef49eca794  ParticleSimulation shared  15 hex digits\r\n"
                            "ps 1111 Foliage probable\n"
                            "vs 2222 Geometry rule\n"        // rule is not a catalog basis
                            "xs 3333 Smoke confirmed\n"      // no such stage
                            "vs zz Smoke confirmed\n"        // not hex
                            "cs 4444 NoSuchCategory probable\n",
                            &first);
    g.Check("catalog: 4 entries parsed, 4 malformed lines counted (basis rule, stage, hex, category), comments skipped",
            c.Size() == 4 && bad == 4 && first.starts_with("vs 2222"),
            fmt::format("size {} bad {} first '{}'", c.Size(), bad, first));
    const I::Class* cs = c.Find('c', 0xa99b7ef49eca794ull);
    g.Check("catalog: a compute entry by its content hash, the shared basis",
            cs != nullptr && cs->category == I::Category::ParticleSimulation && cs->basis == I::Basis::Shared, "");
    const I::DrawShape shadow{.colors = 0, .depth = true, .width = 1024, .height = 1024};
    const I::DrawShape gbuf{.colors = 5, .depth = true, .width = 1920, .height = 1080};
    const I::DrawShape post{.colors = 1, .depth = false, .width = 1920, .height = 1080, .count = 3};
    const I::DrawShape prepass{.colors = 0, .depth = true, .width = 1920, .height = 1080};
    const I::DrawShape plain{.colors = 1, .depth = true, .width = 1920, .height = 1080, .count = 300, .indexed = true};
    g.Check("catalog: a draw takes its PS entry before its VS entry, the VS entry without a PS entry, a rule otherwise",
            c.Draw(0x7d22e10251c65380ull, 0x1111, true, shadow).category == I::Category::Foliage &&
                c.Draw(0x7d22e10251c65380ull, 0x9999, true, shadow).category == I::Category::Smoke &&
                c.Draw(0x7d22e10251c65380ull, 0x1111, false, shadow).category == I::Category::Smoke &&
                c.Draw(0x5555, 0x6666, true, shadow).basis == I::Basis::Rule,
            "");
    g.Check("rules: square depth-only target = shadows, depth-only otherwise and 3+ colour targets = geometry, a small "
            "full-screen draw = post-processing, anything else unknown",
            I::RuleForDraw(shadow).category == I::Category::Shadows &&
                I::RuleForDraw(gbuf).category == I::Category::Geometry &&
                I::RuleForDraw(prepass).category == I::Category::Geometry &&
                I::RuleForDraw(post).category == I::Category::PostProcessing &&
                I::RuleForDraw(plain).category == I::Category::Unknown && I::RuleForDraw(plain).basis == I::Basis::None,
            "");
    g.Check("catalog: a dispatch without an entry is ComputeOther by rule",
            c.Dispatch(0x7777).category == I::Category::ComputeOther && c.Dispatch(0x7777).basis == I::Basis::Rule, "");
    bool names = true;
    for (size_t i = 0; i < I::kCategories; ++i) {
        I::Category back{};
        names = names && I::CategoryFromName(I::CategoryName(static_cast<I::Category>(i)), back) &&
                back == static_cast<I::Category>(i);
    }
    g.Check("categories: every name maps back to its category", names, "");
}

} // namespace

std::vector<TestGroup> TestGpu(const App::Config& cfg) {
    std::vector<TestGroup> out(7);
    out[6].name = "Inspector(catalog)";
    TestInspectorCatalog(out[6]);
    out[0].name = "Pm4Decode";
    out[1].name = "Pm4Stats";
    out[2].name = "GpuOwners";
    out[3].name = "GpuMemory(contract)";
    out[4].name = "GpuVulkan";
    out[5].name = "GpuImageCache(rules)";
    TestImageCacheRules(out[5]);
    TestDecode(out[0]);
    TestStats(out[1], cfg.logs_dir / "selftest_gpu");
    TestStatsThreaded(out[1], cfg.logs_dir / "selftest_gpu" / "threaded");
    TestOwners(out[2]);
    TestMemoryContract(out[3]);
    TestVulkan(out[4]);
    return out;
}

} // namespace SecondSon::Bridge
