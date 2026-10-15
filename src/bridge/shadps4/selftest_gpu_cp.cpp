// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — self-test of SecondSonPC's GPU command processor (Phase 4, P4.2; docs/PHASE4_P42_COMMAND_PROCESSOR.md
// §9). The processor runs on its own (Mode::Isolated) over a test memory and a GPU that records what it gets, and in
// Mode::Shadow over the same streams; nothing of the bridge's renderer is involved. The bridge's headers serve as the
// oracle of the hardware's layouts (packet fields, register indices, the clear state).
//   GpuCp(registers) — the register file: groups, change tracking, windows, clear state, the bridge's register indices;
//   GpuCp(packets)   — every opcode the game sends (and the neighbours the processor takes), field by field;
//   GpuCp(sync)      — fences after GPU work, waits, write ordering, interrupts;
//   GpuCp(state)     — draw state: what changes a pipeline key, dynamic state, targets; graphics vs compute;
//   GpuCp(errors)    — truncated/short/unknown packets, bad indirect buffers, loops, limits, faults and reset;
//   GpuCp(rings)     — compute rings: spans, wrap, a packet cut by the ring's end, the read pointer;
//   GpuCp(trace)     — the trace format and the replay, a synthetic trace and the recorded ones of the game.
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "app/config.h"
#include "bridge/shadps4/selftest_threads.h"
#include "runtime/gpu/command_processor.h"
#include "runtime/gpu/cp_registers.h"
#include "runtime/gpu/cp_replay.h"
#include "runtime/gpu/cp_validation.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/amdgpu/regs.h"

// Options and records are filled naming only the fields a case changes.
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"

namespace SecondSon::Bridge {

namespace {

namespace Cp = Gpu::Cp;
namespace Op = Gpu::Pm4::Op;
using Cp::Group;
using Cp::StepStatus;

/// A type-3 header with `body` dwords after it (0: the header-only form, count 0x3FFF).
u32 H3(u8 opcode, u32 body) {
    return (3u << 30) | ((body == 0 ? 0x3FFFu : body - 1) << 16) | (u32{opcode} << 8);
}

/// A PM4 stream under construction.
struct Stream {
    std::vector<u32> d;
    Stream& P(u8 op, std::initializer_list<u32> body) {
        d.push_back(H3(op, static_cast<u32>(body.size())));
        d.insert(d.end(), body);
        return *this;
    }
    Stream& Sh(u32 reg, std::initializer_list<u32> values) {
        std::vector<u32> b{reg - 0x2C00};
        b.insert(b.end(), values);
        d.push_back(H3(Op::SetShReg, static_cast<u32>(b.size())));
        d.insert(d.end(), b.begin(), b.end());
        return *this;
    }
    Stream& Ctx(u32 reg, std::initializer_list<u32> values) {
        std::vector<u32> b{reg - 0xA000};
        b.insert(b.end(), values);
        d.push_back(H3(Op::SetContextReg, static_cast<u32>(b.size())));
        d.insert(d.end(), b.begin(), b.end());
        return *this;
    }
    Stream& Ucfg(u32 reg, std::initializer_list<u32> values) {
        std::vector<u32> b{reg - 0xC000};
        b.insert(b.end(), values);
        d.push_back(H3(Op::SetUconfigReg, static_cast<u32>(b.size())));
        d.insert(d.end(), b.begin(), b.end());
        return *this;
    }
    Stream& Draw(u32 count, u64 index_base = 0x10080000) {
        return P(Op::DrawIndex2, {0x1000, static_cast<u32>(index_base), static_cast<u32>(index_base >> 32), count, 0});
    }
    u64 Bytes() const {
        return d.size() * 4;
    }
};

/// 1 MiB of guest memory at 0x10000000.
struct TestMem final : Cp::CommandMemory {
    static constexpr u64 kBase = 0x10000000;
    std::vector<u8> bytes = std::vector<u8>(1 << 20);
    u64 reads = 0, writes = 0;
    bool In(u64 a, u64 n) const {
        return a >= kBase && a - kBase <= bytes.size() && n <= bytes.size() - (a - kBase);
    }
    bool Read(u64 a, void* dst, u64 n) override {
        if (!In(a, n)) {
            return false;
        }
        std::memcpy(dst, bytes.data() + (a - kBase), n);
        ++reads;
        return true;
    }
    bool Write(u64 a, const void* src, u64 n) override {
        if (!In(a, n)) {
            return false;
        }
        std::memcpy(bytes.data() + (a - kBase), src, n);
        ++writes;
        return true;
    }
    u32 U32(u64 a) const {
        u32 v = 0;
        std::memcpy(&v, bytes.data() + (a - kBase), 4);
        return v;
    }
    u64 U64(u64 a) const {
        u64 v = 0;
        std::memcpy(&v, bytes.data() + (a - kBase), 8);
        return v;
    }
    void Put(u64 a, const std::vector<u32>& d) {
        std::memcpy(bytes.data() + (a - kBase), d.data(), d.size() * 4);
    }
    void Put32(u64 a, u32 v) {
        std::memcpy(bytes.data() + (a - kBase), &v, 4);
    }
};

struct Rec final : Cp::GpuWork {
    std::vector<Cp::DrawDescriptor> draws;
    std::vector<Cp::DispatchDescriptor> dispatches;
    std::vector<Cp::FenceOp> fences;
    std::vector<Cp::DmaOp> dmas;
    std::vector<Cp::CacheActionOp> cache;
    std::vector<Cp::FlipMarker> flips;
    u32 irqs = 0;
    void Draw(const Cp::DrawDescriptor& d) override {
        draws.push_back(d);
    }
    void Dispatch(const Cp::DispatchDescriptor& d) override {
        dispatches.push_back(d);
    }
    void CacheAction(const Cp::CacheActionOp& c) override {
        cache.push_back(c);
    }
    void Flip(const Cp::FlipMarker& f) override {
        flips.push_back(f);
    }
    void Fence(const Cp::FenceOp& f) override {
        fences.push_back(f);
    }
    void Dma(const Cp::DmaOp& d) override {
        dmas.push_back(d);
    }
    void Interrupt(u32, Cp::FenceKind) override {
        ++irqs;
    }
};

constexpr u64 kCmd = TestMem::kBase + 0x1000;   ///< command buffers
constexpr u64 kIb = TestMem::kBase + 0x40000;   ///< indirect buffers
constexpr u64 kData = TestMem::kBase + 0x80000; ///< data, labels

/// A processor over its own memory and recorder.
struct Rig {
    TestMem mem;
    Rec rec;
    Cp::CommandProcessor cp;
    explicit Rig(Cp::Mode mode = Cp::Mode::Isolated, u64 (*clock)(void*, u32) = nullptr)
        : cp(Cp::Options{.mode = mode, .memory = &mem, .work = &rec, .clock = clock}) {}
    /// The stream at kCmd as one graphics submission, processed until idle/blocked/faulted.
    StepStatus Gfx(const Stream& s, u64 at = kCmd) {
        mem.Put(at, s.d);
        cp.SubmitGraphics(at, static_cast<u32>(s.d.size()));
        return cp.Run(Cp::kGraphicsQueue);
    }
};

u64 FixedClock(void*, u32 data_sel) {
    return data_sel == 3 ? 0x1111222233334444ull : 0x5555666677778888ull;
}

// ---------------------------------------------------------------------------------------------------------------

void TestRegisters(TestGroup& g) {
    // every register of the windows in one group, the groups' lists complete
    size_t listed = 0;
    for (u32 i = 0; i < static_cast<u32>(Group::Count); ++i) {
        listed += Cp::GroupRegisters(static_cast<Group>(i)).size();
    }
    bool all = listed == Cp::kNumRegs;
    for (u32 i = 0; i < Cp::kNumRegs && all; ++i) {
        all = Cp::GroupOf(Cp::RegisterOfIndex(i)) < Group::Count;
    }
    g.Check("every register of the four windows belongs to exactly one group", all, fmt::format("{} listed", listed));
    g.Check("groups of known registers: programs, user data, CB format vs address, raster, primitive type, viewport",
            Cp::GroupOf(0x2C08) == Group::PsProgram && Cp::GroupOf(0x2C4C) == Group::VsUserData &&
                Cp::GroupOf(0xA318) == Group::ColorTarget && Cp::GroupOf(0xA31C) == Group::ColorFormat &&
                Cp::GroupOf(0xA205) == Group::Raster && Cp::GroupOf(0xC242) == Group::PrimitiveType &&
                Cp::GroupOf(0xA10F) == Group::Viewport && Cp::GroupOf(0xA10C) == Group::StencilRef &&
                Cp::GroupOf(0xA1E0) == Group::Blend && Cp::GroupOf(0xC24C) == Group::DrawParams &&
                Cp::GroupOf(0x1234) == Group::Count,
            "");

    Cp::RegisterFile rf;
    rf.ClearDirty(~Cp::GroupMask{0});
    const u32 v0 = rf.Version(Group::Blend);
    const u32 blend = 0x12345678;
    rf.Write(Cp::Space::Context, 0x1E0, &blend, 1);
    g.Check("a new value: its group dirty, its version +1, one change counted",
            rf.Dirty() == Cp::Bit(Group::Blend) && rf.Version(Group::Blend) == v0 + 1 && rf.Changes() == 1, "");
    rf.ClearDirty(~Cp::GroupMask{0});
    rf.Write(Cp::Space::Context, 0x1E0, &blend, 1);
    g.Check("the same value again: nothing dirty, no new version (a write is not a change)",
            rf.Dirty() == 0 && rf.Version(Group::Blend) == v0 + 1 && rf.Writes() == 2 && rf.Changes() == 1, "");
    const u32 two[2] = {1, 2};
    g.Check("a range leaving the window (SH 0x3FF + 2): refused, nothing written",
            !rf.Write(Cp::Space::Sh, 0x3FF, two, 2) && rf.Get(0x2FFF) == 0, "");
    g.Check("an offset past the window: refused", !rf.Write(Cp::Space::Context, 0x400, two, 1), "");
    const u32 sh = 0xABCD;
    rf.Write(Cp::Space::Sh, 0x4C, &sh, 1);
    rf.ClearContext();
    std::array<u32, 0x400> def{};
    Cp::ContextDefaults(def);
    bool ctx_ok = true;
    for (u32 i = 0; i < 0x400; ++i) {
        ctx_ok &= rf.Get(0xA000 + i) == def[i];
    }
    g.Check("CLEAR_STATE: the context back to its defaults, the SH registers kept (the hardware's clear state)",
            ctx_ok && rf.Get(0x2C4C) == 0xABCD, "");

    // the clear state equals the bridge's (fpPS4's table)
    AmdGpu::Regs bregs{};
    bregs.SetDefaults();
    bool same = true;
    for (u32 i = 0; i < 0x400; ++i) {
        same &= bregs.reg_array[0xA000 + i] == def[i];
    }
    g.Check("the context defaults equal the bridge's Regs::SetDefaults", same, "");

    // the register indices the processor names are the bridge's (hardware) ones
    using R = AmdGpu::Regs;
    const auto idx = [](size_t off) { return static_cast<u32>(off / 4); };
    g.Check("register indices: draw initiator, VGT_DMA_*, primitive type, num indices/instances, stages, programs",
            idx(offsetof(R, draw_initiator)) == Cp::Reg::VgtDrawInitiator &&
                idx(offsetof(R, index_base_address)) == Cp::Reg::VgtDmaBaseHi &&
                idx(offsetof(R, max_index_size)) == Cp::Reg::VgtDmaMaxSize &&
                idx(offsetof(R, index_size)) == Cp::Reg::VgtDmaSize &&
                idx(offsetof(R, index_buffer_type)) == Cp::Reg::VgtDmaIndexType &&
                idx(offsetof(R, primitive_type)) == Cp::Reg::VgtPrimitiveType &&
                idx(offsetof(R, num_indices)) == Cp::Reg::VgtNumIndices &&
                idx(offsetof(R, num_instances)) == Cp::Reg::VgtNumInstances &&
                idx(offsetof(R, stage_enable)) == Cp::Reg::VgtShaderStagesEn &&
                idx(offsetof(R, index_offset)) == Cp::Reg::VgtIndxOffset &&
                idx(offsetof(R, ps_program)) == Cp::Reg::SpiShaderPgmLoPs &&
                idx(offsetof(R, vs_program)) == Cp::Reg::SpiShaderPgmLoVs &&
                idx(offsetof(R, color_buffers)) == Cp::Reg::CbColor0Base &&
                idx(offsetof(R, depth_buffer.z_info)) == Cp::Reg::DbZInfo &&
                idx(offsetof(R, cs_program)) == Cp::kComputeBase &&
                sizeof(AmdGpu::ComputeProgram) / 4 == Cp::kComputeCount &&
                idx(offsetof(AmdGpu::ComputeProgram, user_data)) == Cp::Reg::ComputeUserData0 &&
                idx(offsetof(AmdGpu::ComputeProgram, dim_x)) == Cp::Reg::ComputeDimX,
            "");

    {
        // the groups' hashes kept at every change equal a recomputation from the values
        Cp::RegisterFile hf;
        u32 seed = 1;
        for (u32 i = 0; i < 20000; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const u32 value = seed >> 20; // few distinct values: many writes change nothing
            const auto space = static_cast<Cp::Space>(seed % 4);
            hf.Write(space, (seed >> 4) % Cp::kSpaceCount[static_cast<u32>(space)], &value, 1);
        }
        hf.ClearContext();
        bool equal = true;
        for (u32 gi = 0; gi < static_cast<u32>(Group::Count); ++gi) {
            equal &= hf.GroupHash(static_cast<Group>(gi)) == hf.ComputeGroupHash(static_cast<Group>(gi));
        }
        g.Check("the groups' hashes kept at every change equal their recomputation (20000 random writes, a clear)",
                equal, "");
    }
    Cp::ComputeRegisters cr;
    const u32 ud[3] = {7, 8, 9};
    g.Check("compute registers: user data written, its group's version bumped once per batch",
            cr.Write(Cp::Reg::ComputeUserData0, ud, 3) && cr.values[0x41] == 8 &&
                cr.versions[static_cast<u32>(Cp::ComputeGroup::UserData)] == 1 &&
                !cr.Write(0x4F, ud, 2),
            "");
}

// ---------------------------------------------------------------------------------------------------------------

void TestPackets(TestGroup& g) {
    {
        Rig r;
        Stream s;
        s.P(Op::Nop, {}).P(Op::Nop, {0x68750776u}).d.push_back(0x80000000u); // type-2 padding
        g.Check("NOP padding, the flip marker (one Flip with the frame's work), type-2",
                r.Gfx(s) == StepStatus::Idle && r.rec.flips.size() == 1 && r.cp.Stats().type2 == 1, "");
    }
    {
        Rig r;
        Stream s;
        s.Sh(0x2C48, {0x12345678, 0x9A, 0x11, 0x22})        // VS program LO/HI/RSRC1/RSRC2
            .Sh(0x2C4C, {0xA, 0xB, 0xC})                   // VS user data 0..2
            .Sh(0x2C08, {0x87654321, 0x01, 0x33, 0x44})    // PS program
            .P(Op::Nop, {0x68750004u})                     // V# in user data: next SET_SH_REG
            .Sh(0x2C50, {1, 2, 3, 4})                      // VS user data 4..7 (an inline V#)
            .Ctx(0xA1E0, {0x55})                           // CB_BLEND0_CONTROL
            .Ucfg(0xC242, {4})                             // VGT_PRIMITIVE_TYPE: triangle list
            .P(Op::IndexType, {1})                         // 32-bit indices
            .P(Op::NumInstances, {3})
            .Draw(36, 0x1'2345'6000ull);
        const bool ok = r.Gfx(s) == StepStatus::Idle && r.rec.draws.size() == 1;
        const auto& d = ok ? r.rec.draws[0] : Cp::DrawDescriptor{};
        const auto vs = static_cast<u32>(Cp::Stage::Vs), ps = static_cast<u32>(Cp::Stage::Ps);
        g.Check("SET_SH_REG / SET_CONTEXT_REG / SET_UCONFIG_REG, INDEX_TYPE, NUM_INSTANCES, DRAW_INDEX_2: the draw's "
                "program addresses, user data, index buffer, counts, primitive type",
                ok && d.shaders[vs].address == ((0x9Aull << 32 | 0x12345678) << 8) && d.shaders[vs].rsrc1 == 0x11 &&
                    d.shaders[ps].address == ((0x01ull << 32 | 0x87654321) << 8) && d.user_data[vs][0] == 0xA &&
                    d.user_data[vs][2] == 0xC && d.user_data[vs][4] == 1 && d.index_count == 36 &&
                    d.instance_count == 3 && d.index_type == 1 && d.index_base == 0x1'2345'6000ull &&
                    d.max_index_size == 0x1000 && d.primitive_type == 4 && d.indexed && !d.indirect && d.seq == 1 &&
                    d.packet_addr == kCmd + (s.d.size() - 6) * 4,
                ok ? fmt::format("vs {:#x} ud0 {:#x} count {} base {:#x}", d.shaders[vs].address, d.user_data[vs][0],
                                 d.index_count, d.index_base)
                   : r.cp.LastError().Text());
        g.Check("the NOP 'V# in user data' marker: the slots of the next SET_SH_REG flagged as inline descriptors",
                ok && d.inline_sharps[vs] == 0xF0, ok ? fmt::format("{:#x}", d.inline_sharps[vs]) : "");
        g.Check("the graphics register file holds the values; registers named by index",
                r.cp.Registers().Get(0xA1E0) == 0x55 && r.cp.Registers().Get(Cp::Reg::VgtNumInstances) == 3, "");
    }
    {
        Rig r;
        Stream s;
        s.P(Op::IndexBase, {0x0000A000, 0x12}).P(Op::IndexBufferSize, {777})
            .P(Op::SetBase, {1, 0x00050000, 0x0001})
            .P(Op::DrawIndexIndirect, {0x40, 2, 3, 0})
            .P(Op::DrawIndirect, {0x80, 4, 5, 2})
            .P(Op::DrawIndexAuto, {99, 2})
            .P(Op::DrawIndexOffset2, {500, 10, 20, 0});
        const bool ok = r.Gfx(s) == StepStatus::Idle && r.rec.draws.size() == 4;
        const auto& d = r.rec.draws;
        g.Check("INDEX_BASE, INDEX_BUFFER_SIZE, SET_BASE, DRAW_INDEX_INDIRECT: index base, arguments address = base + "
                "offset, VGT_DMA_MAX_SIZE from the buffer size, base vertex/instance SH offsets",
                ok && d[0].indexed && d[0].indirect && d[0].index_base == 0x12'0000A000ull &&
                    d[0].args_addr == 0x1'0005'0040ull && d[0].max_index_size == 777 && d[0].base_vtx_loc == 2 &&
                    d[0].start_inst_loc == 3,
                ok ? fmt::format("args {:#x} max {}", d[0].args_addr, d[0].max_index_size) : r.cp.LastError().Text());
        g.Check("DRAW_INDIRECT (not indexed), DRAW_INDEX_AUTO (count, initiator), DRAW_INDEX_OFFSET_2 (first index)",
                ok && !d[1].indexed && d[1].indirect && d[1].args_addr == 0x1'0005'0080ull && d[1].draw_initiator == 2 &&
                    !d[2].indexed && d[2].index_count == 99 && d[3].indexed && d[3].index_offset == 10 &&
                    d[3].index_count == 20 && d[3].max_index_size == 500 && d[3].seq == 4,
                "");
    }
    {
        Rig r;
        r.cp.MapComputeQueue(1, kIb, 0x400, kData + 0x100);
        Stream s;
        s.Sh(0x2E07, {64, 1, 1})                           // COMPUTE_NUM_THREAD_X/Y/Z
            .Sh(0x2E0C, {0x00ABCDEF, 0x02})                // COMPUTE_PGM_LO/HI
            .Sh(0x2E12, {0x77, 0x88})                      // RSRC1/RSRC2
            .Sh(0x2E40, {0xD0, 0xD1})                      // user data
            .P(Op::DispatchDirect, {8, 4, 2, 1})
            .P(Op::DispatchDirect, {1, 1, 1, 0})           // COMPUTE_SHADER_EN off
            .P(Op::DispatchIndirect, {static_cast<u32>(kData), 0, 1});
        r.mem.Put(kIb, s.d);
        r.cp.SubmitCompute(1, kIb, static_cast<u32>(s.d.size()));
        const StepStatus st = r.cp.Run(1);
        const auto& d = r.rec.dispatches;
        g.Check("compute queue: SET_SH_REG of COMPUTE_*, DISPATCH_DIRECT (groups, threads, program, user data), the "
                "initiator's enable bit off (nothing launched), DISPATCH_INDIRECT on a compute queue (MEC form)",
                st == StepStatus::Idle && d.size() == 2 && d[0].groups == std::array<u32, 3>{8, 4, 2} &&
                    d[0].threads[0] == 64 && d[0].shader.address == ((0x02ull << 32 | 0x00ABCDEF) << 8) &&
                    d[0].shader.rsrc1 == 0x77 && d[0].user_data[1] == 0xD1 && d[1].indirect &&
                    d[1].indirect_addr == kData && r.cp.Stats().dispatches_disabled == 1 &&
                    r.cp.Registers().Get(0x2E40) == 0,
                fmt::format("{} {}", d.size(), r.cp.LastError().Text()));
        g.Check("the compute queue's read pointer follows the packets (Isolated writes it)",
                r.mem.U32(kData + 0x100) == s.d.size() % 0x400, fmt::format("{}", r.mem.U32(kData + 0x100)));
    }
    {
        Rig r;
        Stream s;
        s.Sh(0x2E40, {0x42}).P(Op::DispatchDirect, {2, 2, 2, 1});
        g.Check("a dispatch on the graphics queue uses the graphics queue's own compute registers",
                r.Gfx(s) == StepStatus::Idle && r.rec.dispatches.size() == 1 && r.rec.dispatches[0].user_data[0] == 0x42 &&
                    r.cp.Compute(0)->values[0x40] == 0x42,
                "");
    }
    {
        Rig r;
        for (u32 i = 0; i < 16; ++i) {
            r.mem.Put32(kData + i * 4, 0x100 + i);
        }
        Stream s;
        s.P(Op::DmaData, {0, static_cast<u32>(kData), 0, static_cast<u32>(kData + 0x1000), 0, 64})          // mem->mem
            .P(Op::DmaData, {2u << 29, 0xA5A5A5A5u, 0, static_cast<u32>(kData + 0x2000), 0, 10})          // data->mem
            .P(Op::DmaData, {(2u << 29) | (1u << 20), 0x01020304u, 0, 0x100, 0, 16})                       // data->GDS
            .P(Op::DmaData, {1u << 29, 0x100, 0, static_cast<u32>(kData + 0x3000), 0, 16})                // GDS->mem
            .P(Op::DmaData, {1u << 20, static_cast<u32>(kData), 0, 0x200, 0, 8})                           // mem->GDS
            .P(Op::DmaData, {0, static_cast<u32>(kData), 0, 0x3022C, 0, 64});                             // prefetch
        const StepStatus st = r.Gfx(s);
        g.Check("DMA_DATA memory->memory, data->memory (10 bytes of the pattern), data->GDS->memory, memory->GDS, "
                "the CP_DMA_READ_TAGS prefetch (no write)",
                st == StepStatus::Idle && r.mem.U32(kData + 0x1000) == 0x100 && r.mem.U32(kData + 0x103C) == 0x10F &&
                    r.mem.U32(kData + 0x2000) == 0xA5A5A5A5u && r.mem.bytes[kData + 0x2009 - TestMem::kBase] == 0xA5 &&
                    r.mem.bytes[kData + 0x200A - TestMem::kBase] == 0 && r.mem.U32(kData + 0x3000) == 0x01020304u &&
                    r.mem.U32(kData + 0x300C) == 0x01020304u && r.cp.Gds()[0x200] == 0x00 &&
                    r.cp.Gds()[0x201] == 0x01 && r.cp.Stats().dma_prefetch == 1 && r.rec.dmas.size() == 6,
                r.cp.LastError().Text());
    }
    {
        Rig r;
        Stream s;
        s.P(Op::AcquireMem, {0x28C00000u, 0x100, 0, 0x2000, 0, 10})
            .P(Op::ContextControl, {0x80000000u, 0x80000000u})
            .P(Op::PfpSyncMe, {0})
            .P(Op::EventWrite, {0x0407}); // CS_PARTIAL_FLUSH
        g.Check("ACQUIRE_MEM (a barrier with its range for the GPU), CONTEXT_CONTROL, PFP_SYNC_ME, EVENT_WRITE of a "
                "partial flush",
                r.Gfx(s) == StepStatus::Idle && r.rec.cache.size() == 2 && r.rec.cache[0].base == 0x2000ull << 8 &&
                    r.rec.cache[0].size == 0x100ull << 8 && r.rec.cache[1].event == 0x407,
                r.cp.LastError().Text());
    }
    {
        Rig r;
        Stream s;
        s.Ctx(0xA200, {0x77}).P(Op::ClearState, {0});
        std::array<u32, 0x400> def{};
        Cp::ContextDefaults(def);
        g.Check("CLEAR_STATE restores the context's clear state",
                r.Gfx(s) == StepStatus::Idle && r.cp.Registers().Get(0xA200) == def[0x200] &&
                    r.cp.Registers().Get(0xA080 + 2) == def[0x82],
                "");
    }
    {
        // the bridge's packet layouts read our packets the same way (the hardware's field positions)
        const u32 eop[] = {H3(Op::EventWriteEop, 5), 0x0504, 0x12345678, (2u << 29) | (2u << 24) | 0xAB, 0xDEADBEEF,
                           0xCAFEBABE};
        const auto* b = reinterpret_cast<const AmdGpu::PM4CmdEventWriteEop*>(eop);
        Rig r(Cp::Mode::Shadow); // the address is not in the test memory: decoded and reported, not written
        Stream s;
        s.d.assign(std::begin(eop), std::end(eop));
        r.Gfx(s);
        const auto& f = r.rec.fences;
        g.Check("EVENT_WRITE_EOP fields as the bridge's PM4CmdEventWriteEop: address, data select, interrupt select, "
                "64-bit data, event type",
                f.size() == 1 && f[0].addr == reinterpret_cast<u64>(b->Address<u8>()) &&
                    f[0].data_sel == static_cast<u32>(b->data_sel.Value()) &&
                    f[0].int_sel == static_cast<u32>(b->int_sel.Value()) && f[0].data == b->DataQWord() &&
                    f[0].event_type == b->event_type.Value(),
                "");
        const u32 rel[] = {H3(Op::ReleaseMem, 6), 47, (2u << 29) | (3u << 24), 0x1000, 0x2, 7, 8};
        const auto* br = reinterpret_cast<const AmdGpu::PM4CmdReleaseMem*>(rel);
        const u32 dma[] = {H3(Op::DmaData, 6), (3u << 29) | (3u << 20), 0x1111, 0x22, 0x3333, 0x44, 12345};
        const auto* bd = reinterpret_cast<const AmdGpu::PM4DmaData*>(dma);
        const u32 ib[] = {H3(Op::IndirectBuffer, 3), 0xABCD0, 0x12, (1u << 20) | 77};
        const auto* bi = reinterpret_cast<const AmdGpu::PM4CmdIndirectBuffer*>(ib);
        const u32 wait[] = {H3(Op::WaitRegMem, 6), 0x13, 0x1004, 0x5, 9, 0xFF, 10};
        const auto* bw = reinterpret_cast<const AmdGpu::PM4CmdWaitRegMem*>(wait);
        g.Check("RELEASE_MEM, DMA_DATA, INDIRECT_BUFFER, WAIT_REG_MEM fields agree with the bridge's layouts",
                br->Address<u64>() == 0x2'0000'1000ull && static_cast<u32>(br->data_sel.Value()) == 2 &&
                    static_cast<u32>(br->int_sel.Value()) == 3 && bd->NumBytes() == 12345 &&
                    static_cast<u32>(bd->src_sel) == 3 && static_cast<u32>(bd->dst_sel) == 3 &&
                    bd->SrcAddress<u64>() == 0x22'0000'1111ull && bi->ib_size.Value() == 77 &&
                    bi->chain.Value() == 1 && reinterpret_cast<u64>(bi->Address<u32>()) == 0x12'000A'BCD0ull &&
                    bw->Address<u64>() == 0x5'0000'1004ull && static_cast<u32>(bw->function.Value()) == 3,
                "");
    }
}

// ---------------------------------------------------------------------------------------------------------------

void TestSync(TestGroup& g) {
    {
        Rig r(Cp::Mode::Isolated, &FixedClock);
        Stream s;
        s.P(Op::EventWriteEop, {0x0504, static_cast<u32>(kData), 2u << 29, 0x11111111, 0x22222222}); // nothing before
        s.Draw(3);
        s.P(Op::EventWriteEop, {0x0504, static_cast<u32>(kData + 8), (2u << 29) | (2u << 24), 0x33, 0x44});
        s.P(Op::EventWriteEop, {0x0528, static_cast<u32>(kData + 16), 4u << 29, 0, 0}); // GPU counter
        s.P(Op::EventWriteEos, {0x062F, static_cast<u32>(kData + 24), 2u << 29, 0x55});
        const StepStatus st = r.Gfx(s);
        const bool before = st == StepStatus::Idle && r.mem.U64(kData) == 0x2222222211111111ull &&
                            r.mem.U64(kData + 8) == 0 && r.mem.U64(kData + 16) == 0 && r.mem.U32(kData + 24) == 0;
        g.Check("EVENT_WRITE_EOP with no GPU work before it: written at once; after a draw: held (not written) until "
                "the GPU completes the draw",
                before && r.cp.Stats().fences_deferred == 3, r.cp.LastError().Text());
        r.cp.CompleteWork(0, 1);
        g.Check("CompleteWork: the held EOP/EOS fences written in order, the interrupt of int_sel 2, the GPU counter "
                "from the clock",
                r.mem.U64(kData + 8) == 0x0000004400000033ull && r.mem.U64(kData + 16) == 0x5555666677778888ull &&
                    r.mem.U32(kData + 24) == 0x55 && r.rec.irqs == 1 && r.cp.Stats().fences_written == 4,
                fmt::format("{:#x} {:#x}", r.mem.U64(kData + 8), r.mem.U64(kData + 16)));
    }
    {
        Rig r;
        Stream s;
        s.Draw(3).P(Op::WriteData, {5u << 8, static_cast<u32>(kData), 0, 0xAAAA, 0xBBBB});
        s.P(Op::EventWriteEop, {0x0504, static_cast<u32>(kData + 8), 1u << 29, 0x77, 0});
        r.Gfx(s);
        g.Check("WRITE_DATA is written when the packet is processed (the micro engine's write, not held behind the "
                "draw); the EOP after it is held",
                r.mem.U32(kData) == 0xAAAA && r.mem.U32(kData + 4) == 0xBBBB && r.mem.U32(kData + 8) == 0, "");
        r.cp.Reset();
        g.Check("Reset drops the held fence: counted, never written",
                r.cp.Stats().fences_dropped == 1 && r.mem.U32(kData + 8) == 0, "");
    }
    {
        Rig r;
        Stream s;
        s.P(Op::WaitRegMem, {0x13, static_cast<u32>(kData + 0x40), 0, 7, 0xFF, 10}) // memory == 7
            .Draw(3);
        const StepStatus st1 = r.Gfx(s);
        const bool blocked = st1 == StepStatus::Blocked && r.rec.draws.empty() && r.cp.QueueBlocked(0);
        r.mem.Put32(kData + 0x40, 0x107); // masked to 7
        const StepStatus st2 = r.cp.Run(0);
        g.Check("WAIT_REG_MEM: blocks while the memory does not match, stays on the packet, resumes when it does "
                "(masked compare)",
                blocked && st2 == StepStatus::Idle && r.rec.draws.size() == 1 && r.cp.Stats().waits_blocked == 1,
                fmt::format("{} {}", static_cast<int>(st1), static_cast<int>(st2)));
        Stream t;
        t.Ctx(0xA200, {5}).P(Op::WaitRegMem, {0x05, 0xA200, 0, 5, 0xFF, 1}); // register >= 5
        g.Check("WAIT_REG_MEM on a register (greater or equal)", r.Gfx(t, kCmd + 0x800) == StepStatus::Idle, "");
    }
    {
        Rig r;
        r.cp.MapComputeQueue(2, kIb, 0x400, 0);
        Stream s;
        s.P(Op::DmaData, {(2u << 29) | (1u << 20), 0xFEED, 0, 0x40, 0, 8})                   // GDS[0x40..] = pattern
            .P(Op::DispatchDirect, {1, 1, 1, 1})
            .P(Op::ReleaseMem, {47, (2u << 29) | (2u << 24), static_cast<u32>(kData), 0, 0x99, 0})
            .P(Op::ReleaseMem, {47, 5u << 29, static_cast<u32>(kData + 16), 0, (2u << 16) | 0x40, 0});
        r.mem.Put(kIb, s.d);
        r.cp.SubmitCompute(2, kIb, static_cast<u32>(s.d.size()));
        r.cp.Run(2);
        const bool held = r.mem.U64(kData) == 0;
        r.cp.CompleteWork(2, r.cp.SubmittedWork(2));
        g.Check("RELEASE_MEM after a dispatch: held, then written with its interrupt; GDS data (2 dwords) copied",
                held && r.mem.U64(kData) == 0x99 && r.rec.irqs == 1 && r.mem.U32(kData + 16) == 0xFEED &&
                    r.mem.U32(kData + 20) == 0xFEED && r.cp.Stats().gds_fences == 1,
                fmt::format("{:#x}", r.mem.U32(kData + 16)));
    }
    {
        // Shadow: the same stream writes nothing and waits for nothing
        Rig r(Cp::Mode::Shadow);
        Stream s;
        s.P(Op::WaitRegMem, {0x13, static_cast<u32>(kData + 0x40), 0, 7, 0xFF, 10}).Draw(3);
        s.P(Op::EventWriteEop, {0x0504, static_cast<u32>(kData), 2u << 29, 1, 2});
        s.P(Op::WriteData, {5u << 8, static_cast<u32>(kData + 8), 0, 0xAAAA});
        s.P(Op::DmaData, {2u << 29, 0xA5A5A5A5u, 0, static_cast<u32>(kData + 0x100), 0, 16});
        const StepStatus st = r.Gfx(s);
        g.Check("Shadow mode: no wait, no fence write, no WRITE_DATA, no DMA — only reported (the bridge does them)",
                st == StepStatus::Idle && r.mem.writes == 0 && r.rec.draws.size() == 1 && r.rec.fences.size() == 1 &&
                    !r.rec.fences[0].written && r.rec.dmas.size() == 2 && !r.rec.dmas[1].executed,
                fmt::format("writes {}", r.mem.writes));
    }
}

// ---------------------------------------------------------------------------------------------------------------

void TestState(TestGroup& g) {
    Rig r;
    Stream s;
    s.Sh(0x2C48, {0x1000, 0, 0, 0}).Sh(0x2C4C, {0x10}).Ctx(0xA1E0, {0x1}).Ctx(0xA10F, {0x3F800000})
        .Ctx(0xA318, {0x100}).Draw(3)                                // 0: first
        .Sh(0x2C4C, {0x10}).Ctx(0xA1E0, {0x1}).Draw(3)               // 1: the same values again
        .Ctx(0xA1E0, {0x2}).Draw(3)                                  // 2: blend changed
        .Ctx(0xA10F, {0x40000000}).Draw(3)                           // 3: viewport changed
        .Ctx(0xA318, {0x200}).Draw(3)                                // 4: target address changed
        .Ctx(0xA31C, {0x1234}).Draw(3)                               // 5: target format changed
        .Ctx(0xA0D4, {0x99}).Draw(3)                                 // 6: an unknown context register
        .Sh(0x2C4C, {0x20}).Draw(3)                                  // 7: VS user data
        .Sh(0x2E40, {0x5}).P(Op::DispatchDirect, {1, 1, 1, 1}).Draw(3); // 8: a dispatch between draws
    const bool ok = r.Gfx(s) == StepStatus::Idle && r.rec.draws.size() == 9;
    if (!ok) {
        g.Check("the state stream runs", false, r.cp.LastError().Text());
        return;
    }
    const auto& d = r.rec.draws;
    g.Check("the same values written again: the pipeline key and every version unchanged, no group but the draw's "
            "own registers changed",
            d[1].pipeline_key == d[0].pipeline_key && d[1].pipeline_version == d[0].pipeline_version &&
                d[1].dynamic_version == d[0].dynamic_version && d[1].targets_version == d[0].targets_version &&
                (d[1].changed & ~(Cp::Bit(Group::DrawParams) | Cp::Bit(Group::IndexState))) == 0,
            fmt::format("changed {:#x}", d[1].changed));
    g.Check("one blend register: a new pipeline key and version; dynamic state and targets unchanged",
            d[2].pipeline_key != d[1].pipeline_key && d[2].pipeline_version == d[1].pipeline_version + 1 &&
                d[2].dynamic_version == d[1].dynamic_version && d[2].targets_version == d[1].targets_version &&
                (d[2].changed & Cp::Bit(Group::Blend)) != 0,
            "");
    g.Check("the viewport: dynamic state only (the pipeline key stays)",
            d[3].pipeline_key == d[2].pipeline_key && d[3].dynamic_version == d[2].dynamic_version + 1, "");
    g.Check("a colour target's address: the targets only (the pipeline key stays)",
            d[4].pipeline_key == d[3].pipeline_key && d[4].targets_version == d[3].targets_version + 1, "");
    g.Check("a colour target's format: the pipeline key and the targets",
            d[5].pipeline_key != d[4].pipeline_key && d[5].targets_version == d[4].targets_version + 1, "");
    g.Check("an unnamed context register: a new pipeline key (unknown state is assumed to matter)",
            d[6].pipeline_key != d[5].pipeline_key, "");
    g.Check("user data: the draw carries the new value, the pipeline key stays",
            d[7].user_data[static_cast<u32>(Cp::Stage::Vs)][0] == 0x20 && d[7].pipeline_key == d[6].pipeline_key &&
                (d[7].changed & Cp::Bit(Group::VsUserData)) != 0,
            "");
    g.Check("compute registers and a dispatch between draws change no graphics group",
            d[8].pipeline_version == d[7].pipeline_version &&
                (d[8].changed & ~(Cp::Bit(Group::DrawParams) | Cp::Bit(Group::IndexState))) == 0 &&
                r.rec.dispatches.size() == 1,
            fmt::format("changed {:#x}", d[8].changed));
    g.Check("every draw sees the state of its own position in the stream (user data of draw 0 vs 7)",
            d[0].user_data[static_cast<u32>(Cp::Stage::Vs)][0] == 0x10 && d[0].index_count == 3 &&
                r.cp.Stats().pipeline_key_rebuilds == 4, // draws 0, 2 (blend), 5 (format), 6 (unknown register)
            fmt::format("rebuilds {}", r.cp.Stats().pipeline_key_rebuilds));
    {
        // target size hints: CB_COLOR0 setup of 14 registers then the driver's one-dword NOP
        Rig h;
        Stream t;
        t.Ctx(0xA318, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14}).P(Op::Nop, {0x04380780u}).Draw(3);
        t.Ctx(0xA327, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14}).Draw(3); // CB1 without its hint
        // DB_Z_INFO with 8 registers: the hint 20 dwords after their start (past a 12-dword setup packet)
        t.Ctx(0xA010, {1, 2, 3, 4, 5, 6, 7, 8}).Ctx(0xA002, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0})
            .P(Op::Nop, {0x01000200u}).Draw(3);
        const bool ran = h.Gfx(t) == StepStatus::Idle && h.rec.draws.size() == 3;
        g.Check("target size hints after a colour/depth target setup (the driver's NOP), a missing one counted",
                ran && h.rec.draws[0].cb_extent[0] == 0x04380780u && h.rec.draws[1].cb_extent[1] == 0 &&
                    h.cp.Stats().extent_hints_missing == 1 && h.rec.draws[2].db_extent == 0x01000200u,
                ran ? fmt::format("{:#x} {:#x}", h.rec.draws[0].cb_extent[0], h.rec.draws[2].db_extent)
                    : h.cp.LastError().Text());
    }
    {
        // a DMA writing the group counts of a later DISPATCH_DIRECT of the same buffer (Gnm's indirect dispatch)
        for (const auto mode : {Cp::Mode::Isolated, Cp::Mode::Shadow}) {
            Rig p(mode);
            p.mem.Put(kData, {16, 8, 4});
            Stream t;
            t.P(Op::DmaData, {0, static_cast<u32>(kData), 0, 0, 0, 12}); // destination patched below
            const size_t dispatch_at = t.d.size();
            t.P(Op::DispatchDirect, {1, 1, 1, 1});
            t.d[4] = static_cast<u32>(kCmd + dispatch_at * 4 + 4); // dst_lo: the dispatch's counts
            const bool ran = p.Gfx(t) == StepStatus::Idle && p.rec.dispatches.size() == 1;
            const auto& dd = p.rec.dispatches.empty() ? Cp::DispatchDescriptor{} : p.rec.dispatches[0];
            const bool right = mode == Cp::Mode::Isolated
                                   ? !dd.indirect && dd.patched && dd.groups == std::array<u32, 3>{16, 8, 4}
                                   : dd.indirect && dd.patched && dd.indirect_addr == kData;
            g.Check(fmt::format("{}: a DMA into a later dispatch's group counts — {}",
                                mode == Cp::Mode::Isolated ? "Isolated" : "Shadow",
                                mode == Cp::Mode::Isolated ? "the processor's copy patched, the dispatch takes them"
                                                           : "the dispatch reads them from the DMA's source"),
                    ran && right && p.cp.Stats().dma_command_patches == 1,
                    ran ? fmt::format("groups {} {} {} indirect {}", dd.groups[0], dd.groups[1], dd.groups[2],
                                      dd.indirect)
                        : p.cp.LastError().Text());
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------

void TestErrors(TestGroup& g) {
    const auto expect = [&g](const char* what, std::vector<u32> stream, Cp::CpErrorKind kind) {
        Rig r;
        Stream s;
        s.d = std::move(stream);
        const StepStatus st = r.Gfx(s);
        g.Check(what, st == StepStatus::Faulted && r.cp.LastError().kind == kind && r.cp.QueueFaulted(0),
                r.cp.LastError().Text());
    };
    expect("a packet longer than its buffer: Truncated (nothing read beyond)", {H3(Op::DrawIndex2, 5), 1, 2},
           Cp::CpErrorKind::Truncated);
    expect("a packet shorter than its fields (DRAW_INDEX_2 with 2 body dwords): ShortPacket",
           {H3(Op::DrawIndex2, 2), 1, 2}, Cp::CpErrorKind::ShortPacket);
    expect("an opcode without a name (0xFE): UnknownOpcode", {H3(0xFE, 1), 0}, Cp::CpErrorKind::UnknownOpcode);
    expect("a known opcode the processor does not implement (COND_EXEC): UnsupportedOpcode",
           {H3(Op::CondExec, 3), 0, 0, 0}, Cp::CpErrorKind::UnsupportedOpcode);
    expect("a type-0 packet: refused", {(1u << 16) | 0x2C08, 1, 2}, Cp::CpErrorKind::Type0);
    expect("a type-1 header: refused", {0x40000000u}, Cp::CpErrorKind::ReservedType);
    expect("SET_CONTEXT_REG past the context window: RegisterRange", {H3(Op::SetContextReg, 3), 0x3FF, 1, 2},
           Cp::CpErrorKind::RegisterRange);
    expect("a predicated packet: UnsupportedForm", {H3(Op::Nop, 1) | 1u, 0}, Cp::CpErrorKind::UnsupportedForm);
    expect("WRITE_DATA to one address repeatedly: UnsupportedForm", {H3(Op::WriteData, 4), (5u << 8) | (1u << 16),
                                                                       static_cast<u32>(kData), 0, 1},
           Cp::CpErrorKind::UnsupportedForm);
    expect("DMA_DATA into GDS past its end: GdsRange", {H3(Op::DmaData, 6), (2u << 29) | (1u << 20), 0, 0, 0xFFF0, 0, 64},
           Cp::CpErrorKind::GdsRange);
    expect("an indirect buffer the memory cannot read: IndirectUnreadable",
           {H3(Op::IndirectBuffer, 3), 0x7000'0000u, 0x55, 16}, Cp::CpErrorKind::IndirectUnreadable);
    {
        // an indirect buffer calling itself: the depth limit ends it
        Rig r;
        Stream ib;
        ib.P(Op::IndirectBuffer, {static_cast<u32>(kIb), 0, 4});
        r.mem.Put(kIb, ib.d);
        Stream s;
        s.P(Op::IndirectBuffer, {static_cast<u32>(kIb), 0, 4});
        const StepStatus st = r.Gfx(s);
        g.Check("an indirect buffer calling itself: IndirectTooDeep at the depth limit",
                st == StepStatus::Faulted && r.cp.LastError().kind == Cp::CpErrorKind::IndirectTooDeep,
                r.cp.LastError().Text());
    }
    {
        // an indirect buffer chaining to itself: the submission's budget ends it
        TestMem mem;
        Rec rec;
        Cp::CommandProcessor cp(Cp::Options{.mode = Cp::Mode::Isolated, .memory = &mem, .work = &rec,
                                            .max_dwords_per_submission = 100000});
        Stream ib;
        ib.P(Op::Nop, {0}).P(Op::IndirectBuffer, {static_cast<u32>(kIb), 0, (1u << 20) | 6});
        mem.Put(kIb, ib.d);
        Stream s;
        s.P(Op::IndirectBuffer, {static_cast<u32>(kIb), 0, 6});
        mem.Put(kCmd, s.d);
        cp.SubmitGraphics(kCmd, static_cast<u32>(s.d.size()));
        const StepStatus st = cp.Run(0);
        g.Check("an indirect buffer chaining to itself: BudgetExceeded, no endless loop",
                st == StepStatus::Faulted && cp.LastError().kind == Cp::CpErrorKind::BudgetExceeded,
                cp.LastError().Text());
    }
    {
        // nested indirect buffers and a chain: the draws inside run in order
        Rig r;
        Stream ib2;
        ib2.Draw(7);
        r.mem.Put(kIb + 0x1000, ib2.d);
        Stream ib3;
        ib3.Draw(8);
        r.mem.Put(kIb + 0x2000, ib3.d);
        Stream ib1;
        ib1.Draw(5).P(Op::IndirectBuffer, {static_cast<u32>(kIb + 0x1000), 0, static_cast<u32>(ib2.d.size())})
            .P(Op::IndirectBuffer, {static_cast<u32>(kIb + 0x2000), 0, (1u << 20) | static_cast<u32>(ib3.d.size())})
            .Draw(99); // after a chain: not reached
        r.mem.Put(kIb, ib1.d);
        Stream s;
        s.P(Op::IndirectBuffer, {static_cast<u32>(kIb), 0, static_cast<u32>(ib1.d.size())}).Draw(9);
        const StepStatus st = r.Gfx(s);
        std::string counts;
        for (const auto& d : r.rec.draws) {
            counts += fmt::format(" {}", d.index_count);
        }
        g.Check("nested indirect buffers and a chained one: draws in stream order, the rest of a chained buffer skipped",
                st == StepStatus::Idle && counts == " 5 7 8 9", counts);
    }
    {
        Rig r;
        Stream bad;
        bad.P(0xFE, {0});
        r.Gfx(bad);
        Stream good;
        good.Draw(3);
        r.mem.Put(kCmd + 0x800, good.d);
        r.cp.SubmitGraphics(kCmd + 0x800, static_cast<u32>(good.d.size()));
        const StepStatus stuck = r.cp.Run(0);
        r.cp.Reset();
        r.cp.SubmitGraphics(kCmd + 0x800, static_cast<u32>(good.d.size()));
        const StepStatus after = r.cp.Run(0);
        g.Check("Isolated: a faulted queue stays faulted (nothing after it executes) until Reset; then it runs again",
                stuck == StepStatus::Faulted && r.rec.draws.size() == 1 && after == StepStatus::Idle &&
                    !r.cp.QueueFaulted(0),
                fmt::format("{} draws", r.rec.draws.size()));
    }
    {
        TestMem mem;
        Rec rec;
        Cp::CommandProcessor cp(Cp::Options{.mode = Cp::Mode::Shadow, .memory = &mem, .work = &rec,
                                            .resume_after_fault = true});
        Stream bad, good;
        bad.P(0xFE, {0}).Draw(1);
        good.Draw(3);
        mem.Put(kCmd, bad.d);
        mem.Put(kCmd + 0x800, good.d);
        cp.SubmitGraphics(kCmd, static_cast<u32>(bad.d.size()));
        cp.SubmitGraphics(kCmd + 0x800, static_cast<u32>(good.d.size()));
        cp.Run(0);
        cp.Run(0);
        g.Check("resume after a fault (the shadow's setting): the faulted submission dropped, the next one processed",
                rec.draws.size() == 1 && rec.draws[0].index_count == 3, fmt::format("{}", rec.draws.size()));
    }
    {
        Rig r;
        r.cp.MapComputeQueue(1, kIb, 0x400, 0);
        Stream s;
        s.Draw(3);
        r.mem.Put(kIb, s.d);
        r.cp.SubmitCompute(1, kIb, static_cast<u32>(s.d.size()));
        g.Check("a graphics packet on a compute queue: UnsupportedForm",
                r.cp.Run(1) == StepStatus::Faulted && r.cp.LastError().kind == Cp::CpErrorKind::UnsupportedForm,
                r.cp.LastError().Text());
    }
    {
        Rig r;
        Stream s;
        s.Draw(1);
        r.mem.Put(kCmd, s.d);
        u32 accepted = 0;
        for (u32 i = 0; i < 600; ++i) {
            accepted += r.cp.SubmitGraphics(kCmd, static_cast<u32>(s.d.size())) ? 1 : 0;
        }
        g.Check("too many submissions queued: refused (QueueFull), the queued ones still run",
                accepted == 512 && r.cp.Run(0) == StepStatus::Idle && r.rec.draws.size() == 512, fmt::format("{}", accepted));
    }
    {
        // review fixes (docs §12): a packet written by a DMA from memory is refused in the shadow, not decoded stale
        Rig r(Cp::Mode::Shadow);
        Stream s;
        s.P(Op::DmaData, {0, static_cast<u32>(kData), 0, 0, 0, 8});
        const size_t target = s.d.size();
        s.Sh(0x2C4C, {1, 2}).Draw(3);
        s.d[4] = static_cast<u32>(kCmd + target * 4 + 8); // the SET_SH_REG's values
        const StepStatus st = r.Gfx(s);
        g.Check("Shadow: a packet an earlier DMA from memory wrote into (not a dispatch's counts) — refused with the "
                "reason, never decoded from the stale copy",
                st == StepStatus::Faulted && r.cp.LastError().kind == Cp::CpErrorKind::UnsupportedForm &&
                    r.rec.draws.empty(),
                r.cp.LastError().Text());
    }
    {
        Rig r;
        Stream ib;
        ib.Draw(4).P(Op::IndirectBuffer, {static_cast<u32>(kIb + 0x100), 0, 1u << 20}).Draw(99);
        r.mem.Put(kIb, ib.d);
        Stream s;
        s.P(Op::IndirectBuffer, {static_cast<u32>(kIb), 0, static_cast<u32>(ib.d.size())}).Draw(5);
        r.Gfx(s);
        Stream bad;
        bad.Sh(0x2E40, {7}).P(Op::SetShReg, {0x3FE, 1, 2, 3}); // the second packet leaves the SH window
        const u32 before = r.cp.Compute(0)->values[0x40];
        r.Gfx(bad, kCmd + 0x800);
        g.Check("a chain to an empty buffer ends the current one; SET_SH_REG leaving the window writes nothing",
                r.rec.draws.size() == 2 && r.rec.draws[0].index_count == 4 && r.rec.draws[1].index_count == 5 &&
                    r.cp.LastError().kind == Cp::CpErrorKind::RegisterRange && r.cp.Registers().Get(0x2FFE) == 0 &&
                    r.cp.Compute(0)->values[0x40] == 7 && before == 0,
                fmt::format("{} draws {}", r.rec.draws.size(), r.cp.LastError().Text()));
    }
    {
        Rig r;
        r.cp.MapComputeQueue(5, kIb, 0x400, 0);
        Stream s;
        s.P(Op::DispatchDirect, {1, 1, 1, 1});
        r.mem.Put(kIb, s.d);
        r.cp.SubmitCompute(5, kIb, static_cast<u32>(s.d.size()));
        r.cp.MapComputeQueue(5, kIb, 0x400, 0); // mapped again before it ran
        g.Check("a compute queue mapped again: nothing of the previous ring survives",
                r.cp.PendingSubmissions(5) == 0 && r.cp.Run(5) == StepStatus::Idle && r.rec.dispatches.empty(), "");
    }
    {
        // no hidden shared state: processors on four threads at once, each with its own memory
        std::atomic<u32> good{0};
        std::vector<std::thread> threads;
        for (u32 t = 0; t < 4; ++t) {
            threads.emplace_back([&good, t] {
                Rig r;
                Stream s;
                for (u32 i = 0; i < 200; ++i) {
                    s.Sh(0x2C4C, {t * 1000 + i}).Draw(i + 1);
                }
                bool ok = true;
                for (u32 rep = 0; rep < 20 && ok; ++rep) {
                    r.rec.draws.clear();
                    ok = r.Gfx(s) == StepStatus::Idle && r.rec.draws.size() == 200 &&
                         r.rec.draws[199].user_data[static_cast<u32>(Cp::Stage::Vs)][0] == t * 1000 + 199;
                }
                good += ok ? 1 : 0;
            });
        }
        for (auto& th : threads) {
            th.join();
        }
        g.Check("four processors on four threads at once: independent (no shared hidden state)", good == 4,
                fmt::format("{}/4", good.load()));
    }
}

// ---------------------------------------------------------------------------------------------------------------

void TestRings(TestGroup& g) {
    Rig r;
    constexpr u64 kRing = kIb;
    constexpr u32 kRingDw = 64;
    const u64 rptr = kData + 0x200;
    r.cp.MapComputeQueue(3, kRing, kRingDw, rptr);
    // a DISPATCH_DIRECT (5 dwords) starting 2 dwords before the ring's end: cut by the wrap
    std::vector<u32> ring(kRingDw, 0x80000000u); // type-2 padding
    const u32 start = kRingDw - 2;
    r.mem.Put(kRing, ring);
    const bool d1 = r.cp.DingDong(3, start); // [0, start): padding, consumed before the ring is written again
    r.cp.Run(3);
    const u32 pkt[] = {H3(Op::DispatchDirect, 4), 3, 2, 1, 1};
    ring[start] = pkt[0];
    ring[start + 1] = pkt[1];
    ring[0] = pkt[2];
    ring[1] = pkt[3];
    ring[2] = pkt[4];
    r.mem.Put(kRing, ring);
    const bool d2 = r.cp.DingDong(3, 3); // [start, end) + [0, 3): the cut packet
    const StepStatus st = r.cp.Run(3);
    g.Check("DingDong over the ring's end: two spans, the packet cut by the wrap carried and executed once at its start "
            "address",
            d1 && d2 && st == StepStatus::Idle && r.rec.dispatches.size() == 1 &&
                r.rec.dispatches[0].groups == std::array<u32, 3>{3, 2, 1} &&
                r.rec.dispatches[0].packet_addr == kRing + u64{start} * 4 && r.cp.Stats().ring_carries == 1,
            fmt::format("{} dispatches {}", r.rec.dispatches.size(), r.cp.LastError().Text()));
    g.Check("the read pointer after the carried packet: 3 dwords into the next span",
            r.mem.U32(rptr) == 3, fmt::format("{}", r.mem.U32(rptr)));
    g.Check("DingDong to the current offset: nothing submitted",
            r.cp.DingDong(3, 3) && r.cp.PendingSubmissions(3) == 0 && r.cp.Run(3) == StepStatus::Idle, "");
    g.Check("DingDong past the ring's size: refused", !r.cp.DingDong(3, kRingDw + 1), "");
    {
        // Shadow: spans given by the caller (as the bridge does), the cut packet reassembled the same way
        Rig s(Cp::Mode::Shadow);
        s.mem.Put(kRing, ring);
        s.cp.SubmitCompute(4, kRing + u64{start} * 4, kRingDw - start);
        s.cp.SubmitCompute(4, kRing, 3);
        g.Check("Shadow: ring spans from the caller, the cut packet reassembled, no read pointer written",
                s.cp.Run(4) == StepStatus::Idle && s.rec.dispatches.size() == 1 && s.mem.writes == 0, "");
    }
}

// ---------------------------------------------------------------------------------------------------------------

/// A "bridge" for the synthetic trace: the processor's own state as the bridge would hold it, per checkpoint.
struct TraceBuilder final : Cp::GpuWork {
    void Draw(const Cp::DrawDescriptor&) override {}
    void Dispatch(const Cp::DispatchDescriptor&) override {}
};

void TestTrace(TestGroup& g, const App::Config& cfg) {
    const auto dir = cfg.logs_dir / "selftest_gpu_cp";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    // a synthetic trace: the stream processed by an Isolated processor with the bridge's mirror; its state at each
    // draw recorded as the "bridge's"
    Stream s;
    s.P(Op::ClearState, {0});
    for (u32 i = 0; i < 300; ++i) {
        s.Sh(0x2C4C, {i}).Ctx(0xA1E0, {i % 7}).P(Op::IndexBufferSize, {i}).Draw(i + 3);
    }
    TestMem mem;
    mem.Put(kCmd, s.d);
    TraceBuilder work;
    Cp::CommandProcessor cp(Cp::Options{.mode = Cp::Mode::Shadow, .memory = &mem, .work = &work});
    Cp::BridgeMirror mirror(cp);
    const auto path = dir / "synthetic.sspctrace";
    Cp::TraceWriter w;
    bool written = w.Open(path, "self-test");
    Cp::TraceState st;
    std::copy_n(mirror.Flat(), Cp::kNumRegs, st.flat.begin());
    w.State(st);
    w.Submission(0, false, kCmd, s.d);
    cp.SubmitGraphics(kCmd, static_cast<u32>(s.d.size()));
    u32 seq = 0;
    for (;;) {
        u64 a = 0;
        std::span<const u32> pk;
        if (!cp.Peek(0, a, pk)) {
            break;
        }
        if (((pk[0] >> 8) & 0xFF) == Op::DrawIndex2) {
            Cp::TraceCheckpoint c{.seq = ++seq, .queue = 0, .kind = 0, .addr = a,
                                  .regs_hash = Cp::HashFlatRegs(mirror.Flat(), Cp::kDrawRanges),
                                  .index_buffer_size = cp.Registers().Get(Cp::Reg::VgtDmaSize),
                                  .reg_dma_size = mirror.Get(Cp::Reg::VgtDmaSize)};
            w.Checkpoint(c);
            if (seq % 64 == 0) {
                w.Full(seq, mirror.Flat());
            }
        }
        cp.Step(0);
    }
    w.Flip(1);
    w.Close("self-test");
    Cp::TraceReader reader;
    std::string err;
    written = written && reader.Load(path, err);
    Cp::ReplayResult rr;
    const bool replayed = written && Cp::Replay(reader, rr);
    g.Check("a synthetic trace written, read back, replayed: every checkpoint matches",
            replayed && rr.Matched() && rr.checkpoints == 300 && rr.draws == 300 && rr.full_checks == 4,
            replayed ? fmt::format("{} checkpoints, {} mismatches, {} unreached{}", rr.checkpoints, rr.hash_mismatches,
                                   rr.unreached, rr.details.empty() ? "" : ": " + rr.details[0])
                     : err + rr.error);

    // the bridge's mirror: INDEX_BUFFER_SIZE leaves VGT_DMA_SIZE, CLEAR_STATE clears SH as the bridge does
    {
        TestMem m2;
        Rec rec;
        Cp::CommandProcessor c2(Cp::Options{.mode = Cp::Mode::Shadow, .memory = &m2, .work = &rec});
        Cp::BridgeMirror mir(c2);
        Stream t;
        t.Sh(0x2C4C, {0x77}).P(Op::IndexBufferSize, {55}).P(Op::ClearState, {0}).P(Op::DrawIndirect, {0, 0, 0, 9});
        m2.Put(kCmd, t.d);
        c2.SubmitGraphics(kCmd, static_cast<u32>(t.d.size()));
        c2.Run(0);
        g.Check("the bridge's view: VGT_DMA_SIZE kept apart, SH cleared by CLEAR_STATE, no initiator from an indirect "
                "draw — while the processor keeps the hardware's values",
                mir.Get(0x2C4C) == 0 && c2.Registers().Get(0x2C4C) == 0x77 &&
                    c2.Registers().Get(Cp::Reg::VgtDmaSize) == 0 && mir.Get(Cp::Reg::VgtDrawInitiator) == 0 &&
                    c2.Registers().Get(Cp::Reg::VgtDrawInitiator) == 9,
                "");
    }

    // corrupt traces
    {
        std::ofstream(dir / "bad.sspctrace", std::ios::binary) << "not a trace";
        Cp::TraceReader bad;
        std::string e2;
        g.Check("a file that is not a trace: refused with the reason", !bad.Load(dir / "bad.sspctrace", e2) &&
                                                                          e2.find("magic") != std::string::npos,
                e2);
        std::ifstream in(path, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        bytes.resize(bytes.size() - 10);
        std::ofstream(dir / "cut.sspctrace", std::ios::binary) << bytes;
        Cp::TraceReader cut;
        g.Check("a trace cut short: refused (truncated record)", !cut.Load(dir / "cut.sspctrace", e2), e2);
    }

    // the game's recorded traces (--gpu-trace), if there are any
    u32 found = 0, matched = 0;
    std::string notes;
    for (auto it = std::filesystem::directory_iterator(cfg.logs_dir / "gpu_trace", ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (it->path().extension() != ".sspctrace") {
            continue;
        }
        ++found;
        Cp::TraceReader tr;
        Cp::ReplayResult res;
        std::string e3;
        const bool ok = tr.Load(it->path(), e3) && Cp::Replay(tr, res) && res.Matched();
        matched += ok ? 1 : 0;
        notes += fmt::format(" {}: {} checkpoints, {} draws, {} dispatches{}", it->path().filename().string(),
                             res.checkpoints, res.draws, res.dispatches,
                             ok ? ""
                                : " DIFFERENT (" + (res.details.empty() ? e3 + res.error : res.details[0]) + ")");
    }
    if (found == 0) {
        g.Info("GpuCp(trace): no recorded game trace in logs\\gpu_trace (record one with --gpu-trace)");
    } else {
        g.Check(fmt::format("the game's recorded traces ({}): replayed, equal to the bridge at every checkpoint", found),
                matched == found, notes);
    }
}

// ---------------------------------------------------------------------------------------------------------------

/// P5.3 direct path (engine/render/native_render_ops.h): descriptions in the draws' trailing NOPs, the native stream's
/// runs at their tokens - in the stream's order, once.
void TestNativeDirect(TestGroup& g) {
    namespace R = Engine::Render;
    R::TableShape a;
    a.dwords = 48;
    a.refs = 2;
    a.sgpr[1] = 4;
    a.offset[1] = 0x10;
    R::TableShape b = a;
    b.offset[1] = 0x12;
    u32 ia = 0, ia2 = 0, ib = 0;
    const bool interned = R::InternShape(a, ia) && R::InternShape(a, ia2) && R::InternShape(b, ib);
    const R::TableShape* sa = R::ShapeOf(ia);
    g.Check("table shapes interned: one shape one id, another shape another; 0 and ids never given name none",
            interned && ia != 0 && ia == ia2 && ib != ia && sa != nullptr && std::memcmp(sa, &a, sizeof(a)) == 0 &&
                R::ShapeOf(0) == nullptr && R::ShapeOf(R::kMaxShapes - 1) == nullptr,
            fmt::format("ids {} {} {}", ia, ia2, ib));

    const auto draw_with_nop = [](Stream& s, u32 count, const std::array<u32, 4>& nop) {
        s.Draw(count);
        s.d.insert(s.d.end(), nop.begin(), nop.end());
    };
    const std::array<u32, 4> described{R::kDrawNopHeader, R::kDescribeMagic, R::DescribeWord(5, ia), ib};
    {
        R::SetDirect(true);
        Rig r;
        Stream s;
        draw_with_nop(s, 3, described);
        draw_with_nop(s, 4, {R::kDrawNopHeader, 0, 0, 0}); // the driver's NOP as it writes it
        draw_with_nop(s, 5, {R::kDrawNopHeader, R::kDescribeMagic, R::DescribeWord(5, R::kMaxShapes - 1), 0});
        const StepStatus st = r.Gfx(s);
        const auto& d = r.rec.draws;
        g.Check("a draw whose trailing NOP carries a description gets the pass and the VS/PS tables; the driver's plain "
                "NOP and an unknown shape id leave the draw undescribed (the frontend's own path)",
                st == StepStatus::Idle && d.size() == 3 && d[0].engine_pass == 5 && d[0].engine_vs.dwords == 48 &&
                    d[0].engine_vs.refs == 2 && d[0].engine_vs.sgpr[1] == 4 && d[0].engine_vs.offset[1] == 0x10 &&
                    d[0].engine_ps.offset[1] == 0x12 && d[1].engine_pass == Cp::kNoEnginePass &&
                    d[2].engine_pass == Cp::kNoEnginePass,
                fmt::format("{} draws", d.size()));
        R::SetDirect(false);
        Rig off;
        Stream s2;
        draw_with_nop(s2, 3, described);
        off.Gfx(s2);
        g.Check("without the direct path no description is read from a NOP",
                off.rec.draws.size() == 1 && off.rec.draws[0].engine_pass == Cp::kNoEnginePass, "");
    }
    {
        R::SetDirect(true);
        Rig r;
        R::StreamCursor c;
        constexpr u64 kAt = kCmd + 0x100; // where the engine's DCB would carry the token (the run is found by its content)
        u32 token[R::kStreamTokenDwords] = {}, unused[R::kStreamTokenDwords] = {};
        // the commit's registers: PS user data 0, VS user data 2
        u32* op = R::StreamReserve(c, kAt, 5);
        bool ok = op != nullptr;
        if (ok) {
            const u32 w[5] = {R::OpHeader(R::StreamOp::Registers, 4), 0x2C0C, 0x1111, 0x2C4E, 0x2222};
            std::memcpy(op, w, sizeof(w));
        }
        const bool first_joined = ok && R::StreamCommit(c, 5, token);
        R::StreamTokenWritten(c, kAt + 16);
        // render state: CB_BLEND0_CONTROL and colour target 0's extent (a pseudo register)
        op = R::StreamReserve(c, kAt + 16, 5);
        ok = ok && op != nullptr;
        if (ok) {
            const u32 w[5] = {R::OpHeader(R::StreamOp::State, 4), 0xA1E0, 0x12345, R::kStateCbExtent0, 0x00400030};
            std::memcpy(op, w, sizeof(w));
        }
        const bool state_joined = ok && R::StreamCommit(c, 5, unused);
        op = R::StreamReserve(c, kAt + 16, 1 + R::kStreamDrawWords);
        ok = ok && op != nullptr;
        if (ok) {
            const u32 w[8] = {R::OpHeader(R::StreamOp::Draw, R::kStreamDrawWords), 1, 1, 33, 0x10090000, 0, ia, ib};
            std::memcpy(op, w, sizeof(w));
        }
        const bool draw_joined = ok && R::StreamCommit(c, 8, unused);
        Stream s;
        s.Sh(0x2C0C, {0x0AAA}); // the game's packets before the run
        s.d.insert(s.d.end(), token, token + R::kStreamTokenDwords);
        s.Sh(0x2C0C, {0x0BBB}); // ... and after it
        s.Draw(7);
        const u64 runs0 = R::Direct().runs.load(), stale0 = R::Direct().runs_stale.load();
        r.Gfx(s);
        const auto& d = r.rec.draws;
        constexpr size_t kPs = static_cast<size_t>(Cp::Stage::Ps), kVs = static_cast<size_t>(Cp::Stage::Vs);
        g.Check("operations with no game packet between them share one run (one token); the run runs at its token: "
                "commit registers, render state, the native draw with its pass and tables - before the game's next "
                "packets",
                !first_joined && state_joined && draw_joined && token[1] == R::kStreamMagic && d.size() == 2 &&
                    d[0].index_count == 33 && d[0].index_base == 0x10090000 && d[0].engine_pass == 1 &&
                    d[0].engine_vs.dwords == 48 && d[0].engine_ps.offset[1] == 0x12 && d[0].user_data[kPs][0] == 0x1111 &&
                    d[0].user_data[kVs][2] == 0x2222 && d[0].cb_extent[0] == 0x00400030 &&
                    d[1].user_data[kPs][0] == 0x0BBB && d[1].engine_pass == Cp::kNoEnginePass &&
                    r.cp.Registers().Get(0xA1E0) == 0x12345 && R::Direct().runs.load() == runs0 + 1 &&
                    R::Direct().runs_stale.load() == stale0,
                fmt::format("{} draws", d.size()));
        r.rec.draws.clear();
        r.Gfx(s);
        g.Check("the same token again (a buffer replayed): its run was taken - nothing of it runs twice; counted",
                r.rec.draws.size() == 1 && r.rec.draws[0].index_count == 7 && R::Direct().runs_stale.load() == stale0 + 1,
                "");
        // the processor took the run: an operation for it now is not lost - it starts a run of its own
        op = R::StreamReserve(c, kAt + 16, 3);
        u32 token2[R::kStreamTokenDwords] = {};
        if (op != nullptr) {
            const u32 w[3] = {R::OpHeader(R::StreamOp::Registers, 2), 0x2C0C, 0x3333};
            std::memcpy(op, w, sizeof(w));
        }
        const bool late_joined = op == nullptr || R::StreamCommit(c, 3, token2);
        R::StreamTokenWritten(c, kAt + 0x200);
        // a game packet after the token (the DCB moved on): the next operation starts a new run
        op = R::StreamReserve(c, kAt + 0x208, 3);
        u32 token3[R::kStreamTokenDwords] = {};
        if (op != nullptr) {
            const u32 w[3] = {R::OpHeader(R::StreamOp::Registers, 2), 0x2C0C, 0x4444};
            std::memcpy(op, w, sizeof(w));
        }
        const bool moved_joined = op == nullptr || R::StreamCommit(c, 3, token3);
        Stream s3;
        s3.d.insert(s3.d.end(), token2, token2 + R::kStreamTokenDwords);
        s3.Draw(8);
        s3.d.insert(s3.d.end(), token3, token3 + R::kStreamTokenDwords);
        s3.Draw(9);
        r.rec.draws.clear();
        r.Gfx(s3);
        const auto& d3 = r.rec.draws;
        g.Check("an operation after the processor took its run, and one after a game packet: each a run and token of "
                "its own, run in the stream's order",
                !late_joined && !moved_joined && token2[2] != token[2] && token3[2] != token2[2] && d3.size() == 2 &&
                    d3[0].user_data[kPs][0] == 0x3333 && d3[1].user_data[kPs][0] == 0x4444,
                fmt::format("{} draws", d3.size()));
        Stream bad;
        bad.d.insert(bad.d.end(), {R::kStreamTokenHeader, R::kStreamMagic, 5u | (4000u << 16), 0xDEADu});
        bad.Draw(10);
        r.rec.draws.clear();
        const u64 stale1 = R::Direct().runs_stale.load();
        r.Gfx(bad);
        g.Check("a token naming no run (a block not given out with that generation): nothing run, counted; the "
                "stream goes on",
                r.rec.draws.size() == 1 && R::Direct().runs_stale.load() == stale1 + 1, "");
        R::SetDirect(false);
    }
}

} // namespace

std::vector<TestGroup> TestGpuCommandProcessor(const App::Config& cfg) {
    std::vector<TestGroup> out(7);
    out[0].name = "GpuCp(registers)";
    out[1].name = "GpuCp(packets)";
    out[2].name = "GpuCp(sync)";
    out[3].name = "GpuCp(state)";
    out[4].name = "GpuCp(errors)";
    out[5].name = "GpuCp(rings)";
    out[6].name = "GpuCp(trace)";
    TestRegisters(out[0]);
    TestPackets(out[1]);
    TestSync(out[2]);
    TestState(out[3]);
    TestErrors(out[4]);
    TestRings(out[5]);
    TestTrace(out[6], cfg);
    return out;
}

std::vector<TestGroup> TestGpuNativeDirect() {
    std::vector<TestGroup> out(1);
    out[0].name = "GpuCp(direct)";
    TestNativeDirect(out[0]);
    return out;
}

} // namespace SecondSon::Bridge
