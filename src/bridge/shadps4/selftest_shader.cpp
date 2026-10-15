// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — self-test of SecondSonPC's shader frontend (Phase 4, P4.3; docs/PHASE4_P43_SHADER_TRANSLATION.md §14).
// The bridge's layouts and functions serve as the oracle where they define the hardware's formats (V#/T#/S#
// bitfields, the fetch shader parser); the SRT walker interpreter is checked against the same bytes run natively.
//   Shader(identity) — footers, code extraction, content identity, unreadable/unmapped/corrupt programs;
//   Shader(validate) — revalidation per frame / per bind, rewritten programs, Invalidate, hash collisions;
//   Shader(runtime)  — stage configurations, runtime parameters from registers, the groups they live in;
//   Shader(fetch)    — SecondSonPC's fetch shader parser against the bridge's, refused forms;
//   Shader(descr)    — V#/T#/S# decoders against the bridge's layouts, locations, statuses, post-ops, vertex V#s;
//   Shader(srt)      — the SRT walker interpreter against native execution, refused instructions, misuse;
//   Shader(xlate)    — translations through the adapter: SPIR-V, reflection, errors, threads, Vulkan modules;
//   Shader(cache)    — the frontend: modules by key, hits/misses, pipelines, collisions, dirty-state reuse;
//   Shader(capture)  — the capture format's round trip, a corrupt file;
//   Shader(disk)     — the persistent shader cache (P4.14): a run's modules read by the next one instead of translated,
//                      the manifest, a second process, a torn write, a damaged record, another generation, the limit.
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "app/config.h"
#include "bridge/shadps4/selftest_threads.h"
#include "runtime/gpu/command_processor.h"
#include "runtime/gpu/cp_registers.h"
#include "runtime/gpu/shader/gcn_program.h"
#include "runtime/gpu/shader/pipeline_state.h"
#include "runtime/gpu/shader/runtime_params.h"
#include "runtime/gpu/shader/shader_cache.h"
#include "runtime/gpu/shader/shader_capture.h"
#include "runtime/gpu/shader/shader_disk_cache.h"
#include "runtime/gpu/shader/shader_frontend.h"
#include "runtime/gpu/shader/shader_resources.h"
#include "runtime/gpu/shader/shader_translator.h"
#include "runtime/gpu/vulkan/vk_foundation.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/recompiler.h"
#include "video_core/amdgpu/regs.h"
#include "video_core/amdgpu/resource.h"

#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"

namespace SecondSon::Bridge {

namespace {

namespace GS = Gpu::Shader;
namespace Cp = Gpu::Cp;

/// A guest memory of the test: `size` bytes from guest address `base`.
struct TestMemory final : GS::GuestReader {
    u64 base = 0x10000000;
    std::vector<u8> bytes = std::vector<u8>(1 << 20);
    u32 reads = 0;
    bool Read(u64 addr, void* dst, u64 n) override {
        ++reads;
        if (addr < base || addr + n > base + bytes.size() || addr + n < addr) {
            return false;
        }
        std::memcpy(dst, bytes.data() + (addr - base), n);
        return true;
    }
    void Put(u64 addr, std::span<const u32> words) {
        std::memcpy(bytes.data() + (addr - base), words.data(), words.size_bytes());
    }
    void Put(u64 addr, const void* p, size_t n) {
        std::memcpy(bytes.data() + (addr - base), p, n);
    }
};

/// The 28-byte footer of a program `code_bytes` long, with the compiler's `hash`.
std::array<u32, 7> Footer(u32 code_bytes, u64 hash) {
    std::array<u8, 28> b{};
    std::memcpy(b.data(), "OrbShdr", 7);
    b[7] = 7; // version
    const u32 bits = (code_bytes << 8) | (1u << 2);
    std::memcpy(b.data() + 8, &bits, 4);
    std::memcpy(b.data() + 16, &hash, 8);
    std::array<u32, 7> w{};
    std::memcpy(w.data(), b.data(), 28);
    return w;
}

/// A program: s_mov_b32 vcc_hi, <footer offset>, then `body`, then s_endpgm; its footer right after it.
std::vector<u32> Program(std::initializer_list<u32> body, u64 hash) {
    std::vector<u32> p{GS::kTokenMovVcchi, 0};
    p.insert(p.end(), body);
    p.push_back(0xBF810000u); // s_endpgm
    if (p.size() % 2 != 0) {
        p.push_back(0xBF800000u); // s_nop: the footer is qword-aligned
    }
    const u32 bytes = static_cast<u32>(p.size() * 4);
    p[1] = bytes / 8 - 1;
    const auto f = Footer(bytes, hash);
    p.insert(p.end(), f.begin(), f.end());
    return p;
}

constexpr u32 kVMovV0One = 0x7E0002F2u;   // v_mov_b32 v0, 1.0
constexpr u32 kExpMrt0 = 0xF800180Fu;     // exp mrt0 v0, v0, v0, v0 done vm
constexpr u32 kExpPos0 = 0xF80008CFu;     // exp pos0 v0, v0, v0, v0 done
constexpr u32 kSetpc = 0xBE802000u;       // s_setpc_b64 s[0:1]

// ---- Shader(identity)

void TestIdentity(TestGroup& g) {
    TestMemory mem;
    std::vector<u32> scan;
    const auto ps = Program({kVMovV0One, kExpMrt0, 0}, 0x1111);
    mem.Put(0x10001000, ps);
    GS::ProgramImage img;
    g.Check("a program with the token: footer where the first instruction points, code copied, hashed",
            GS::ReadProgram(mem, 0x10001000, GS::HwStage::Ps, img, scan) == GS::ProgramError::None &&
                img.footer.by_token && img.footer.length == (ps.size() - 7) * 4 && img.footer.shader_hash == 0x1111 &&
                img.code.size() == ps.size() - 7 && img.hash == GS::HashCode(img.code),
            fmt::format("length {}, by token {}", img.footer.length, img.footer.by_token));
    // without the token: the 64 KiB search
    auto no_token = ps;
    no_token[0] = 0xBF800000u; // s_nop instead of s_mov_b32 vcc_hi
    mem.Put(0x10010000, no_token);
    GS::ProgramImage img2;
    g.Check("without the token: the footer found by the search (as the bridge does)",
            GS::ReadProgram(mem, 0x10010000, GS::HwStage::Ps, img2, scan) == GS::ProgramError::None &&
                !img2.footer.by_token && img2.footer.offset == (ps.size() - 7) * 4,
            "");
    // the same code at another address: the same identity; another code with the same footer hash: another one
    mem.Put(0x10002000, ps);
    GS::ProgramImage img3;
    GS::ReadProgram(mem, 0x10002000, GS::HwStage::Ps, img3, scan);
    auto other = Program({kVMovV0One, kExpMrt0, 0x01010101}, 0x1111);
    mem.Put(0x10003000, other);
    GS::ProgramImage img4;
    GS::ReadProgram(mem, 0x10003000, GS::HwStage::Ps, img4, scan);
    g.Check("identity by content: one program at two addresses; two programs under one footer hash stay two",
            img3.hash == img.hash && !(img4.hash == img.hash) && img4.footer.shader_hash == img.footer.shader_hash, "");
    // errors, never a read past what the reader allows
    GS::ProgramImage e;
    g.Check("address 0: refused", GS::ReadProgram(mem, 0, GS::HwStage::Ps, e, scan) == GS::ProgramError::NullAddress,
            "");
    g.Check("outside the 40-bit GPU space: refused",
            GS::ReadProgram(mem, 1ull << 40, GS::HwStage::Ps, e, scan) == GS::ProgramError::OutOfRange, "");
    g.Check("unreadable (outside the test memory): refused",
            GS::ReadProgram(mem, 0x20000000, GS::HwStage::Ps, e, scan) == GS::ProgramError::Unreadable, "");
    std::vector<u32> zeros(0x4000 + 16, 0);
    mem.Put(0x10020000, zeros);
    g.Check("no footer in 64 KiB: refused",
            GS::ReadProgram(mem, 0x10020000, GS::HwStage::Ps, e, scan) == GS::ProgramError::NoFooter, "");
    auto bad = ps;
    const auto f0 = Footer(6, 0x22); // a length not a multiple of 4
    std::copy(f0.begin(), f0.end(), bad.end() - 7);
    mem.Put(0x10030000, bad);
    g.Check("a corrupt footer (length 6): refused",
            GS::ReadProgram(mem, 0x10030000, GS::HwStage::Ps, e, scan) == GS::ProgramError::BadLength, "");
    auto huge = ps;
    const auto f1 = Footer(GS::kMaxProgramBytes + 4, 0x22);
    std::copy(f1.begin(), f1.end(), huge.end() - 7);
    mem.Put(0x10040000, huge);
    g.Check("a footer claiming more than 1 MiB: refused",
            GS::ReadProgram(mem, 0x10040000, GS::HwStage::Ps, e, scan) == GS::ProgramError::TooLarge, "");
    // the GPU memory contract in this process: unmapped pages read as zeros, beyond 40 bits refused
    GS::GuestReader& game = GS::GameMemory();
    u32 probe = 0xFFFFFFFF;
    g.Check("the game reader: an address beyond 40 bits refused, an unmapped one read as zeros (no fault)",
            !game.Read(1ull << 41, &probe, 4) && game.Read(0x7F0000000000ull >> 8, &probe, 4) && probe == 0,
            fmt::format("{:#x}", probe));
    // settings: the bits of RSRC1/RSRC2 a translation reads
    const auto s = GS::DecodeSettings(GS::HwStage::Ps, 0x002C0041u, 0x0000000Au);
    g.Check("RSRC decoding: VGPRS, float modes, DX10_CLAMP, USER_SGPR",
            s.vgpr_blocks == 1 && s.sgpr_blocks == 1 && s.float_denorm32 == 0 && s.float_round32 == 0 &&
                s.dx10_clamp && s.user_sgprs == 5 && !s.scratch,
            fmt::format("vgpr {} sgpr {} user {}", s.vgpr_blocks, s.sgpr_blocks, s.user_sgprs));
}

// ---- Shader(validate)

GS::ContentHash ConstantHash(std::span<const u32>) {
    return {42, 42};
}

void TestValidation(TestGroup& g) {
    TestMemory mem;
    const auto a = Program({kVMovV0One, kExpMrt0, 0}, 0xA);
    const auto b = Program({kVMovV0One, kExpMrt0, 0x01010101}, 0xA); // same length, other code, same footer hash
    mem.Put(0x10001000, a);
    GS::ProgramCache pc;
    auto l1 = pc.Get(mem, 0x10001000, GS::HwStage::Ps);
    auto l2 = pc.Get(mem, 0x10001000, GS::HwStage::Ps);
    g.Check("first use reads; the same epoch trusts the address (no read)",
            l1.program != nullptr && l1.read && l2.program == l1.program && !l2.read && pc.Stats().trusted == 1, "");
    mem.Put(0x10001000, b); // rewritten in place
    auto l3 = pc.Get(mem, 0x10001000, GS::HwStage::Ps);
    g.Check("rewritten within the epoch: not seen yet (the per-frame fallback's window)", l3.program == l1.program, "");
    pc.NewEpoch();
    auto l4 = pc.Get(mem, 0x10001000, GS::HwStage::Ps);
    g.Check("next epoch: read again, another program (identity by content, not the footer hash)",
            l4.read && l4.changed && l4.program != l1.program && pc.Programs() == 2 && pc.Stats().changed == 1, "");
    mem.Put(0x10001000, a);
    pc.NewEpoch();
    auto l5 = pc.Get(mem, 0x10001000, GS::HwStage::Ps);
    g.Check("the old code back: its program again, no third one", l5.program == l1.program && pc.Programs() == 2, "");
    pc.SetValidation(GS::Validation::PerBind);
    mem.Put(0x10001000, b);
    auto l6 = pc.Get(mem, 0x10001000, GS::HwStage::Ps);
    g.Check("per bind: every use reads (the rewrite seen at once)", l6.read && l6.program == l4.program, "");
    pc.SetValidation(GS::Validation::PerFrame);
    pc.NewEpoch();
    pc.Get(mem, 0x10001000, GS::HwStage::Ps);
    mem.Put(0x10001000, a);
    pc.Invalidate(0x10001008, 4);
    auto l7 = pc.Get(mem, 0x10001000, GS::HwStage::Ps);
    g.Check("Invalidate over the code: read at the next use, within the epoch", l7.read && l7.program == l1.program,
            "");
    pc.Invalidate(0x10050000, 4);
    auto l8 = pc.Get(mem, 0x10001000, GS::HwStage::Ps);
    g.Check("Invalidate elsewhere: still trusted", !l8.read, "");
    // forced hash collisions: two programs with one 128-bit hash are refused, never merged
    GS::ProgramCache pc2;
    pc2.SetTestHash(&ConstantHash);
    mem.Put(0x10002000, a);
    mem.Put(0x10003000, b);
    auto c1 = pc2.Get(mem, 0x10002000, GS::HwStage::Ps);
    auto c2 = pc2.Get(mem, 0x10003000, GS::HwStage::Ps);
    g.Check("a forced content-hash collision: the second program refused (Collision), not taken for the first",
            c1.program != nullptr && c2.program == nullptr && c2.error == GS::ProgramError::Collision, "");
    // a stage is part of the identity
    auto v = pc.Get(mem, 0x10002000, GS::HwStage::Vs);
    auto p = pc.Get(mem, 0x10002000, GS::HwStage::Ps);
    g.Check("the same code as VS and PS: two programs (a stage translates differently)",
            v.program != nullptr && p.program != nullptr && v.program != p.program, "");
}

// ---- Shader(runtime)

void TestRuntime(TestGroup& g) {
    g.Check("stage configurations: VS only, ES/GS, LS/HS, LS/HS/ES/GS, other",
            GS::DecodeStages(0) == GS::StageConfig::Vs && GS::DecodeStages(0xB0) == GS::StageConfig::EsGs &&
                GS::DecodeStages(0x45) == GS::StageConfig::LsHs && GS::DecodeStages(0xAD) == GS::StageConfig::LsHsEsGs &&
                GS::DecodeStages(0x100) == GS::StageConfig::Unknown,
            "");
    Cp::RegisterFile regs;
    regs.WriteReg(GS::Reg::kPgmLo[0] + 2, 0x002C0041u | (0xFFu << 24)); // PS RSRC1 (priority etc. masked out)
    regs.WriteReg(GS::Reg::kPgmLo[0] + 3, 0x8u);                        // PS RSRC2: USER_SGPR 4
    regs.WriteReg(GS::Reg::SpiPsInputEna, 0x4002);
    regs.WriteReg(GS::Reg::SpiPsInputAddr, 0x4002);
    regs.WriteReg(GS::Reg::SpiPsInControl, 2);
    regs.WriteReg(GS::Reg::SpiPsInputCntl0, 0x20);
    regs.WriteReg(GS::Reg::SpiPsInputCntl0 + 1, 0x401);
    regs.WriteReg(GS::Reg::SpiPsInputCntl0 + 2, 0x7); // beyond NUM_INTERP: ignored
    regs.WriteReg(GS::Reg::PaScAaConfig, 2);
    regs.WriteReg(GS::Reg::CbColorControl, 1u << 4);
    regs.WriteReg(GS::Reg::CbTargetMask, 0xF);
    regs.WriteReg(GS::Reg::CbColor0Base, 0x1000);
    regs.WriteReg(GS::Reg::CbColor0Base + GS::Reg::CbColorInfoOffset, (10u << 2) | (4u << 8) | (1u << 13));
    regs.WriteReg(GS::Reg::CbBlend0Control, (1u << 30) | 15u); // SRC1_COLOR: dual source
    GS::RuntimeParams ps;
    g.Check("fragment runtime parameters from the registers",
            GS::BuildGraphicsRuntime(regs, GS::HwStage::Ps, GS::SwStage::Fragment, 0, 0, ps) &&
                ps.rsrc1 == ((0x002C0041u | (0xFFu << 24)) & GS::Rsrc1TranslationMask(GS::HwStage::Ps)) && ps.rsrc2 == 0x8u &&
                ps.num_interp == 2 && ps.ps_input_cntl[0] == 0x20 && ps.ps_input_cntl[1] == 0x401 &&
                ps.ps_input_cntl[2] == 0 && ps.aa_samples_log2 == 2 && ps.dual_source == 1 && ps.targets[0].bound == 1 &&
                ps.targets[0].info == ((10u << 2) | (4u << 8)) && ps.targets[1].bound == 0,
            fmt::format("rsrc1 {:#x} info {:#x} samples {}", ps.rsrc1, ps.targets[0].info, ps.aa_samples_log2));
    GS::RuntimeParams ps2;
    regs.WriteReg(GS::Reg::CbColor0Base + GS::Reg::CbColorInfoOffset, (10u << 2) | (4u << 8)); // FAST_CLEAR off
    GS::BuildGraphicsRuntime(regs, GS::HwStage::Ps, GS::SwStage::Fragment, 0, 0, ps2);
    g.Check("a register field the translation does not read (FAST_CLEAR) leaves the parameters equal",
            std::memcmp(&ps, &ps2, sizeof(ps)) == 0, "");
    regs.WriteReg(GS::Reg::CbColorControl, 0);
    GS::BuildGraphicsRuntime(regs, GS::HwStage::Ps, GS::SwStage::Fragment, 0, 0, ps2);
    g.Check("colour writes disabled (CB_COLOR_CONTROL.MODE 0): no target bound", ps2.targets[0].bound == 0, "");
    regs.WriteReg(GS::Reg::VgtPrimitiveType, GS::kPrimRectList);
    regs.WriteReg(GS::Reg::PaClClipCntl, (1u << 16) | (1u << 19) | (1u << 26));
    GS::RuntimeParams vs;
    g.Check("vertex runtime parameters: RECTLIST, clip control (masked), the indirect draw's SGPR offsets",
            GS::BuildGraphicsRuntime(regs, GS::HwStage::Vs, GS::SwStage::Vertex, 12, 13, vs) && vs.rect_list == 1 &&
                vs.clip_cntl == ((1u << 16) | (1u << 19)) && vs.vertex_sgpr_offset == 12 && vs.instance_sgpr_offset == 13,
            "");
    GS::RuntimeParams gs;
    g.Check("GS/HS/ES/LS: not prepared (refused with a reason, not guessed)",
            !GS::BuildGraphicsRuntime(regs, GS::HwStage::Gs, GS::SwStage::Geometry, 0, 0, gs) &&
                !GS::BuildGraphicsRuntime(regs, GS::HwStage::Vs, GS::SwStage::TessEval, 0, 0, gs),
            "");
    std::array<u32, Cp::kComputeCount> cs{};
    cs[GS::Reg::ComputeNumThreadX] = 64 | (5u << 16);
    cs[GS::Reg::ComputeNumThreadX + 1] = 1;
    cs[GS::Reg::ComputeNumThreadX + 2] = 1;
    cs[GS::Reg::ComputePgmRsrc2] = (1u << 7) | (4u << 15);
    GS::RuntimeParams c;
    GS::BuildComputeRuntime(cs, c);
    g.Check("compute runtime parameters: full group size (partial ignored), TGID, LDS",
            c.threads[0] == 64 && c.threads[1] == 1 && c.rsrc2 == ((1u << 7) | (4u << 15)), "");
    // the groups: every register read lives in kRuntimeGroups; the dynamic state groups are out
    bool all = true;
    for (u32 r : {GS::Reg::SpiPsInputEna, GS::Reg::SpiShaderColFormat, GS::Reg::CbBlend0Control, GS::Reg::PaClClipCntl,
                  GS::Reg::PaClVsOutCntl, GS::Reg::VgtInstanceStepRate0, GS::Reg::VgtShaderStagesEn, GS::Reg::PaScAaConfig,
                  GS::Reg::CbColor0Base, GS::Reg::CbColor0Base + 4, GS::Reg::VgtPrimitiveType, GS::Reg::kPgmLo[0] + 2,
                  GS::Reg::kPgmLo[1] + 3}) {
        all &= (GS::kRuntimeGroups & Cp::Bit(Cp::GroupOf(r))) != 0;
    }
    g.Check("every register the runtime parameters read is in their groups; viewport/scissor are not",
            all && (GS::kRuntimeGroups & (Cp::Bit(Cp::Group::Viewport) | Cp::Bit(Cp::Group::Scissor))) == 0, "");

    // P4.5: the colour write mask in the image's component order (CB_TARGET_MASK enables export components; the
    // program writes image component i from export component Map(i) of the COMP_SWAP swizzle)
    const auto info = [](u32 format, u32 swap) { return (format << 2) | (swap << 11); };
    g.Check("write mask: 16_16_16_16 ALT (the G-buffer normal, CB_COLOR_INFO 0x830) 0x3 -> G and B (0x6), R kept",
            info(12, 1) == 0x830 && GS::ImageWriteMask(0x830, 0x3) == 0x6, fmt::format("{:#x}", GS::ImageWriteMask(0x830, 0x3)));
    g.Check("write mask: standard swap unchanged (0x3, 0x5, 0xF), ALT full mask unchanged",
            GS::ImageWriteMask(info(12, 0), 0x3) == 0x3 && GS::ImageWriteMask(info(10, 0), 0x5) == 0x5 &&
                GS::ImageWriteMask(info(10, 0), 0xF) == 0xF && GS::ImageWriteMask(info(12, 1), 0xF) == 0xF,
            "");
    g.Check("write mask: STD_REV 4 components (A,B,G,R): export R -> image A; ALT_REV (A,R,G,B): export R -> image G",
            GS::ImageWriteMask(info(12, 2), 0x1) == 0x8 && GS::ImageWriteMask(info(12, 3), 0x1) == 0x2 &&
                GS::ImageWriteMask(info(12, 3), 0x8) == 0x1,
            fmt::format("{:#x} {:#x}", GS::ImageWriteMask(info(12, 2), 0x1), GS::ImageWriteMask(info(12, 3), 0x1)));
    g.Check("write mask: 16_16 ALT (R,A): export R and A -> image R and G (the format has no B, A to write)",
            (GS::ImageWriteMask(info(5, 1), 0x9) & 0x3) == 0x3 && (GS::ImageWriteMask(info(5, 1), 0x8) & 0x3) == 0x2 &&
                (GS::ImageWriteMask(info(5, 1), 0x2) & 0x3) == 0,
            fmt::format("{:#x}", GS::ImageWriteMask(info(5, 1), 0x9)));
    Cp::RegisterFile cregs;
    cregs.WriteReg(GS::Reg::CbColorControl, 1u << 4);
    cregs.WriteReg(GS::Reg::CbTargetMask, 0x3u << 4); // MRT1 exports R, G
    cregs.WriteReg(GS::Reg::CbColor0Base + GS::Reg::CbColorStride, 0x1000);
    cregs.WriteReg(GS::Reg::CbColor0Base + GS::Reg::CbColorStride + GS::Reg::CbColorInfoOffset, 0x830);
    GS::GraphicsPipelineState pst;
    GS::BuildGraphicsPipelineState(cregs, 1, 2, 0x2, pst);
    g.Check("pipeline state: an ALT target's partial CB_TARGET_MASK reaches Vulkan in the image's order (0x3 -> 0x6)",
            pst.targets[1].write_mask == 0x6, fmt::format("{:#x}", pst.targets[1].write_mask));
}

// ---- Shader(fetch)

void TestFetch(TestGroup& g) {
    // s_load_dwordx4 s[8:11], s[2:3], 0x0; s_load_dwordx4 s[12:15], s[2:3], 0x4; s_waitcnt;
    // buffer_load_format_xyzw v[4:7], v0, s[8:11], 0 idxen; tbuffer_load_format_xy v[8:9], v0, s[12:15], dfmt 10 nfmt 4;
    // v_add_i32 v0, s5, v0; s_setpc_b64
    const u32 smrd0 = 0xC0000000u | (2u << 22) | (8u << 15) | (1u << 9) | (1u << 8) | 0;
    const u32 smrd1 = 0xC0000000u | (2u << 22) | (12u << 15) | (1u << 9) | (1u << 8) | 4;
    const u32 wait = 0xBF8C0000u | 0x007F;
    const u32 mubuf0 = (0x38u << 26) | (3u << 18) | (1u << 13), mubuf1 = (2u << 16) | (4u << 8) | 0;
    const u32 mtbuf0 = (0x3Au << 26) | (10u << 19) | (4u << 23) | (1u << 16) | (1u << 13), mtbuf1 = (3u << 16) | (8u << 8);
    const u32 vadd = (37u << 25) | (0u << 17) | (0u << 9) | 5u;
    std::vector<u32> code{smrd0, smrd1, wait, mubuf0, mubuf1, mtbuf0, mtbuf1, vadd, kSetpc};
    GS::FetchShader ours;
    std::string err;
    const bool ok = GS::ParseFetchShaderCode(code, 0, ours, err);
    // the bridge's parser on the same code (through the user data pair, as the translator finds it)
    std::array<u32, 16> ud{};
    const u64 ptr = reinterpret_cast<u64>(code.data());
    ud[0] = static_cast<u32>(ptr);
    ud[1] = static_cast<u32>(ptr >> 32);
    const u32 dummy[4] = {};
    Shader::Info info(Shader::HwStage::Vertex, Shader::SwStage::Vertex,
                      Shader::ShaderParams{.user_data = std::span<const u32, 16>(ud), .code = dummy, .hash = 0});
    info.has_fetch_shader = true;
    info.fetch_shader_sgpr_base = 0;
    Shader::Gcn::FetchShaderData theirs;
    const bool tok = Shader::Gcn::ParseFetchShader(info, theirs);
    bool same = ok && tok && ours.attributes.size() == theirs.attributes.size() && ours.size == theirs.size &&
                ours.vertex_offset_sgpr == theirs.vertex_offset_sgpr &&
                ours.instance_offset_sgpr == theirs.instance_offset_sgpr;
    for (size_t i = 0; same && i < ours.attributes.size(); ++i) {
        const auto& a = ours.attributes[i];
        const auto& b = theirs.attributes[i];
        same = a.dest_vgpr == b.dest_vgpr && a.num_elements == b.num_elements && a.sgpr_base == b.sgpr_base &&
               a.dword_offset == b.dword_offset && a.instance_data == b.instance_data &&
               a.data_format == b.data_format && a.num_format == b.num_format && (a.inst_offset & 0xFF) == b.inst_offset;
    }
    g.Check("the driver's fetch shader form: SecondSonPC's parser equals the bridge's (attributes, offsets, size)",
            same, fmt::format("ours {} attributes ({}), bridge {}", ours.attributes.size(), err, theirs.attributes.size()));
    g.Check("the parsed fields: two attributes, typed format of the second, vertex offset in s5",
            ok && ours.attributes.size() == 2 && ours.attributes[0].num_elements == 4 && ours.attributes[1].num_elements == 2 &&
                ours.attributes[1].data_format == 10 && ours.attributes[1].num_format == 4 && ours.vertex_offset_sgpr == 5 &&
                ours.size == code.size() * 4,
            "");
    std::vector<u32> cut(code.begin(), code.end() - 1);
    g.Check("no s_setpc_b64 within the code read: an error (the bridge would read on past it)",
            !GS::ParseFetchShaderCode(cut, 0, ours, err) && err.find("s_setpc") != std::string::npos, err);
    std::vector<u32> odd{0xBF810000u /*s_endpgm*/, kSetpc};
    g.Check("another instruction: an error naming it (the bridge asserts)",
            !GS::ParseFetchShaderCode(odd, 0, ours, err) && !err.empty(), err);
    std::vector<u32> unset{mubuf0, mubuf1, kSetpc};
    g.Check("a buffer load whose V# registers were never loaded: an error (the bridge would read uninitialised data)",
            !GS::ParseFetchShaderCode(unset, 0, ours, err), err);
}

// ---- Shader(descr)

void TestDescriptors(TestGroup& g) {
    std::mt19937 rng(1234);
    bool v_ok = true, t_ok = true, s_ok = true;
    for (int i = 0; i < 2000; ++i) {
        GS::BufferDesc v;
        for (auto& w : v.raw) {
            w = rng();
        }
        AmdGpu::Buffer b;
        std::memcpy(&b, v.raw.data(), 16);
        v_ok &= v.Base() == b.base_address && v.Stride() == b.stride && v.SwizzleEnable() == bool(b.swizzle_enable) &&
                v.NumRecords() == b.num_records && v.DstSel(0) == b.dst_sel_x && v.DstSel(3) == b.dst_sel_w &&
                v.NumFormat() == b.num_format && v.DataFormat() == b.data_format && v.ElementSize() == b.element_size &&
                v.IndexStride() == b.index_stride && v.AddTidEnable() == bool(b.add_tid_enable) && v.Mtype() == b.mtype &&
                v.Type() == b.type && static_cast<u32>(v.Size()) == b.GetSize(); // the bridge's size is 32-bit
        GS::ImageDesc t;
        for (auto& w : t.raw) {
            w = rng();
        }
        AmdGpu::Image im;
        std::memcpy(&im, t.raw.data(), 32);
        t_ok &= t.Base() == im.Address() && t.DataFormat() == im.data_format && t.NumFormat() == im.num_format &&
                t.Width() == im.width + 1 && t.Height() == im.height + 1 && t.DstSel(1) == im.dst_sel_y &&
                t.BaseLevel() == im.base_level && t.LastLevel() == im.last_level && t.TilingIndex() == im.tiling_index &&
                t.Type() == im.type && t.Depth() == im.depth + 1 && t.Pitch() == im.pitch + 1 &&
                t.BaseArray() == im.base_array && t.LastArray() == im.last_array && t.IsImage() == im.Valid();
        GS::SamplerDesc s;
        for (auto& w : s.raw) {
            w = rng();
        }
        AmdGpu::Sampler sa;
        std::memcpy(&sa, s.raw.data(), 16);
        s_ok &= s.ClampX() == u32(sa.clamp_x.Value()) && s.MaxAniso() == u32(sa.max_aniso.Value()) &&
                s.ForceUnnormalized() == bool(sa.force_unnormalized.Value()) &&
                s.ForceDegamma() == bool(sa.force_degamma.Value()) && s.FilterMode() == u32(sa.filter_mode.Value()) &&
                s.MinLod() == sa.min_lod.Value() && s.MaxLod() == sa.max_lod.Value() &&
                s.MipFilter() == u32(sa.mip_filter.Value()) && s.BorderColorPtr() == sa.border_color_ptr.Value() &&
                s.InRange() == sa.Valid();
    }
    g.Check("V# fields (2000 random descriptors) against the bridge's AmdGpu::Buffer layout", v_ok, "");
    g.Check("T# fields (2000 random descriptors) against the bridge's AmdGpu::Image layout", t_ok, "");
    g.Check("S# fields (2000 random descriptors) against the bridge's AmdGpu::Sampler layout", s_ok, "");

    // locations in the flattened user data
    std::vector<u32> flat(24);
    for (u32 i = 0; i < flat.size(); ++i) {
        flat[i] = 100 + i;
    }
    GS::SharpLocation single{.summary = GS::SharpLocation::Summary::SingleLoad, .dwords = 4};
    single.offsets[0] = 18;
    std::array<u32, 4> out{};
    GS::SharpLocation multi{.summary = GS::SharpLocation::Summary::MultiLoad, .dwords = 4, .load_mask = 0b0101};
    multi.offsets = {3, 0, 7, 0};
    multi.immediates = {0, 0xAA, 0, 0xBB};
    std::array<u32, 4> out2{};
    GS::SharpLocation past = single;
    past.offsets[0] = 22;
    GS::SharpLocation invalid{};
    g.Check("locations: one load, per-dword loads/immediates, past the end, invalid",
            GS::FetchSharp(single, flat, out) && out[0] == 118 && out[3] == 121 && GS::FetchSharp(multi, flat, out2) &&
                out2[0] == 103 && out2[1] == 0xAA && out2[2] == 107 && out2[3] == 0xBB &&
                !GS::FetchSharp(past, flat, out) && !GS::FetchSharp(invalid, flat, out),
            "");

    // resolution: statuses and post-ops
    GS::ShaderReflection r;
    r.hw = GS::HwStage::Ps;
    r.flatbuf_dwords = 16;
    auto at = [](u16 off, u8 dwords) {
        GS::SharpLocation l{.summary = GS::SharpLocation::Summary::SingleLoad, .dwords = dwords};
        l.offsets[0] = off;
        return l;
    };
    r.buffers.push_back({.sharp = at(0, 4)});                                                   // ok
    r.buffers.push_back({.sharp = at(4, 4), .post_op = GS::SharpPostOp::OffsetByProgramBase});  // inline constant buffer
    r.buffers.push_back({.sharp = at(20, 4)});                                                  // past the end
    r.buffers.push_back({.kind = GS::BufferKind::Flatbuf});                                     // special: no descriptor
    r.images.push_back({.sharp = at(8, 8), .post_op = GS::SharpPostOp::ConvertCubeTo2DArray});
    r.samplers.push_back({.sharp = at(12, 4), .post_op = GS::SharpPostOp::ForceRepeatXyzClamp});
    std::array<u32, 16> ud{};
    ud[0] = 0x2000; ud[1] = 0x10 << 16; ud[2] = 4; ud[3] = (4u << 15);       // base 0x2000, stride 16, 4 records
    ud[4] = 0x100; ud[5] = 0; ud[6] = 64; ud[7] = 0;                          // inline: 0x100 + the program's address
    ud[8] = 0x123; ud[9] = (10u << 20); ud[10] = 255 | (255u << 14); ud[11] = 0xBu << 28; // a cube, 256x256
    ud[12] = 0x1FF; ud[13] = 0; ud[14] = 0; ud[15] = 0;                       // clamps set, forced to wrap
    GS::ResourceBindingPlan plan;
    GS::SrtProgram none;
    TestMemory mem;
    GS::ResolveResources(r, none, GS::BindingStart{3, 1}, nullptr, ud, 0x10000000, mem, plan);
    const bool statuses = plan.buffers.size() == 4 && plan.buffers[0].status == GS::ResourceStatus::Ok &&
                          plan.buffers[0].size == 64 && plan.buffers[0].binding == 3 &&
                          plan.buffers[1].address == 0x10000100 && plan.buffers[2].status == GS::ResourceStatus::BadLocation &&
                          plan.buffers[3].status == GS::ResourceStatus::Ok && plan.buffers[3].binding == 6;
    g.Check("buffers: a V#, an inline constant buffer (+ the program's address), a location past the data, a special "
            "buffer (no descriptor); bindings from the stage's start",
            statuses, fmt::format("inline {:#x}, status {}", plan.buffers.size() > 1 ? plan.buffers[1].address : 0,
                                  plan.buffers.size() > 2 ? GS::ResourceStatusName(plan.buffers[2].status) : "?"));
    g.Check("a cube T#: seen as a 2D array of 6 layers per cube (the bridge's post-op), binding after the buffers",
            plan.images.size() == 1 && plan.images[0].desc.Type() == GS::kImageType2DArray &&
                plan.images[0].desc.Depth() == (0x1FF + 1) * 6 && plan.images[0].binding == 7 &&
                plan.images[0].status == GS::ResourceStatus::Ok,
            "");
    g.Check("a sampler forced to wrap (post-op), binding after the images",
            plan.samplers.size() == 1 && plan.samplers[0].desc.ClampX() == 0 && plan.samplers[0].binding == 8, "");
    const auto end = GS::EndOf(GS::BindingStart{3, 1}, plan);
    g.Check("the next stage's bindings start after these (4 buffers, 1 image, 1 sampler)",
            end.unified == 9 && end.buffer == 5, "");
    // wrong types and null descriptors are reported, not replaced
    ud[3] = 2u << 30; // a T#'s type bits in the first V#
    ud[8] = 0;        // T# base 0
    GS::ResolveResources(r, none, GS::BindingStart{}, nullptr, ud, 0, mem, plan);
    g.Check("a V# with image type bits: WrongType; a T# with base 0: Null (reported, never a silent null)",
            plan.buffers[0].status == GS::ResourceStatus::WrongType &&
                plan.images[0].status == GS::ResourceStatus::Null &&
                plan.status_counts[static_cast<size_t>(GS::ResourceStatus::WrongType)] == 1,
            "");
    // vertex V#s from the vertex buffer table in guest memory; a typed load's format replaces the V#'s
    GS::ShaderReflection vr;
    vr.hw = GS::HwStage::Vs;
    GS::FetchShader fetch;
    fetch.present = true;
    fetch.attributes.push_back({.dest_vgpr = 4, .num_elements = 4, .sgpr_base = 2, .dword_offset = 4, .inst_offset = 8});
    fetch.attributes.push_back({.num_elements = 2, .sgpr_base = 2, .dword_offset = 8, .data_format = 10, .num_format = 4});
    const u32 vtable[12] = {0, 0, 0, 0, 0x3000, 12 << 16, 100, (7u << 15), 0x4000, 0, 50, (13u << 15)};
    mem.Put(0x10000100, vtable, sizeof(vtable));
    std::array<u32, 16> vud{};
    vud[2] = 0x10000100;
    GS::ResolveResources(vr, none, GS::BindingStart{}, &fetch, vud, 0, mem, plan);
    g.Check("vertex attributes: V#s read from the table (+ the instruction offset), a typed load's format",
            plan.vertices.size() == 2 && plan.vertices[0].address == 0x3008 && plan.vertices[0].size == 1200 &&
                plan.vertices[1].desc.DataFormat() == 10 && plan.vertices[1].desc.NumFormat() == 4 &&
                plan.vertices[1].read[3] == (13u << 15),
            "");
    vud[2] = 0x20000000; // the table outside readable memory
    GS::ResolveResources(vr, none, GS::BindingStart{}, &fetch, vud, 0, mem, plan);
    g.Check("an unreadable vertex buffer table: Unresolved",
            plan.vertices[0].status == GS::ResourceStatus::Unresolved, "");
}

// ---- Shader(srt)

/// The SRT walker's test memory: the same tables twice -- for the native walk with host pointers, for the
/// interpreter at a guest address (the interpreter takes 40-bit addresses only; the process's low addresses may all be
/// the guest address space's).
struct WalkerMemory final : GS::GuestReader {
    static constexpr u64 kGuest = 0x7F00000000ull;
    static constexpr u64 kSize = 1 << 16;
    std::vector<u8> host = std::vector<u8>(kSize);  ///< the native walk reads this (nested pointers: host addresses)
    std::vector<u8> guest = std::vector<u8>(kSize); ///< the interpreter reads this at kGuest (nested: guest addresses)
    bool Read(u64 addr, void* dst, u64 n) override {
        if (addr < kGuest || addr + n > kGuest + kSize) {
            return false;
        }
        std::memcpy(dst, guest.data() + (addr - kGuest), n);
        return true;
    }
};

using Walker = void __attribute__((sysv_abi)) (*)(const u32*, u32*);

/// Runs `code` natively (an executable copy) on the same user data.
bool RunNative(std::span<const u8> code, const u32* ud, u32* flat) {
    void* x = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (x == nullptr) {
        return false;
    }
    std::memcpy(x, code.data(), code.size());
    FlushInstructionCache(GetCurrentProcess(), x, code.size());
    reinterpret_cast<Walker>(x)(ud, flat);
    VirtualFree(x, 0, MEM_RELEASE);
    return true;
}

void TestSrt(TestGroup& g) {
    WalkerMemory low;
    const u64 host_base = reinterpret_cast<u64>(low.host.data());
    for (auto* mem : {&low.host, &low.guest}) {
        u32* t = reinterpret_cast<u32*>(mem->data());
        for (u32 i = 0; i < 64; ++i) {
            t[i] = 0xA0000000u + i;
        }
        u32* t2 = reinterpret_cast<u32*>(mem->data() + 0x100);
        for (u32 i = 0; i < 16; ++i) {
            t2[i] = 0xB0000000u + i;
        }
        const u64 second = (mem == &low.host ? host_base : WalkerMemory::kGuest) + 0x100;
        t[8] = static_cast<u32>(second); // a nested pointer at byte 32
        t[9] = static_cast<u32>(second >> 32);
    }
    std::array<u32, 16> ud{}, ud_native{};
    ud[4] = static_cast<u32>(WalkerMemory::kGuest);
    ud[5] = static_cast<u32>(WalkerMemory::kGuest >> 32);
    ud[6] = 3; // a dynamic offset (dwords)
    ud_native = ud;
    ud_native[4] = static_cast<u32>(host_base);
    ud_native[5] = static_cast<u32>(host_base >> 32);
    // the generator's forms: root pointer from s[4:5], copies, a dynamic offset, a nested pointer, min, the stack
    const std::vector<u8> walker = {
        0x57,                                                       // push rdi
        0x48, 0x8B, 0x7F, 0x10,                                     // mov rdi, [rdi + 16]   (s[4:5])
        0x49, 0xBA, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, // mov r10, 0xFFFFFFFFFFFF
        0x4C, 0x21, 0xD7,                                           // and rdi, r10
        0x44, 0x8B, 0x57, 0x00,                                     // mov r10d, [rdi + 0]
        0x44, 0x89, 0x56, 0x40,                                     // mov [rsi + 64], r10d   (flat[16] = t[0])
        0x44, 0x8B, 0x57, 0x14,                                     // mov r10d, [rdi + 20]
        0x44, 0x89, 0x56, 0x44,                                     // mov [rsi + 68], r10d   (flat[17] = t[5])
        0x44, 0x8B, 0x56, 0x18,                                     // mov r10d, [rsi + 24]   (s6 = 3)
        0x41, 0xC1, 0xE2, 0x02,                                     // shl r10d, 2
        0x45, 0x89, 0xD2,                                           // mov r10d, r10d
        0x46, 0x8B, 0x14, 0x17,                                     // mov r10d, [rdi + r10]
        0x44, 0x89, 0x56, 0x48,                                     // mov [rsi + 72], r10d   (flat[18] = t[3])
        0x57,                                                       // push rdi
        0x48, 0x8B, 0x7F, 0x20,                                     // mov rdi, [rdi + 32]   (the nested pointer)
        0x49, 0xBA, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, // mov r10, 0xFFFFFFFFFFFF
        0x4C, 0x21, 0xD7,                                           // and rdi, r10
        0x44, 0x8B, 0x57, 0x0C,                                     // mov r10d, [rdi + 12]
        0x44, 0x89, 0x56, 0x4C,                                     // mov [rsi + 76], r10d   (flat[19] = t2[3])
        0x5F,                                                       // pop rdi
        0x44, 0x8B, 0x56, 0x18,                                     // mov r10d, [rsi + 24]
        0xB9, 0x02, 0x00, 0x00, 0x00,                               // mov ecx, 2
        0x41, 0x39, 0xCA,                                           // cmp r10d, ecx
        0x44, 0x0F, 0x47, 0xD1,                                     // cmova r10d, ecx      (umin(s6, 2))
        0x41, 0x52,                                                 // push r10
        0x44, 0x8B, 0x56, 0x18,                                     // mov r10d, [rsi + 24]
        0x44, 0x03, 0x14, 0x24,                                     // add r10d, [rsp]
        0x48, 0x83, 0xC4, 0x08,                                     // add rsp, 8
        0x44, 0x89, 0x56, 0x50,                                     // mov [rsi + 80], r10d   (flat[20] = s6 + min)
        0x5F,                                                       // pop rdi
        0xC3,                                                       // ret
    };
    GS::SrtProgram srt;
    std::string err;
    const bool compiled = srt.Compile(walker, err);
    std::vector<u32> native(24, 0), ours(24, 0);
    std::copy(ud_native.begin(), ud_native.end(), native.begin());
    std::copy(ud.begin(), ud.end(), ours.begin());
    RunNative(walker, ud_native.data(), native.data());
    GS::SrtProgram::RunStats stats;
    const bool ran = compiled && srt.Run(ud, ours, low, stats, err);
    g.Check("the walker's forms decoded (pointer loads, masks, copies, dynamic offset, nested pointer, min, stack)",
            compiled, err);
    g.Check("the interpreter writes the flattened user data the native walk writes (word for word)",
            ran && std::equal(ours.begin() + 16, ours.end(), native.begin() + 16) && ours[16] == 0xA0000000u && ours[18] == 0xA0000003u && ours[19] == 0xB0000003u &&
                ours[20] == 5,
            fmt::format("{} reads; flat[16..20] {:#x} {:#x} {:#x} {:#x} {}", stats.reads, ours[16], ours[17], ours[18],
                        ours[19], ours[20]));
    g.Check("the walk names the user data registers it read (s[4:5] the pointer, s6 through the flattened copy) and "
            "leaves the registers' part of the flattened data alone (P5.3)",
            ran && stats.user_data_read == 0x70 && !stats.user_data_written,
            fmt::format("read {:#x}, written {}", stats.user_data_read, stats.user_data_written));
    // a pointer to unreadable memory: zeros (the bridge's patched load), counted, no fault
    std::array<u32, 16> bad = ud;
    bad[4] = 0x1000; // a low unmapped address in the interpreter's eyes
    bad[5] = 0;
    std::vector<u32> z(24, 0xFFFFFFFF);
    std::copy(bad.begin(), bad.end(), z.begin());
    GS::SrtProgram::RunStats st2;
    const bool ran2 = srt.Run(bad, z, low, st2, err);
    g.Check("an unreadable table: the copies read 0 and the refusals are counted (the next walk reads again)",
            ran2 && z[16] == 0 && z[17] == 0 && st2.faults > 0, fmt::format("faults {}", st2.faults));
    // refusals
    GS::SrtProgram s2;
    const u8 foreign[] = {0x48, 0x89, 0xC8, 0xC3}; // mov rax, rcx; ret
    g.Check("an instruction the generator never emits: refused, named", !s2.Compile(foreign, err) && !err.empty(), err);
    const u8 no_ret[] = {0x57, 0x5F};
    g.Check("no ret at the end: refused", !s2.Compile(no_ret, err), err);
    const u8 unbalanced[] = {0x57, 0xC3}; // push rdi; ret
    std::vector<u32> f3(16);
    g.Check("a ret with the stack not empty: refused at run time",
            s2.Compile(unbalanced, err) && !s2.Run(ud, f3, low, stats, err), err);
    const u8 store_past[] = {0x44, 0x89, 0x56, 0x7C, 0xC3}; // mov [rsi + 124], r10d (flat[31]); ret
    std::vector<u32> f4(20);
    g.Check("a store past the flattened buffer: refused (nothing written past it)",
            s2.Compile(store_past, err) && !s2.Run(ud, f4, low, stats, err), err);
}

// ---- Shader(xlate)

GS::DeviceProfile TestProfile() {
    GS::DeviceProfile p;
    p.max_viewport_width = 16384;
    p.max_viewport_height = 16384;
    p.max_shared_memory_size = 49152;
    p.subgroup_size = 32;
    p.sparse_page_shift = 16;
    p.support_int16 = p.support_int64 = p.support_float64 = true;
    p.support_fp32_denorm_preserve = p.support_fp32_denorm_flush = true;
    p.needs_clip_distance_emulation = true;
    p.depth_clip_control = true;
    p.source = "test";
    return p;
}

void TestTranslate(TestGroup& g) {
    GS::ShaderTranslator* tr = GS::GetShaderTranslator();
    if (tr == nullptr) {
        g.Check("the adapter is installed", false, "");
        return;
    }
    const GS::DeviceProfile profile = TestProfile();
    TestMemory mem;
    auto ps = Program({kVMovV0One, kExpMrt0, 0}, 0x5151);
    const u32 code_dw = static_cast<u32>(ps.size() - 7);
    GS::TranslationRequest req;
    req.hw = GS::HwStage::Ps;
    req.sw = GS::SwStage::Fragment;
    req.address = 0x10001000;
    req.code = std::span<const u32>(ps.data(), code_dw);
    req.profile = &profile;
    req.memory = &mem;
    req.runtime.hw = GS::HwStage::Ps;
    req.runtime.sw = GS::SwStage::Fragment;
    req.runtime.col_format = 9; // mrt0: ABGR_32 (an uncompressed export)
    req.runtime.targets[0] = {.bound = 1, .info = 10u << 2}; // 8_8_8_8 unorm
    req.footer_hash = 0x5151;
    const auto res = tr->Translate(req);
    g.Check("a fragment program through the adapter: SPIR-V (magic), one colour export, no resources",
            res.Ok() && res.spirv.size() > 5 && res.spirv[0] == 0x07230203u && res.reflection.mrt_mask == 1 &&
                res.reflection.buffers.empty() && res.reflection.images.empty(),
            fmt::format("{}: {} ({} words, mrt {:#x})", GS::TranslationErrorName(res.error), res.detail,
                        res.spirv.size(), res.reflection.mrt_mask));
    const auto res2 = tr->Translate(req);
    g.Check("the same request again: the same SPIR-V (deterministic)", res2.Ok() && res2.spirv == res.spirv, "");
    // a vertex program
    auto vs = Program({kVMovV0One, kExpPos0, 0}, 0x5252);
    GS::TranslationRequest vreq = req;
    vreq.hw = GS::HwStage::Vs;
    vreq.sw = GS::SwStage::Vertex;
    vreq.code = std::span<const u32>(vs.data(), vs.size() - 7);
    vreq.runtime.hw = GS::HwStage::Vs;
    vreq.runtime.sw = GS::SwStage::Vertex;
    const auto vres = tr->Translate(vreq);
    g.Check("a vertex program through the adapter: SPIR-V", vres.Ok() && !vres.spirv.empty(),
            fmt::format("{}: {}", GS::TranslationErrorName(vres.error), vres.detail));
    // requests the adapter refuses with a reason (never a crash)
    GS::TranslationRequest bad = req;
    bad.hw = GS::HwStage::Gs;
    bad.sw = GS::SwStage::Geometry;
    const auto b1 = tr->Translate(bad);
    bad = req;
    bad.profile = nullptr;
    const auto b2 = tr->Translate(bad);
    bad = req;
    bad.memory = nullptr;
    bad.flatbuf = {};
    const auto b3 = tr->Translate(bad);
    g.Check("refused with a reason: a geometry program, no profile, no memory and no flattened data",
            b1.error == GS::TranslationError::BadRequest && b2.error == GS::TranslationError::BadRequest &&
                b3.error == GS::TranslationError::BadRequest,
            b1.detail);
    // a given flattened user data of the wrong size: the SRT error, not a translation of other data
    // (this program has no SRT walk: its flattened data is the 16 registers)
    GS::TranslationRequest given = req;
    const std::vector<u32> wrong(20, 0);
    given.flatbuf = wrong;
    const auto g1 = tr->Translate(given);
    g.Check("a given flattened user data whose size the translation does not have: refused (Srt)",
            g1.error == GS::TranslationError::Srt, g1.detail);
    // runtime information: SecondSonPC's parameters against themselves through the adapter (equal), and one changed
    GS::RuntimeParams rp = req.runtime;
    rp.num_interp = 1;
    rp.ps_input_cntl[0] = 0x1;
    std::vector<u8> ref(sizeof(rp) * 0);
    std::string diff;
    g.Check("CompareRuntime: a reference of the wrong size refused with a reason",
            !tr->CompareRuntime(rp, profile, ref, diff) && !diff.empty(), diff);
    // threads: the adapter serializes its own translations; results equal
    std::atomic<u32> same{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&] {
            for (int k = 0; k < 4; ++k) {
                const auto r = tr->Translate(req);
                same += (r.Ok() && r.spirv == res.spirv) ? 1 : 0;
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    g.Check("4 threads x 4 translations at once: all succeed, all equal", same.load() == 16,
            fmt::format("{} of 16", same.load()));
    // the device's acceptance of the modules
    u32 accepted = 0, refused = 0;
    std::string detail;
    const bool device = Gpu::Vk::CheckShaderModules({res.spirv, vres.spirv}, accepted, refused, detail);
    if (!device) {
        g.Info("Shader(xlate): no Vulkan device for vkCreateShaderModule (" + detail + ")");
    } else {
        g.Check("vkCreateShaderModule on this device: both modules accepted", accepted == 2 && refused == 0, detail);
    }
}

// ---- Shader(cache)

/// A test draw: PS and VS programs in the test memory, registers pointing to them.
struct DrawSetup {
    TestMemory mem;
    Cp::RegisterFile regs;
    Cp::DrawDescriptor d;
    DrawSetup() {
        const auto ps = Program({kVMovV0One, kExpMrt0, 0}, 0x6161);
        const auto vs = Program({kVMovV0One, kExpPos0, 0}, 0x6262);
        mem.Put(0x10001000, ps);
        mem.Put(0x10002000, vs);
        regs.WriteReg(GS::Reg::CbColorControl, 1u << 4);
        regs.WriteReg(GS::Reg::CbTargetMask, 0xF);
        regs.WriteReg(GS::Reg::CbColor0Base, 0x1000);
        regs.WriteReg(GS::Reg::CbColor0Base + GS::Reg::CbColorInfoOffset, (10u << 2) | (4u << 8));
        regs.WriteReg(GS::Reg::SpiShaderColFormat, 9);
        regs.WriteReg(GS::Reg::VgtPrimitiveType, 4);
        d.shaders[0] = {0x10001000, 0, 0};
        d.shaders[1] = {0x10002000, 0, 0};
        d.active_stages = 3;
        d.changed = ~0ull;
    }
};

u64 ConstantKeyHash(const void*, size_t) {
    return 7;
}

void TestCache(TestGroup& g) {
    const GS::DeviceProfile profile = TestProfile();
    DrawSetup s;
    GS::ShaderFrontend fe({.async = false, .profile = &profile});
    const auto& p1 = fe.PrepareDraw(s.d, s.regs, s.mem);
    const bool first_ready = p1.ready;
    const u32 pipe1 = p1.pipeline;
    g.Check("a first draw: both programs read, translated (synchronously here), a pipeline",
            first_ready && pipe1 != 0 && fe.Programs().Programs() == 2 && fe.Stats().layouts == 2,
            fmt::format("ps {}, vs {}", GS::StageStatusName(fe.Stage(GS::HwStage::Ps).status),
                        GS::StageStatusName(fe.Stage(GS::HwStage::Vs).status)));
    const u64 translations_after_first = fe.Stats().queued;
    s.d.changed = 0;
    const auto& p2 = fe.PrepareDraw(s.d, s.regs, s.mem);
    g.Check("the same draw again with nothing changed: no translation, the module by key, the pipeline kept without a "
            "lookup (dirty tracking)",
            p2.ready && p2.pipeline == pipe1 && p2.pipeline_reused && fe.Stats().queued == translations_after_first &&
                fe.Stats().module_hits >= 2 && fe.Stats().program_reuse >= 2,
            fmt::format("queued {}, hits {}", fe.Stats().queued, fe.Stats().module_hits));
    // a dynamic state register: no new pipeline
    s.regs.WriteReg(0xA080 + 0, 123); // PA_SC_WINDOW_OFFSET-ish, the Scissor group
    s.d.changed = Cp::Bit(Cp::Group::Scissor) | Cp::Bit(Cp::Group::Viewport);
    const auto& p3 = fe.PrepareDraw(s.d, s.regs, s.mem);
    g.Check("viewport/scissor changed: the same pipeline (dynamic state)", p3.ready && p3.pipeline == pipe1, "");
    // blending changed: another pipeline, the same modules
    s.regs.WriteReg(GS::Reg::CbBlend0Control, (1u << 30) | 4u | (5u << 8));
    s.d.changed = Cp::Bit(Cp::Group::Blend);
    const u32 modules_before = fe.Modules();
    const auto& p4 = fe.PrepareDraw(s.d, s.regs, s.mem);
    g.Check("blending on: another pipeline, no new module", p4.ready && p4.pipeline != pipe1 && fe.Modules() == modules_before,
            fmt::format("modules {} -> {}", modules_before, fe.Modules()));
    // the colour target's format changed: a new fragment specialization (its runtime parameters)
    s.regs.WriteReg(GS::Reg::CbColor0Base + GS::Reg::CbColorInfoOffset, (12u << 2) | (7u << 8));
    s.d.changed = Cp::Bit(Cp::Group::ColorFormat);
    const auto& p5 = fe.PrepareDraw(s.d, s.regs, s.mem);
    g.Check("the colour format changed: a new fragment module (a new layout for new runtime parameters)",
            p5.ready && fe.Modules() > modules_before, fmt::format("modules {}", fe.Modules()));
    // the program rewritten in place: seen at the next frame
    const auto ps2 = Program({kVMovV0One, kExpMrt0, 0x01010101}, 0x6161);
    s.mem.Put(0x10001000, ps2);
    s.d.changed = 0;
    fe.PrepareDraw(s.d, s.regs, s.mem);
    const size_t programs_before = fe.Programs().Programs();
    fe.NewFrame();
    fe.PrepareDraw(s.d, s.regs, s.mem);
    g.Check("the fragment program rewritten in place: a new program at the next frame",
            fe.Programs().Programs() == programs_before + 1, "");
    // no program at the address: the stage reports it, the draw waits; nothing guessed
    s.d.shaders[0].address = 0x10090000;
    const auto& p6 = fe.PrepareDraw(s.d, s.regs, s.mem);
    g.Check("no program at PGM_LO/HI: the draw is not ready (no program), nothing else used",
            !p6.ready && fe.Stage(GS::HwStage::Ps).status == GS::StageStatus::NoProgram, "");
    s.d.shaders[0].address = 0x10001000;
    // GS enabled: unsupported, named
    s.regs.WriteReg(GS::Reg::VgtShaderStagesEn, 0xB0);
    const auto& p7 = fe.PrepareDraw(s.d, s.regs, s.mem);
    g.Check("a GS configuration: unsupported, reported (not prepared)", !p7.ready && p7.unsupported, "");
    s.regs.WriteReg(GS::Reg::VgtShaderStagesEn, 0);

    // module keys: a hash collision never merges two specializations
    GS::Program prog;
    auto m1 = std::make_unique<GS::Module>();
    m1->keyed = true;
    m1->key.words = {1, 2, 3};
    m1->key.hash = 99;
    prog.modules.push_back(std::move(m1));
    GS::SpecializationKey other{.words = {1, 2, 4}, .hash = 99};
    u64 collisions = 0;
    g.Check("two keys with one hash and other words: not found (the collision counted)",
            prog.FindModule(other, collisions) == nullptr && collisions == 1, "");
    // pipelines: forced hash collisions keep the states apart
    GS::PipelineStateCache pc;
    pc.SetTestHash(&ConstantKeyHash);
    GS::GraphicsPipelineState a{}, b{};
    a.vs_module = 1;
    b.vs_module = 2;
    bool created = false;
    const u32 ia = pc.GetGraphics(a, created);
    const u32 ib = pc.GetGraphics(b, created);
    const u32 ia2 = pc.GetGraphics(a, created);
    g.Check("pipelines with one forced hash: two pipelines, the collision counted, the first found again",
            ia != ib && ia2 == ia && pc.Stats().collisions >= 1 && pc.Size() == 2, "");
    const auto groups = GS::PipelineStateGroups();
    g.Check("the pipeline state's register groups: colour formats, blending, depth formats; not viewport/scissor",
            (groups & Cp::Bit(Cp::Group::ColorFormat)) && (groups & Cp::Bit(Cp::Group::Blend)) &&
                (groups & Cp::Bit(Cp::Group::DepthFormat)) &&
                !(groups & (Cp::Bit(Cp::Group::Viewport) | Cp::Bit(Cp::Group::Scissor))),
            "");
    // P4.7 stage memo: the same inputs take the last preparation again; any reported change prepares anew
    {
        DrawSetup sm;
        GS::ShaderFrontend mfe({.async = false, .profile = &profile, .stage_memo = true});
        mfe.PrepareDraw(sm.d, sm.regs, sm.mem);
        sm.d.changed = 0;
        const u64 h0 = mfe.Stats().memo_hits;
        const auto& m1 = mfe.PrepareDraw(sm.d, sm.regs, sm.mem);
        const u64 h1 = mfe.Stats().memo_hits;
        g.Check("stage memo: the same draw again - both stages taken again, the same pipeline",
                m1.ready && h1 == h0 + 2 && m1.pipeline_reused, fmt::format("hits {} -> {}", h0, h1));
        sm.d.user_data[1][5] ^= 0x10; // a VS user data register
        mfe.PrepareDraw(sm.d, sm.regs, sm.mem);
        g.Check("stage memo: other VS user data - the VS prepared anew, the PS taken again",
                mfe.Stats().memo_hits == h1 + 1 && mfe.Stats().memo_miss_user_data == 1, "");
        const u64 h2 = mfe.Stats().memo_hits;
        mfe.GuestMemoryChanged();
        const auto& m2 = mfe.PrepareDraw(sm.d, sm.regs, sm.mem);
        g.Check("stage memo: guest memory changed - both stages prepared anew (still ready)",
                m2.ready && mfe.Stats().memo_hits == h2 && mfe.Stats().memo_miss_memory == 2, "");
        mfe.PrepareDraw(sm.d, sm.regs, sm.mem);
        const u64 h3 = mfe.Stats().memo_hits;
        mfe.NewFrame();
        mfe.PrepareDraw(sm.d, sm.regs, sm.mem);
        const u64 h4 = mfe.Stats().memo_hits;
        mfe.Invalidate(0x10001000, 4);
        mfe.PrepareDraw(sm.d, sm.regs, sm.mem);
        g.Check("stage memo: a new frame and a written program - prepared anew each time",
                h3 == h2 + 2 && h4 == h3 && mfe.Stats().memo_hits == h4, fmt::format("hits {} {} {}", h3, h4,
                                                                                     mfe.Stats().memo_hits));
        sm.regs.WriteReg(GS::Reg::CbColor0Base + GS::Reg::CbColorInfoOffset, (12u << 2) | (7u << 8));
        sm.d.changed = Cp::Bit(Cp::Group::ColorFormat);
        const u64 o0 = mfe.Stats().memo_miss_other;
        mfe.PrepareDraw(sm.d, sm.regs, sm.mem);
        g.Check("stage memo: runtime registers changed - prepared anew", mfe.Stats().memo_miss_other > o0, "");
        GS::ShaderFrontend nfe({.async = false, .profile = &profile});
        DrawSetup sn;
        nfe.PrepareDraw(sn.d, sn.regs, sn.mem);
        sn.d.changed = 0;
        nfe.PrepareDraw(sn.d, sn.regs, sn.mem);
        g.Check("stage memo: off unless the owner asks for it (it must report memory changes)",
                nfe.Stats().memo_hits == 0, "");
    }
    // P5.3 Native Render Frontend: stages of an engine-described draw keyed by their inputs and the snapshot's bytes
    for (const GS::NativeStages mode : {GS::NativeStages::On, GS::NativeStages::Verify}) {
        const bool verify = mode == GS::NativeStages::Verify;
        const std::string tag = verify ? "native frontend (verify)" : "native frontend";
        DrawSetup sx;
        GS::ShaderFrontend xfe({.async = false,
                                .profile = &profile,
                                .native_stages = mode,
                                .native_passes = 1u << 1,
                                .native_admit = 1}); // (cached at once; the admission is checked below)
        // the engine's tables: the VS snapshot through s[2:3] (its start) and s[6:7] (4 dwords in), the PS through s[4:5]
        sx.d.engine_pass = 1;
        sx.d.engine_vs = {.dwords = 8, .refs = 2, .sgpr = {2, 6}, .offset = {0, 4}};
        sx.d.engine_ps = {.dwords = 4, .refs = 1, .sgpr = {4}, .offset = {0}};
        const std::array<u32, 8> vs_snap{1, 2, 3, 4, 5, 6, 7, 8};
        const std::array<u32, 4> ps_snap{9, 10, 11, 12};
        const auto place = [&](u64 vs_at, u64 ps_at) {
            sx.mem.Put(vs_at, vs_snap);
            sx.mem.Put(ps_at, ps_snap);
            auto& vu = sx.d.user_data[static_cast<size_t>(Cp::Stage::Vs)];
            auto& pu = sx.d.user_data[static_cast<size_t>(Cp::Stage::Ps)];
            vu[2] = static_cast<u32>(vs_at);
            vu[3] = static_cast<u32>(vs_at >> 32);
            vu[6] = static_cast<u32>(vs_at + 16);
            vu[7] = static_cast<u32>((vs_at + 16) >> 32);
            vu[9] = 0x77; // a plain user data value (no preparation reads it: not keyed)
            pu[4] = static_cast<u32>(ps_at);
            pu[5] = static_cast<u32>(ps_at >> 32);
        };
        place(0x10010000, 0x10010100);
        const auto& x1 = xfe.PrepareDraw(sx.d, sx.regs, sx.mem);
        const u32 xpipe = x1.pipeline;
        g.Check(tag + ": the first engine draw - both stages prepared in full and cached",
                x1.ready && xfe.Stats().native_lookups == 2 && xfe.Stats().native_learned == 2 &&
                    xfe.Stats().native_hits == 0,
                fmt::format("lookups {}, learned {}, not cacheable {}", xfe.Stats().native_lookups,
                            xfe.Stats().native_learned, xfe.Stats().native_uncacheable));
        sx.d.changed = 0;
        place(0x10020000, 0x10020200); // the same bytes in a new snapshot (the next frame's allocation)
        const auto& x2 = xfe.PrepareDraw(sx.d, sx.regs, sx.mem);
        const GS::StageView& xv = xfe.Stage(GS::HwStage::Vs);
        if (!verify) {
            g.Check(tag + ": the same bytes elsewhere - both stages from the cache, shared (not copied), the pipeline kept",
                    x2.ready && x2.pipeline == xpipe && xfe.Stats().native_hits == 2 && xv.native_shared &&
                        xfe.SharedStage(GS::HwStage::Vs) != nullptr && xfe.SharedStage(GS::HwStage::Ps) != nullptr &&
                        xv.plan.flat.size() >= 16 && xv.plan.flat[2] == 0x10010000u,
                    fmt::format("hits {}, shared {}", xfe.Stats().native_hits, xv.native_shared));
        } else {
            g.Check(tag + ": the same bytes elsewhere - prepared in full anyway and equal to the cached ones",
                    x2.ready && x2.pipeline == xpipe && xfe.Stats().native_verified == 2 &&
                        xfe.Stats().native_mismatches == 0 && xfe.SharedStage(GS::HwStage::Vs) == nullptr,
                    fmt::format("verified {}, mismatches {}", xfe.Stats().native_verified,
                                xfe.Stats().native_mismatches));
        }
        // a register neither preparation read (no SRT walk, no descriptor in the registers): not keyed - the executor
        // writes the draw's own registers into the flattened data
        const u64 taken = verify ? xfe.Stats().native_verified : xfe.Stats().native_hits;
        sx.d.user_data[static_cast<size_t>(Cp::Stage::Vs)][9] = 0x78;
        sx.d.changed = 0;
        xfe.PrepareDraw(sx.d, sx.regs, sx.mem);
        g.Check(tag + ": a register no preparation read differs - both stages still from the cache",
                (verify ? xfe.Stats().native_verified : xfe.Stats().native_hits) == taken + 2 &&
                    xfe.Stats().native_mismatches == 0,
                fmt::format("taken {} -> {}", taken, verify ? xfe.Stats().native_verified : xfe.Stats().native_hits));
        sx.d.user_data[static_cast<size_t>(Cp::Stage::Vs)][9] = 0x77;
        const u64 learned = xfe.Stats().native_learned;
        sx.mem.Put(0x10020000 + 12, std::array<u32, 1>{99}); // one dword of the VS snapshot differs
        xfe.PrepareDraw(sx.d, sx.regs, sx.mem);
        g.Check(tag + ": one snapshot dword differs - the VS prepared anew and cached, the PS from the cache",
                xfe.Stats().native_learned == learned + 1 && xfe.SharedStage(GS::HwStage::Vs) != nullptr, "");
        const u64 lookups = xfe.Stats().native_lookups;
        auto& vu = sx.d.user_data[static_cast<size_t>(Cp::Stage::Vs)];
        vu[6] += 4; // the second pointer no longer agrees with the first on the snapshot's start
        xfe.PrepareDraw(sx.d, sx.regs, sx.mem);
        g.Check(tag + ": the snapshot pointers disagree - not looked up (prepared as before), counted",
                xfe.Stats().native_unkeyed == 1 && xfe.Stats().native_lookups == lookups + 1, "");
        vu[6] -= 4;
        sx.d.engine_pass = Cp::kNoEnginePass;
        const u64 before = xfe.Stats().native_lookups;
        const auto& x3 = xfe.PrepareDraw(sx.d, sx.regs, sx.mem);
        g.Check(tag + ": a draw the engine did not describe - no lookup, its own preparation",
                x3.ready && xfe.Stats().native_lookups == before && xfe.SharedStage(GS::HwStage::Vs) == nullptr, "");
        sx.d.engine_pass = 2; // a pass the frontend does not take
        xfe.PrepareDraw(sx.d, sx.regs, sx.mem);
        g.Check(tag + ": a pass outside native_passes - no lookup", xfe.Stats().native_lookups == before, "");
    }
    // ... cached at the second sighting of its inputs (native_admit, the game's default); verify names why a lookup
    // missed
    {
        DrawSetup sa;
        GS::ShaderFrontend afe({.async = false,
                                .profile = &profile,
                                .native_stages = GS::NativeStages::Verify,
                                .native_passes = 1u << 1});
        sa.d.engine_pass = 1;
        sa.d.engine_vs = {.dwords = 8, .refs = 2, .sgpr = {2, 6}, .offset = {0, 4}};
        sa.d.engine_ps = {.dwords = 4, .refs = 1, .sgpr = {4}, .offset = {0}};
        constexpr u64 vs_at = 0x10030000, ps_at = 0x10030100;
        sa.mem.Put(vs_at, std::array<u32, 8>{1, 2, 3, 4, 5, 6, 7, 8});
        sa.mem.Put(ps_at, std::array<u32, 4>{9, 10, 11, 12});
        auto& vu = sa.d.user_data[static_cast<size_t>(Cp::Stage::Vs)];
        auto& pu = sa.d.user_data[static_cast<size_t>(Cp::Stage::Ps)];
        vu[2] = static_cast<u32>(vs_at);
        vu[3] = static_cast<u32>(vs_at >> 32);
        vu[6] = static_cast<u32>(vs_at + 16);
        vu[7] = static_cast<u32>((vs_at + 16) >> 32);
        vu[9] = 0x77;
        pu[4] = static_cast<u32>(ps_at);
        pu[5] = static_cast<u32>(ps_at >> 32);
        const GS::FrontendStats& as = afe.Stats();
        const auto& a1 = afe.PrepareDraw(sa.d, sa.regs, sa.mem);
        g.Check("native frontend (admission): a first sighting - prepared in full, not cached; verify: inputs never seen",
                a1.ready && as.native_first_seen == 2 && as.native_learned == 0 && as.native_miss_new == 2,
                fmt::format("first sightings {}, learned {}, new {}", as.native_first_seen, as.native_learned,
                            as.native_miss_new));
        sa.d.changed = 0;
        afe.PrepareDraw(sa.d, sa.regs, sa.mem);
        g.Check("native frontend (admission): the second sighting - cached; verify: the same inputs and bytes",
                as.native_learned == 2 && as.native_miss_same == 2 && afe.SharedStage(GS::HwStage::Vs) != nullptr,
                fmt::format("learned {}, same {}", as.native_learned, as.native_miss_same));
        sa.d.changed = 0;
        afe.PrepareDraw(sa.d, sa.regs, sa.mem);
        g.Check("native frontend (admission): the third - a hit (verify: compared, equal)",
                as.native_verified == 2 && as.native_mismatches == 0, "");
        sa.mem.Put(vs_at, std::array<u32, 1>{0x55}); // the first dword of the VS snapshot differs (a descriptor's base)
        sa.d.changed = 0;
        afe.PrepareDraw(sa.d, sa.regs, sa.mem);
        g.Check("native frontend (verify): a miss by other bytes - one dword, the first of its group",
                as.native_miss_bytes == 1 && as.native_miss_dwords[0] == 1 && as.native_miss_word0 == 1 &&
                    as.native_first_seen == 3,
                fmt::format("other bytes {}, 1 dword {}, word0 {}, first sightings {}", as.native_miss_bytes,
                            as.native_miss_dwords[0], as.native_miss_word0, as.native_first_seen));
    }
    // the asynchronous worker: queued translations finish; the draw becomes ready
    DrawSetup s2;
    GS::ShaderFrontend async_fe({.async = true, .profile = &profile});
    async_fe.PrepareDraw(s2.d, s2.regs, s2.mem);
    async_fe.Drain();
    s2.d.changed = 0;
    async_fe.PrepareDraw(s2.d, s2.regs, s2.mem); // the layouts are ready now: the modules by key may queue
    async_fe.Drain();
    const auto& pa = async_fe.PrepareDraw(s2.d, s2.regs, s2.mem);
    async_fe.NewFrame();
    g.Check("the worker thread: translations queued, drained, the draw ready; translations counted",
            pa.ready && async_fe.Stats().translations >= 2,
            fmt::format("translations {}", async_fe.Stats().translations));
}

// ---- Shader(capture)

void TestCapture(TestGroup& g, const App::Config& cfg) {
    const auto dir = cfg.logs_dir / "selftest_shader";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / "roundtrip.sspcshd";
    GS::CaptureWriter w;
    GS::CapturedModule m;
    m.hw = GS::HwStage::Ps;
    m.sw = GS::SwStage::Fragment;
    m.perm_idx = 3;
    m.footer_hash = 0x1234;
    m.address = 0x10001000;
    m.start = {2, 1};
    m.code = {1, 2, 3, 4};
    m.flat = std::vector<u32>(20, 7);
    m.has_fetch = true;
    m.fetch_code = {kSetpc};
    m.vertex_sharps = {{1, 2, 3, 4}};
    m.sh.assign(0x400, 5);
    m.context.assign(0x400, 6);
    m.spirv = {0x07230203, 1, 2};
    m.profile = TestProfile();
    m.bridge_runtime = {9, 8, 7};
    const bool opened = w.Open(path, "self-test");
    w.Write(m);
    w.Close();
    std::vector<GS::CapturedModule> got;
    std::string info, err;
    const bool loaded = GS::LoadCapture(path, got, info, err);
    g.Check("the capture format: a module written and read back field for field",
            opened && loaded && got.size() == 1 && got[0].perm_idx == 3 && got[0].footer_hash == 0x1234 &&
                got[0].code == m.code && got[0].flat == m.flat && got[0].fetch_code == m.fetch_code &&
                got[0].vertex_sharps == m.vertex_sharps && got[0].sh == m.sh && got[0].spirv == m.spirv &&
                got[0].bridge_runtime == m.bridge_runtime && GS::DiffProfiles(got[0].profile, m.profile).empty() &&
                info == "self-test",
            err);
    std::ofstream(dir / "bad.sspcshd", std::ios::binary) << "not a capture";
    got.clear();
    g.Check("a file that is not a capture: refused with the reason",
            !GS::LoadCapture(dir / "bad.sspcshd", got, info, err) && err.find("magic") != std::string::npos, err);
    GS::DeviceProfile p2 = m.profile;
    p2.subgroup_size = 64;
    g.Check("profile differences named", GS::DiffProfiles(m.profile, p2).find("subgroup_size") != std::string::npos,
            "");
    std::filesystem::remove_all(dir, ec); // the test's files: cleaned up
    g.Check("the test's files removed", !std::filesystem::exists(dir, ec), "");
}

// ---- Shader(disk) (P4.14: the persistent shader cache)

void TestDisk(TestGroup& g, const App::Config& cfg) {
    const auto dir = cfg.logs_dir / "selftest_shader_disk";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    const GS::DeviceProfile profile = TestProfile();
    const std::string gen = GS::ShaderCacheGeneration(profile);
    const auto cache_dir = dir / gen;
    GS::DiskCacheOptions opt{.dir = cache_dir, .generation = gen};

    // a first run: the frontend translates and the cache keeps what it made
    std::vector<GS::SpirvHash> made;
    std::vector<std::vector<u32>> made_spirv, made_flat;
    std::vector<size_t> made_buffers;
    {
        GS::ShaderDiskCache disk;
        std::string err;
        const bool opened = disk.Open(opt, err);
        DrawSetup s;
        GS::ShaderFrontend fe({.async = false, .profile = &profile});
        fe.SetDiskCache(&disk);
        const auto& p = fe.PrepareDraw(s.d, s.regs, s.mem);
        for (const auto* prog : fe.Programs().All()) {
            for (const auto& m : prog->modules) {
                made.push_back(m->spirv_hash);
                made_spirv.push_back(m->result.spirv);
                made_flat.push_back(m->result.flatbuf);
                made_buffers.push_back(m->result.reflection.buffers.size());
            }
        }
        disk.Flush();
        const auto st = disk.Stats();
        g.Check("a first run: a fresh cache, the draw translated, its programs, modules and SPIR-V written",
                opened && p.ready && st.recreated && st.written_modules >= 2 && st.written_programs >= 2 &&
                    st.written_spirv >= 1 && st.write_errors == 0,
                fmt::format("{} - modules {}, programs {}, SPIR-V {}", err, st.written_modules, st.written_programs,
                            st.written_spirv));
        GS::PipelineDesc d;
        d.has_ps = true;
        d.vs = made.empty() ? GS::SpirvHash{} : made.front();
        d.state.prim_type = 4;
        d.bindings = {(u64{3} << 32) | (7u << 16) | 1};
        disk.AddPipeline(d, d.Hash(), 1234);
        disk.AddPipeline(d, d.Hash(), 1234); // (a second time: the manifest keeps it once)
        disk.Flush();
        g.Check("a pipeline added twice: one manifest record", disk.Stats().written_pipelines == 1, "");
    }
    // the next run: the same draw reads its modules instead of translating them
    {
        GS::ShaderDiskCache disk;
        std::string err;
        const bool opened = disk.Open(opt, err);
        const auto st = disk.Stats();
        g.Check("the next run: the cache read back whole (no record cut, nothing recreated)",
                opened && !st.recreated && st.torn_bytes == 0 && st.modules == made.size() && st.pipelines == 1,
                fmt::format("{} - modules {} of {}, pipelines {}", err, st.modules, made.size(), st.pipelines));
        DrawSetup s;
        GS::ShaderFrontend fe({.async = false, .profile = &profile});
        fe.SetDiskCache(&disk);
        const auto& p = fe.PrepareDraw(s.d, s.regs, s.mem);
        bool same = true;
        size_t i = 0;
        for (const auto* prog : fe.Programs().All()) {
            for (const auto& m : prog->modules) {
                same = same && i < made.size() && m->from_disk && m->spirv_hash == made[i] &&
                       m->result.spirv == made_spirv[i] && m->result.flatbuf == made_flat[i] &&
                       m->result.reflection.buffers.size() == made_buffers[i];
                ++i;
            }
        }
        g.Check("the next run: the draw ready with no translation - every module read from the cache, the same SPIR-V, "
                "flattened user data and resources",
                p.ready && fe.Translations().count == 0 && fe.Stats().disk_loads == made.size() && same &&
                    i == made.size(),
                fmt::format("translations {}, read {}", fe.Translations().count, fe.Stats().disk_loads));
        const auto descs = disk.Pipelines();
        GS::PipelineDesc back;
        size_t pos = 0;
        std::vector<u8> bytes;
        if (!descs.empty()) {
            descs[0].Serialize(bytes);
        }
        g.Check("the pipeline manifest: the description read back equal, its hash the same",
                descs.size() == 1 && back.Deserialize(bytes, pos) && back == descs[0] && back.Hash() == descs[0].Hash() &&
                    back.bindings.size() == 1 && back.state.prim_type == 4,
                "");
        GS::ShaderDiskCache second;
        const bool second_ok = second.Open(opt, err);
        g.Check("a second process on the same directory: it reads, never writes", second_ok && !second.Writable(), err);
    }
    // a write cut by the process's end: the torn record is cut off at the next open, everything before it stays
    const auto pack = cache_dir / "shaders.pack";
    {
        GS::ShaderDiskCache disk;
        std::string err;
        disk.Open(opt, err);
        GS::PipelineDesc d;
        d.compute = true;
        d.vs.lo = 77;
        disk.TestTearNextWrite(40);
        disk.AddPipeline(d, d.Hash(), 5);
        disk.Flush();
    }
    {
        GS::ShaderDiskCache disk;
        std::string err;
        const bool opened = disk.Open(opt, err);
        const auto st = disk.Stats();
        g.Check("an interrupted write: the torn record cut off (40 bytes), the earlier records all kept",
                opened && st.torn_bytes == 40 && st.modules == made.size() && st.pipelines == 1 && !st.recreated,
                fmt::format("torn {}, modules {}, pipelines {}", st.torn_bytes, st.modules, st.pipelines));
    }
    // a damaged record at the end: it is cut, what precedes stays; the size limit
    {
        const auto size = std::filesystem::file_size(pack, ec);
        std::fstream f(pack, std::ios::in | std::ios::out | std::ios::binary);
        f.seekg(static_cast<std::streamoff>(size - 20));
        char c = 0;
        f.read(&c, 1);
        c = static_cast<char>(c ^ 0x5A);
        f.seekp(static_cast<std::streamoff>(size - 20));
        f.write(&c, 1);
    }
    {
        GS::ShaderDiskCache disk;
        std::string err;
        auto small = opt;
        small.max_bytes = 1024; // (smaller than the file already: nothing more is added)
        const bool opened = disk.Open(small, err);
        const auto st = disk.Stats();
        GS::PipelineDesc d;
        d.vs.lo = 99;
        disk.AddPipeline(d, d.Hash(), 1);
        disk.Flush();
        g.Check("a damaged byte in the last record: its checksum fails, it is cut, the rest kept; over the size limit "
                "nothing is added (counted)",
                opened && st.torn_bytes > 0 && st.records > 0 && !st.recreated && disk.Stats().dropped_full == 1,
                fmt::format("torn {}, records {}, dropped {}", st.torn_bytes, st.records, disk.Stats().dropped_full));
    }
    // another generation in the same file (a format change): started anew, the old file kept once
    {
        GS::ShaderDiskCache disk;
        std::string err;
        auto other = opt;
        other.generation = "0123456789abcdef";
        const bool opened = disk.Open(other, err);
        const auto st = disk.Stats();
        g.Check("a file of another generation: not read - started anew, the old one kept as shaders.pack.bad",
                opened && st.recreated && st.recreate_reason == "another generation" && st.records == 0 &&
                    std::filesystem::exists(cache_dir / "shaders.pack.bad", ec),
                st.recreate_reason);
    }
    // garbage instead of a cache file
    {
        std::ofstream(pack, std::ios::binary | std::ios::trunc) << std::string(200, 'x');
        GS::ShaderDiskCache disk;
        std::string err;
        const bool opened = disk.Open(opt, err);
        g.Check("a file that is not a cache: started anew (never parsed)",
                opened && disk.Stats().recreated && disk.Stats().records == 0, disk.Stats().recreate_reason);
    }
    std::filesystem::remove_all(dir, ec);
    g.Check("the test's files removed", !std::filesystem::exists(dir, ec), "");
}

// P4.15A R4: overlap writer startup with setters, then require a published callback
// to execute. This stress test complements (does not replace) the mutex happens-before proof.
void TestPeriodic(TestGroup& g, const App::Config& cfg) {
    const auto dir = cfg.logs_dir / "selftest_shader_periodic";
    bool ok = true;
    u64 callbacks = 0;
    for (u32 round = 0; round < 32; ++round) {
        GS::ShaderDiskCache disk;
        std::string error;
        const GS::DiskCacheOptions opt{.dir = dir / std::to_string(round), .generation = "periodic-test"};
        if (!disk.Open(opt, error)) {
            ok = false;
            break;
        }
        std::atomic<u32> calls{0};
        const auto fn = +[](void* p) { static_cast<std::atomic<u32>*>(p)->fetch_add(1); };
        std::thread a([&] {
            for (u32 i = 0; i < 10000; ++i) {
                disk.SetPeriodic(fn, &calls, 100 + i % 3);
            }
        });
        std::thread b([&] {
            for (u32 i = 0; i < 10000; ++i) {
                disk.SetPeriodic(nullptr, nullptr, i % 2);
            }
        });
        a.join();
        b.join();
        disk.SetPeriodic(fn, &calls, 100);
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (calls.load() == 0 && std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        disk.Close(); // joins the callback before calls goes out of scope
        ok &= calls.load() != 0 && disk.Stats().write_errors == 0;
        callbacks += calls.load();
    }
    g.Check("periodic writer startup/setter stress: 32 opens, 640000 updates, callbacks joined before context dies",
            ok, fmt::format("callbacks {}", callbacks));
}

} // namespace

std::vector<TestGroup> TestShaderFrontend(const App::Config& cfg) {
    std::vector<TestGroup> out(10);
    out[0].name = "Shader(identity)";
    out[1].name = "Shader(validate)";
    out[2].name = "Shader(runtime)";
    out[3].name = "Shader(fetch)";
    out[4].name = "Shader(descr)";
    out[5].name = "Shader(srt)";
    out[6].name = "Shader(xlate)";
    out[7].name = "Shader(cache)";
    out[8].name = "Shader(capture)";
    out[9].name = "Shader(disk)";
    TestIdentity(out[0]);
    TestValidation(out[1]);
    TestRuntime(out[2]);
    TestFetch(out[3]);
    TestDescriptors(out[4]);
    TestSrt(out[5]);
    TestTranslate(out[6]);
    TestCache(out[7]);
    TestCapture(out[8], cfg);
    TestDisk(out[9], cfg);
    TestPeriodic(out[9], cfg);
    return out;
}

} // namespace SecondSon::Bridge
