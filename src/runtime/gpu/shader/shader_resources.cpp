// SecondSonPC — the resources a GCN program reads (P4.3). See shader_resources.h.
#include "runtime/gpu/shader/shader_resources.h"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>
#include <xxhash.h>
#include <Zydis/Zydis.h>

namespace SecondSon::Gpu::Shader {

namespace {

constexpr u64 kGpuSpace = 1ull << 40;

bool InGpuSpace(u64 addr, u64 bytes) {
    return addr < kGpuSpace && bytes <= kGpuSpace - addr;
}

// GCN encodings (bits that identify them), as the bridge's GetInstructionEncoding tests them.
enum class Enc : u8 { Sop1, Sopp, Sopc, Vop1, Vopc, Vop2, Smrd, Mubuf, Mtbuf, Other };
Enc Encoding(u32 w) {
    switch (w >> 23) {
    case 0x17D:
        return Enc::Sop1;
    case 0x17F:
        return Enc::Sopp;
    case 0x17E:
        return Enc::Sopc;
    default:
        break;
    }
    switch (w >> 25) {
    case 0x3F:
        return Enc::Vop1;
    case 0x3E:
        return Enc::Vopc;
    default:
        break;
    }
    switch (w >> 26) {
    case 0x38:
        return Enc::Mubuf;
    case 0x3A:
        return Enc::Mtbuf;
    case 0x34: case 0x33: case 0x3E: case 0x32: case 0x36: case 0x3C: // VOP3, VOP3P, EXP, VINTRP, DS, MIMG
        return Enc::Other;
    default:
        break;
    }
    if ((w >> 27) == 0x18) {
        return Enc::Smrd;
    }
    if ((w >> 28) == 0xB || (w >> 30) == 0x2) { // SOPK, SOP2
        return Enc::Other;
    }
    if ((w >> 31) == 0) {
        return Enc::Vop2;
    }
    return Enc::Other;
}

constexpr u32 kOpSetpcB64 = 32;      // SOP1
constexpr u32 kOpWaitcnt = 12;       // SOPP
constexpr u32 kOpLoadDwordx4 = 2;    // SMRD
constexpr u32 kOpVAddI32 = 37;       // VOP2
constexpr u32 kOpVMovB32 = 1;        // VOP1
constexpr u32 kInlineOneF32 = 242;   // the inline constant 1.0

} // namespace

// ---- the fetch shader

bool ParseFetchShaderCode(std::span<const u32> code, u32 sgpr_base, FetchShader& out, std::string& error) {
    out = {};
    out.present = true;
    out.sgpr_base = sgpr_base;
    struct Load {
        u32 dword_offset = 0;
        u32 base_sgpr = 0;
        bool set = false;
    };
    std::array<Load, 128> loads{};
    size_t i = 0;
    while (true) {
        if (i >= code.size()) {
            error = fmt::format("no s_setpc_b64 in the {} dwords read", code.size());
            return false;
        }
        const u32 w0 = code[i++];
        switch (Encoding(w0)) {
        case Enc::Sop1:
            if (((w0 >> 8) & 0xFF) != kOpSetpcB64) {
                error = fmt::format("SOP1 {:#x} at dword {} is not s_setpc_b64", w0, i - 1);
                return false;
            }
            out.size = static_cast<u32>(i * 4);
            return true;
        case Enc::Smrd: {
            const u32 op = (w0 >> 22) & 0x1F;
            if (op != kOpLoadDwordx4) {
                error = fmt::format("SMRD {:#x} at dword {} is not s_load_dwordx4", w0, i - 1);
                return false;
            }
            const u32 sdst = (w0 >> 15) & 0x7F;
            loads[sdst] = {w0 & 0xFF, ((w0 >> 9) & 0x3F) * 2, true};
            break;
        }
        case Enc::Vop2: {
            const u32 op = (w0 >> 25) & 0x3F;
            const u32 src0 = w0 & 0x1FF;
            const u32 vsrc1 = (w0 >> 9) & 0xFF;
            const u32 vdst = (w0 >> 17) & 0xFF;
            if (op != kOpVAddI32 || src0 >= 16 || vsrc1 != vdst || (vdst != 0 && vdst != 3)) {
                error = fmt::format("VOP2 {:#x} at dword {} is not v_add_i32 v0/v3, s(user data), v0/v3", w0, i - 1);
                return false;
            }
            (vdst == 0 ? out.vertex_offset_sgpr : out.instance_offset_sgpr) = static_cast<s32>(src0);
            break;
        }
        case Enc::Mubuf:
        case Enc::Mtbuf: {
            if (i >= code.size()) {
                error = "a buffer load cut by the end of what was read";
                return false;
            }
            const u32 w1 = code[i++];
            const bool mubuf = Encoding(w0) == Enc::Mubuf;
            const u32 op = mubuf ? (w0 >> 18) & 0x7F : (w0 >> 16) & 0x7;
            if (op > 3) {
                error = fmt::format("{} {:#x} at dword {} is not a *_load_format_x..xyzw", mubuf ? "MUBUF" : "MTBUF", w0,
                                    i - 2);
                return false;
            }
            const u32 srsrc = ((w1 >> 16) & 0x1F) * 4;
            if (!loads[srsrc].set) {
                error = fmt::format("the buffer load at dword {} uses s[{}:{}] no s_load_dwordx4 set", i - 2, srsrc,
                                    srsrc + 3);
                return false;
            }
            FetchAttribute a;
            a.dest_vgpr = (w1 >> 8) & 0xFF;
            a.num_elements = op + 1;
            a.sgpr_base = loads[srsrc].base_sgpr;
            a.dword_offset = loads[srsrc].dword_offset;
            a.inst_offset = w0 & 0xFFF;
            a.instance_data = w1 & 0xFF;
            if (!mubuf) {
                a.data_format = (w0 >> 19) & 0xF;
                a.num_format = (w0 >> 23) & 0x7;
            }
            out.attributes.push_back(a);
            break;
        }
        case Enc::Sopp:
            if (((w0 >> 16) & 0x7F) != kOpWaitcnt) {
                error = fmt::format("SOPP {:#x} at dword {} is not s_waitcnt", w0, i - 1);
                return false;
            }
            break;
        case Enc::Vop1:
            if (((w0 >> 9) & 0xFF) != kOpVMovB32 || (w0 & 0x1FF) != kInlineOneF32) {
                error = fmt::format("VOP1 {:#x} at dword {} is not v_mov_b32 vN, 1.0", w0, i - 1);
                return false;
            }
            break; // a component set to 1.0 (the bridge ignores it, with a warning)
        default:
            error = fmt::format("instruction {:#x} at dword {}: not of the fetch shader's form", w0, i - 1);
            return false;
        }
    }
}

// ---- the SRT walk

namespace {

// Micro-ops of the walker (flatten_extended_userdata_pass.cpp's generator; the comments give the x86 they come from).
enum class SrtOp : u8 {
    PushRdi,        // push rdi
    PopRdi,         // pop rdi
    PushR10,        // push r10
    PopR10,         // pop r10
    PopRcx,         // pop rcx
    Drop,           // add rsp, 8
    LoadRdiDisp,    // mov rdi, qword [rdi + disp]
    LoadRdiR10,     // mov rdi, qword [rdi + r10]
    LoadR10Disp,    // mov r10d, dword [rdi + disp]
    LoadR10R10,     // mov r10d, dword [rdi + r10]
    LoadR10Flat,    // mov r10d, dword [rsi + disp]
    StoreFlat,      // mov dword [rsi + disp], r10d
    MovR10Imm64,    // mov r10, imm64
    MovR10Imm32,    // mov r10d, imm32
    AndRdiR10,      // and rdi, r10
    ZextR10,        // mov r10d, r10d
    ShlR10Imm,      // shl r10d, imm8
    ShrR10Imm,      // shr r10d, imm8
    ShlR10Cl,       // shl r10d, cl
    ShrR10Cl,       // shr r10d, cl
    AddR10Imm,      // add r10d, imm
    SubR10Imm,      // sub r10d, imm
    AndR10Imm,      // and r10d, imm
    OrR10Imm,       // or r10d, imm
    XorR10Imm,      // xor r10d, imm
    ImulR10Imm,     // imul r10d, r10d, imm
    NegR10,         // neg r10d
    NotR10,         // not r10d
    AddR10Stack,    // add r10d, dword [rsp]
    AndR10Stack,    // and r10d, dword [rsp]
    OrR10Stack,     // or r10d, dword [rsp]
    XorR10Stack,    // xor r10d, dword [rsp]
    ImulR10Stack,   // imul r10d, dword [rsp]
    CmpR10Stack,    // cmp r10d, dword [rsp]
    CmovaR10Stack,  // cmova r10d, dword [rsp]
    CmovbR10Stack,  // cmovb r10d, dword [rsp]
    MovEcxR10,      // mov ecx, r10d
    MovEcxImm,      // mov ecx, imm32
    CmpR10Ecx,      // cmp r10d, ecx
    CmovaR10Ecx,    // cmova r10d, ecx
    CmovbR10Ecx,    // cmovb r10d, ecx
    ShrStackCl,     // shr dword [rsp], cl
    MovEdxImm,      // mov edx, imm32
    ShlEdxCl,       // shl edx, cl
    DecEdx,         // dec edx
    AndStackEdx,    // and dword [rsp], edx
    Ret,            // ret
};

ZydisDecoder& Decoder() {
    static ZydisDecoder decoder = [] {
        ZydisDecoder d{};
        ZydisDecoderInit(&d, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
        return d;
    }();
    return decoder;
}

bool IsReg(const ZydisDecodedOperand& o, ZydisRegister r) {
    return o.type == ZYDIS_OPERAND_TYPE_REGISTER && o.reg.value == r;
}
bool IsImm(const ZydisDecodedOperand& o) {
    return o.type == ZYDIS_OPERAND_TYPE_IMMEDIATE;
}
/// [base + disp] (no index), or [base + index] (no disp) when `index` is given.
bool IsMem(const ZydisDecodedOperand& o, ZydisRegister base, ZydisRegister index, u32 bits) {
    return o.type == ZYDIS_OPERAND_TYPE_MEMORY && o.mem.base == base && o.mem.index == index &&
           (index == ZYDIS_REGISTER_NONE || (o.mem.scale <= 1 && o.mem.disp.value == 0)) && o.size == bits &&
           o.mem.segment != ZYDIS_REGISTER_FS && o.mem.segment != ZYDIS_REGISTER_GS;
}

/// One decoded instruction to one micro-op; false if it is not one the generator emits.
bool ToOp(const ZydisDecodedInstruction& in, const ZydisDecodedOperand* op, SrtProgram::Op& out) {
    using enum SrtOp;
    const u32 n = in.operand_count_visible;
    auto set = [&](SrtOp code, s64 disp = 0, u64 imm = 0) {
        out.code = static_cast<u8>(code);
        out.disp = static_cast<s32>(disp);
        out.imm = imm;
        return true;
    };
    constexpr auto RDI = ZYDIS_REGISTER_RDI, RSI = ZYDIS_REGISTER_RSI, RSP = ZYDIS_REGISTER_RSP;
    constexpr auto R10 = ZYDIS_REGISTER_R10, R10D = ZYDIS_REGISTER_R10D, ECX = ZYDIS_REGISTER_ECX,
                   EDX = ZYDIS_REGISTER_EDX, RCX = ZYDIS_REGISTER_RCX, CL = ZYDIS_REGISTER_CL,
                   NONE = ZYDIS_REGISTER_NONE;
    switch (in.mnemonic) {
    case ZYDIS_MNEMONIC_RET:
        return set(Ret);
    case ZYDIS_MNEMONIC_PUSH:
        if (n == 1 && IsReg(op[0], RDI)) return set(PushRdi);
        if (n == 1 && IsReg(op[0], R10)) return set(PushR10);
        return false;
    case ZYDIS_MNEMONIC_POP:
        if (n == 1 && IsReg(op[0], RDI)) return set(PopRdi);
        if (n == 1 && IsReg(op[0], R10)) return set(PopR10);
        if (n == 1 && IsReg(op[0], RCX)) return set(PopRcx);
        return false;
    case ZYDIS_MNEMONIC_MOV:
        if (n != 2) return false;
        if (IsReg(op[0], ZYDIS_REGISTER_RDI) && IsMem(op[1], RDI, NONE, 64)) return set(LoadRdiDisp, op[1].mem.disp.value);
        if (IsReg(op[0], ZYDIS_REGISTER_RDI) && IsMem(op[1], RDI, R10, 64)) return set(LoadRdiR10);
        if (IsReg(op[0], R10D) && IsMem(op[1], RDI, NONE, 32)) return set(LoadR10Disp, op[1].mem.disp.value);
        if (IsReg(op[0], R10D) && IsMem(op[1], RDI, R10, 32)) return set(LoadR10R10);
        if (IsReg(op[0], R10D) && IsMem(op[1], RSI, NONE, 32)) return set(LoadR10Flat, op[1].mem.disp.value);
        if (IsMem(op[0], RSI, NONE, 32) && IsReg(op[1], R10D)) return set(StoreFlat, op[0].mem.disp.value);
        if (IsReg(op[0], R10) && IsImm(op[1])) return set(MovR10Imm64, 0, op[1].imm.value.u);
        if (IsReg(op[0], R10D) && IsImm(op[1])) return set(MovR10Imm32, 0, op[1].imm.value.u & 0xFFFFFFFFu);
        if (IsReg(op[0], R10D) && IsReg(op[1], R10D)) return set(ZextR10);
        if (IsReg(op[0], ECX) && IsReg(op[1], R10D)) return set(MovEcxR10);
        if (IsReg(op[0], ECX) && IsImm(op[1])) return set(MovEcxImm, 0, op[1].imm.value.u & 0xFFFFFFFFu);
        if (IsReg(op[0], EDX) && IsImm(op[1])) return set(MovEdxImm, 0, op[1].imm.value.u & 0xFFFFFFFFu);
        return false;
    case ZYDIS_MNEMONIC_AND:
        if (n != 2) return false;
        if (IsReg(op[0], RDI) && IsReg(op[1], R10)) return set(AndRdiR10);
        if (IsReg(op[0], R10D) && IsImm(op[1])) return set(AndR10Imm, 0, op[1].imm.value.u & 0xFFFFFFFFu);
        if (IsReg(op[0], R10D) && IsMem(op[1], RSP, NONE, 32) && op[1].mem.disp.value == 0) return set(AndR10Stack);
        if (IsMem(op[0], RSP, NONE, 32) && op[0].mem.disp.value == 0 && IsReg(op[1], EDX)) return set(AndStackEdx);
        return false;
    case ZYDIS_MNEMONIC_ADD:
        if (n != 2) return false;
        if (IsReg(op[0], RSP) && IsImm(op[1]) && op[1].imm.value.u == 8) return set(Drop);
        if (IsReg(op[0], R10D) && IsImm(op[1])) return set(AddR10Imm, 0, op[1].imm.value.u & 0xFFFFFFFFu);
        if (IsReg(op[0], R10D) && IsMem(op[1], RSP, NONE, 32) && op[1].mem.disp.value == 0) return set(AddR10Stack);
        return false;
    case ZYDIS_MNEMONIC_SUB:
        if (n == 2 && IsReg(op[0], R10D) && IsImm(op[1])) return set(SubR10Imm, 0, op[1].imm.value.u & 0xFFFFFFFFu);
        return false;
    case ZYDIS_MNEMONIC_OR:
        if (n != 2) return false;
        if (IsReg(op[0], R10D) && IsImm(op[1])) return set(OrR10Imm, 0, op[1].imm.value.u & 0xFFFFFFFFu);
        if (IsReg(op[0], R10D) && IsMem(op[1], RSP, NONE, 32) && op[1].mem.disp.value == 0) return set(OrR10Stack);
        return false;
    case ZYDIS_MNEMONIC_XOR:
        if (n != 2) return false;
        if (IsReg(op[0], R10D) && IsImm(op[1])) return set(XorR10Imm, 0, op[1].imm.value.u & 0xFFFFFFFFu);
        if (IsReg(op[0], R10D) && IsMem(op[1], RSP, NONE, 32) && op[1].mem.disp.value == 0) return set(XorR10Stack);
        return false;
    case ZYDIS_MNEMONIC_IMUL:
        if (n == 3 && IsReg(op[0], R10D) && IsReg(op[1], R10D) && IsImm(op[2]))
            return set(ImulR10Imm, 0, op[2].imm.value.u & 0xFFFFFFFFu);
        if (n == 2 && IsReg(op[0], R10D) && IsMem(op[1], RSP, NONE, 32) && op[1].mem.disp.value == 0)
            return set(ImulR10Stack);
        return false;
    case ZYDIS_MNEMONIC_NEG:
        return n == 1 && IsReg(op[0], R10D) && set(NegR10);
    case ZYDIS_MNEMONIC_NOT:
        return n == 1 && IsReg(op[0], R10D) && set(NotR10);
    case ZYDIS_MNEMONIC_DEC:
        return n == 1 && IsReg(op[0], EDX) && set(DecEdx);
    case ZYDIS_MNEMONIC_SHL:
        if (n != 2) return false;
        if (IsReg(op[0], R10D) && IsImm(op[1])) return set(ShlR10Imm, 0, op[1].imm.value.u & 0xFF);
        if (IsReg(op[0], R10D) && IsReg(op[1], CL)) return set(ShlR10Cl);
        if (IsReg(op[0], EDX) && IsReg(op[1], CL)) return set(ShlEdxCl);
        return false;
    case ZYDIS_MNEMONIC_SHR:
        if (n != 2) return false;
        if (IsReg(op[0], R10D) && IsImm(op[1])) return set(ShrR10Imm, 0, op[1].imm.value.u & 0xFF);
        if (IsReg(op[0], R10D) && IsReg(op[1], CL)) return set(ShrR10Cl);
        if (IsMem(op[0], RSP, NONE, 32) && op[0].mem.disp.value == 0 && IsReg(op[1], CL)) return set(ShrStackCl);
        return false;
    case ZYDIS_MNEMONIC_CMP:
        if (n != 2) return false;
        if (IsReg(op[0], R10D) && IsReg(op[1], ECX)) return set(CmpR10Ecx);
        if (IsReg(op[0], R10D) && IsMem(op[1], RSP, NONE, 32) && op[1].mem.disp.value == 0) return set(CmpR10Stack);
        return false;
    case ZYDIS_MNEMONIC_CMOVNBE: // cmova
        if (n == 2 && IsReg(op[0], R10D) && IsReg(op[1], ECX)) return set(CmovaR10Ecx);
        if (n == 2 && IsReg(op[0], R10D) && IsMem(op[1], RSP, NONE, 32) && op[1].mem.disp.value == 0)
            return set(CmovaR10Stack);
        return false;
    case ZYDIS_MNEMONIC_CMOVB:
        if (n == 2 && IsReg(op[0], R10D) && IsReg(op[1], ECX)) return set(CmovbR10Ecx);
        if (n == 2 && IsReg(op[0], R10D) && IsMem(op[1], RSP, NONE, 32) && op[1].mem.disp.value == 0)
            return set(CmovbR10Stack);
        return false;
    default:
        return false;
    }
}

} // namespace

bool SrtProgram::Compile(std::span<const u8> code, std::string& error) {
    ops_.clear();
    size_t pc = 0;
    while (pc < code.size()) {
        ZydisDecodedInstruction in;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&Decoder(), code.data() + pc, code.size() - pc, &in, ops))) {
            error = fmt::format("walker byte {}: not an instruction ({:02x})", pc, code[pc]);
            ops_.clear();
            return false;
        }
        Op op;
        if (!ToOp(in, ops, op)) {
            std::string bytes;
            for (u32 i = 0; i < in.length; ++i) {
                bytes += fmt::format("{:02x}", code[pc + i]);
            }
            error = fmt::format("walker byte {}: instruction {} not of the generator's forms", pc, bytes);
            ops_.clear();
            return false;
        }
        ops_.push_back(op);
        pc += in.length;
    }
    if (ops_.empty() || ops_.back().code != static_cast<u8>(SrtOp::Ret)) {
        error = "the walker does not end with ret";
        ops_.clear();
        return false;
    }
    return true;
}

bool SrtProgram::Run(std::span<const u32, 16> user_data, std::span<u32> flat, GuestReader& memory, RunStats& stats,
                     std::string& error) const {
    using enum SrtOp;
    // rdi is a pointer: into the user data registers (where the walk starts) or a guest address.
    struct Ptr {
        bool guest = false;
        u64 value = 0;
    };
    Ptr rdi{false, 0};
    u64 r10 = 0;
    u32 ecx = 0, edx = 0;
    bool cf = false, zf = false;
    struct Slot {
        bool ptr = false; // a pushed rdi
        Ptr p;
        u64 v = 0;
    };
    std::array<Slot, 64> stack;
    u32 sp = 0;
    auto push = [&](const Slot& s) {
        if (sp >= stack.size()) {
            return false;
        }
        stack[sp++] = s;
        return true;
    };
    // dword [rsp]: the low half of the top slot (a pushed 64-bit register)
    auto set_top_low = [&](u32 value) {
        u64& v = stack[sp - 1].v;
        v = (v & ~0xFFFFFFFFull) | value;
    };
    auto load = [&](const Ptr& base, s64 offset, void* dst, u32 bytes) {
        if (!base.guest) {
            // the user data registers (host memory: 16 dwords)
            if (offset < 0 || static_cast<u64>(offset) + bytes > 64) {
                return false;
            }
            std::memcpy(dst, reinterpret_cast<const u8*>(user_data.data()) + offset, bytes);
            for (u64 i = static_cast<u64>(offset) / 4; i <= (static_cast<u64>(offset) + bytes - 1) / 4; ++i) {
                stats.user_data_read |= 1u << i;
            }
            return true;
        }
        ++stats.reads;
        const u64 addr = base.value + static_cast<u64>(offset);
        if (!InGpuSpace(addr, bytes) || !memory.Read(addr, dst, bytes)) {
            ++stats.faults;
            std::memset(dst, 0, bytes); // the bridge's patched load: the register becomes 0
        }
        return true;
    };
    auto flat_index = [&](s32 disp, u32& index) {
        if (disp < 0 || disp % 4 != 0 || static_cast<u32>(disp / 4) >= flat.size()) {
            return false;
        }
        index = static_cast<u32>(disp / 4);
        return true;
    };
    for (size_t pc = 0; pc < ops_.size(); ++pc) {
        const Op& op = ops_[pc];
        u32 fi = 0;
        switch (static_cast<SrtOp>(op.code)) {
        case PushRdi:
            if (!push({true, rdi, 0})) goto stack_error;
            break;
        case PopRdi:
            if (sp == 0 || !stack[sp - 1].ptr) goto stack_error;
            rdi = stack[--sp].p;
            break;
        case PushR10:
            if (!push({false, {}, r10})) goto stack_error;
            break;
        case PopR10:
            if (sp == 0 || stack[sp - 1].ptr) goto stack_error;
            r10 = stack[--sp].v;
            break;
        case PopRcx:
            if (sp == 0 || stack[sp - 1].ptr) goto stack_error;
            ecx = static_cast<u32>(stack[--sp].v);
            break;
        case Drop:
            if (sp == 0) goto stack_error;
            --sp;
            break;
        case LoadRdiDisp:
        case LoadRdiR10: {
            u64 v = 0;
            const s64 off = static_cast<SrtOp>(op.code) == LoadRdiDisp ? op.disp : static_cast<s64>(r10);
            if (!load(rdi, off, &v, 8)) {
                error = fmt::format("micro-op {}: user data read at byte {} outside the 16 registers", pc, off);
                return false;
            }
            rdi = {true, v};
            break;
        }
        case LoadR10Disp:
        case LoadR10R10: {
            u32 v = 0;
            const s64 off = static_cast<SrtOp>(op.code) == LoadR10Disp ? op.disp : static_cast<s64>(r10);
            if (!load(rdi, off, &v, 4)) {
                error = fmt::format("micro-op {}: user data read at byte {} outside the 16 registers", pc, off);
                return false;
            }
            r10 = v;
            break;
        }
        case LoadR10Flat:
            if (!flat_index(op.disp, fi)) goto flat_error;
            r10 = flat[fi];
            if (fi < 16) {
                stats.user_data_read |= 1u << fi; // (the registers' copy at the flattened data's start)
            }
            break;
        case StoreFlat:
            if (!flat_index(op.disp, fi)) goto flat_error;
            flat[fi] = static_cast<u32>(r10);
            if (fi < 16) {
                stats.user_data_written = true;
            }
            break;
        case MovR10Imm64:
            r10 = op.imm;
            break;
        case MovR10Imm32:
            r10 = static_cast<u32>(op.imm);
            break;
        case AndRdiR10:
            rdi.value &= r10;
            break;
        case ZextR10:
            r10 = static_cast<u32>(r10);
            break;
        case ShlR10Imm:
            r10 = static_cast<u32>(static_cast<u32>(r10) << (op.imm & 31));
            break;
        case ShrR10Imm:
            r10 = static_cast<u32>(r10) >> (op.imm & 31);
            break;
        case ShlR10Cl:
            r10 = static_cast<u32>(static_cast<u32>(r10) << (ecx & 31));
            break;
        case ShrR10Cl:
            r10 = static_cast<u32>(r10) >> (ecx & 31);
            break;
        case AddR10Imm:
            r10 = static_cast<u32>(static_cast<u32>(r10) + static_cast<u32>(op.imm));
            break;
        case SubR10Imm:
            r10 = static_cast<u32>(static_cast<u32>(r10) - static_cast<u32>(op.imm));
            break;
        case AndR10Imm:
            r10 = static_cast<u32>(r10) & static_cast<u32>(op.imm);
            break;
        case OrR10Imm:
            r10 = static_cast<u32>(r10) | static_cast<u32>(op.imm);
            break;
        case XorR10Imm:
            r10 = static_cast<u32>(r10) ^ static_cast<u32>(op.imm);
            break;
        case ImulR10Imm:
            r10 = static_cast<u32>(static_cast<u32>(r10) * static_cast<u32>(op.imm));
            break;
        case NegR10:
            r10 = static_cast<u32>(0u - static_cast<u32>(r10));
            break;
        case NotR10:
            r10 = ~static_cast<u32>(r10);
            break;
        case AddR10Stack:
        case AndR10Stack:
        case OrR10Stack:
        case XorR10Stack:
        case ImulR10Stack:
        case CmpR10Stack:
        case CmovaR10Stack:
        case CmovbR10Stack: {
            if (sp == 0 || stack[sp - 1].ptr) goto stack_error;
            const u32 s = static_cast<u32>(stack[sp - 1].v);
            const u32 a = static_cast<u32>(r10);
            switch (static_cast<SrtOp>(op.code)) {
            case AddR10Stack: r10 = static_cast<u32>(a + s); break;
            case AndR10Stack: r10 = a & s; break;
            case OrR10Stack: r10 = a | s; break;
            case XorR10Stack: r10 = a ^ s; break;
            case ImulR10Stack: r10 = static_cast<u32>(a * s); break;
            case CmpR10Stack: cf = a < s; zf = a == s; break;
            case CmovaR10Stack: r10 = (!cf && !zf) ? s : a; break;
            default: r10 = cf ? s : a; break; // cmovb
            }
            break;
        }
        case MovEcxR10:
            ecx = static_cast<u32>(r10);
            break;
        case MovEcxImm:
            ecx = static_cast<u32>(op.imm);
            break;
        case CmpR10Ecx:
            cf = static_cast<u32>(r10) < ecx;
            zf = static_cast<u32>(r10) == ecx;
            break;
        case CmovaR10Ecx:
            r10 = (!cf && !zf) ? ecx : static_cast<u32>(r10);
            break;
        case CmovbR10Ecx:
            r10 = cf ? ecx : static_cast<u32>(r10);
            break;
        case ShrStackCl:
            if (sp == 0 || stack[sp - 1].ptr) goto stack_error;
            set_top_low(static_cast<u32>(stack[sp - 1].v) >> (ecx & 31));
            break;
        case MovEdxImm:
            edx = static_cast<u32>(op.imm);
            break;
        case ShlEdxCl:
            edx <<= (ecx & 31);
            break;
        case DecEdx:
            edx -= 1;
            break;
        case AndStackEdx:
            if (sp == 0 || stack[sp - 1].ptr) goto stack_error;
            set_top_low(static_cast<u32>(stack[sp - 1].v) & edx);
            break;
        case Ret:
            if (sp != 0) {
                error = fmt::format("ret with {} values on the walker's stack", sp);
                return false;
            }
            return true;
        }
        continue;
    stack_error:
        error = fmt::format("micro-op {}: the walker's stack is misused (depth {})", pc, sp);
        return false;
    flat_error:
        error = fmt::format("micro-op {}: flattened user data offset {} outside its {} dwords", pc, op.disp,
                            flat.size());
        return false;
    }
    error = "the walker ran past its end";
    return false;
}

// ---- resolution

std::string_view ResourceStatusName(ResourceStatus status) {
    static constexpr std::string_view kNames[] = {"ok",           "null",        "wrong type",
                                                  "out of range", "bad location", "unresolved"};
    static_assert(std::size(kNames) == static_cast<size_t>(ResourceStatus::Count));
    const auto i = static_cast<size_t>(status);
    return i < std::size(kNames) ? kNames[i] : "?";
}

void ResourceBindingPlan::Clear() {
    flat.clear();
    buffers.clear();
    images.clear();
    samplers.clear();
    vertices.clear();
    status_counts = {};
    srt = {};
    error.clear();
}

bool FetchSharp(const SharpLocation& loc, std::span<const u32> flat, std::span<u32> out) {
    const size_t n = std::min<size_t>(out.size(), 8);
    switch (loc.summary) {
    case SharpLocation::Summary::SingleLoad:
        if (size_t{loc.offsets[0]} + n > flat.size()) {
            return false;
        }
        std::copy_n(flat.data() + loc.offsets[0], n, out.data());
        return true;
    case SharpLocation::Summary::MultiLoad:
        for (size_t i = 0; i < n; ++i) {
            if ((loc.load_mask >> i) & 1) {
                if (loc.offsets[i] >= flat.size()) {
                    return false;
                }
                out[i] = flat[loc.offsets[i]];
            } else {
                out[i] = loc.immediates[i];
            }
        }
        return true;
    default:
        return false;
    }
}

namespace {

void Count(ResourceBindingPlan& plan, ResourceStatus status) {
    ++plan.status_counts[static_cast<size_t>(status)];
}

/// The image type a view of `type` has (the bridge's GetViewType: arrays seen without their array index are single).
u32 ViewType(u32 type, bool is_array) {
    if (!is_array && (type == 12 || type == 13 || type == 15)) { // 1D array, 2D array, 2D MSAA array
        return type - 4;
    }
    return type;
}

} // namespace

bool ResolveResources(const ShaderReflection& r, const SrtProgram& srt, const BindingStart& start,
                      const FetchShader* fetch, std::span<const u32, 16> user_data, u64 program_address,
                      GuestReader& memory, ResourceBindingPlan& plan) {
    plan.Clear();
    plan.flat.resize(std::max<u32>(r.flatbuf_dwords, 16));
    std::copy(user_data.begin(), user_data.end(), plan.flat.begin());
    bool ok = true;
    SrtProgram::RunStats stats;
    std::string error;
    if (!srt.Empty() && !srt.Run(user_data, plan.flat, memory, stats, error)) {
        ok = false; // the flattened data past the registers is not trustworthy: descriptors there are Unresolved
        plan.flat.resize(16);
    }
    DecodeResources(r, start, fetch, user_data, program_address, VertexSource{&memory, {}}, plan);
    plan.srt = stats;
    plan.error = std::move(error);
    return ok;
}

void DecodeResources(const ShaderReflection& r, const BindingStart& start, const FetchShader* fetch,
                     std::span<const u32, 16> user_data, u64 program_address, const VertexSource& vertices,
                     ResourceBindingPlan& plan) {
    // plan.flat is the flattened user data (ResolveResources) or the caller's (a translation's)
    plan.buffers.clear();
    plan.images.clear();
    plan.samplers.clear();
    plan.vertices.clear();
    plan.status_counts = {};
    plan.stage = r.hw;
    if (plan.flat.size() < 16) {
        plan.flat.resize(16);
        std::copy(user_data.begin(), user_data.end(), plan.flat.begin());
    }
    // a walk that failed left only the registers: whatever the reflection locates past them is unresolved
    const bool walked = plan.flat.size() >= r.flatbuf_dwords;
    const std::span<const u32> flat(plan.flat);
    auto missing = [&] { return walked ? ResourceStatus::BadLocation : ResourceStatus::Unresolved; };
    u32 binding = start.unified;

    for (const BufferResource& b : r.buffers) {
        BufferBinding out;
        out.binding = binding++;
        out.kind = b.kind;
        out.written = b.is_written;
        out.formatted = b.is_formatted;
        if (b.kind == BufferKind::Guest) {
            if (!FetchSharp(b.sharp, flat, out.desc.raw)) {
                out.status = missing();
            } else {
                if (b.post_op == SharpPostOp::BitwiseOrDw1WithImm) {
                    out.desc.raw[1] |= b.post_op_dw1_mask;
                }
                out.address = out.desc.Base();
                if (b.post_op == SharpPostOp::OffsetByProgramBase) {
                    out.address = (out.address + program_address) & (kGpuSpace - 1);
                }
                out.size = out.desc.Size();
                if (!out.desc.IsBuffer()) {
                    out.status = ResourceStatus::WrongType;
                } else if (out.desc.NumRecords() == 0) {
                    out.status = ResourceStatus::Null;
                } else if (!InGpuSpace(out.address, out.size)) {
                    out.status = ResourceStatus::OutOfRange;
                }
            }
        }
        Count(plan, out.status);
        plan.buffers.push_back(out);
    }
    for (const ImageResource& im : r.images) {
        ImageBinding out;
        out.binding = binding;
        out.storage = im.is_written;
        out.depth = im.is_depth;
        out.array = im.is_array;
        out.atomic = im.is_atomic;
        const u32 dwords = im.is_r128 ? 4 : 8;
        if (!FetchSharp(im.sharp, flat, std::span<u32>(out.desc.raw.data(), dwords))) {
            out.status = missing();
        } else {
            if (im.is_r128) {
                // a 128-bit T#: the pitch is its width (the bridge's ImageResource::Fetch)
                const u32 width = out.desc.raw[2] & 0x3FFF;
                out.desc.raw[4] = (out.desc.raw[4] & ~(0x3FFFu << 13)) | (width << 13);
            }
            if (im.post_op == SharpPostOp::ConvertCubeTo2DArray) {
                const u32 depth = ((out.desc.raw[4] & 0x1FFF) + 1) * 6 - 1;
                out.desc.raw[4] = (out.desc.raw[4] & ~0x1FFFu) | (depth & 0x1FFF);
                out.desc.raw[3] = (out.desc.raw[3] & 0x0FFFFFFFu) | (kImageType2DArray << 28);
            }
            out.view_type = ViewType(out.desc.Type(), im.is_array);
            out.fmask = out.desc.DataFormat() >= kFirstFmaskFormat && out.desc.DataFormat() <= kLastFmaskFormat;
            if (out.desc.Base() == 0) {
                out.status = ResourceStatus::Null;
            } else if (!out.desc.IsImage()) {
                out.status = ResourceStatus::WrongType;
            } else if (im.is_depth && out.desc.DataFormat() != 2 && out.desc.DataFormat() != 4) {
                out.status = ResourceStatus::WrongType; // a depth read of a non-16/32-bit format (the bridge: null)
            } else if (!InGpuSpace(out.desc.Base(), 1)) {
                out.status = ResourceStatus::OutOfRange;
            }
            if (im.mip_fallback == MipFallback::DynamicIndex && out.status == ResourceStatus::Ok) {
                const u32 base = out.desc.BaseLevel(), last = out.desc.LastLevel();
                out.count = last >= base ? last - base + 1 : 1;
            }
        }
        binding += out.count;
        Count(plan, out.status);
        plan.images.push_back(out);
    }
    for (const SamplerResource& s : r.samplers) {
        SamplerBinding out;
        out.binding = binding++;
        if (!FetchSharp(s.sharp, flat, out.desc.raw)) {
            out.status = missing();
        } else {
            u32& w0 = out.desc.raw[0];
            switch (s.post_op) {
            case SharpPostOp::DisableAnisoIfSingleLod:
                if (s.post_op_tsharp_dw3 >= flat.size()) {
                    out.status = missing();
                } else if (((flat[s.post_op_tsharp_dw3] >> 12) & 0xFF) == 0) {
                    w0 &= ~(7u << 9);
                }
                break;
            case SharpPostOp::ForceRepeatXyzClamp:
                w0 &= ~0x1FFu;
                break;
            case SharpPostOp::ForceLastTexelXyClamp:
                w0 = (w0 & ~0x1FFu) | 2u | (2u << 3);
                break;
            case SharpPostOp::ClearAnisoRatioAndThreshold:
                w0 &= ~((7u << 9) | (7u << 16));
                break;
            default:
                break;
            }
            if (out.status == ResourceStatus::Ok) {
                if (out.desc.Empty()) {
                    out.status = ResourceStatus::Null;
                } else if (!out.desc.InRange()) {
                    out.status = ResourceStatus::WrongType;
                }
            }
        }
        Count(plan, out.status);
        plan.samplers.push_back(out);
    }
    if (fetch != nullptr && fetch->present) {
        for (u32 i = 0; i < fetch->attributes.size(); ++i) {
            const FetchAttribute& a = fetch->attributes[i];
            VertexBinding out;
            out.attribute = i;
            out.fetch = a;
            bool got = false;
            if (!vertices.given.empty()) {
                if (i < vertices.given.size()) {
                    out.desc.raw = vertices.given[i];
                    got = true;
                } else {
                    out.status = ResourceStatus::Unresolved;
                }
            } else if (a.sgpr_base + 1 >= 16) {
                out.status = ResourceStatus::BadLocation;
            } else {
                const u64 table =
                    (user_data[a.sgpr_base] | (u64{user_data[a.sgpr_base + 1]} << 32)) & 0xFFFFFFFFFFFFull;
                const u64 at = table + u64{a.dword_offset} * 4;
                got = vertices.memory != nullptr && InGpuSpace(at, 16) &&
                      vertices.memory->Read(at, out.desc.raw.data(), 16);
                if (!got) {
                    out.status = ResourceStatus::Unresolved;
                }
            }
            if (got) {
                out.read = out.desc.raw;
                if (a.data_format != 0) { // a typed load: its format replaces the V#'s
                    out.desc.raw[3] = (out.desc.raw[3] & ~((0xFu << 15) | (7u << 12))) | (a.data_format << 15) |
                                      (a.num_format << 12);
                }
                out.address = out.desc.Base() + a.inst_offset;
                out.size = out.desc.Size();
                if (!out.desc.IsBuffer()) {
                    out.status = ResourceStatus::WrongType;
                } else if (out.desc.NumRecords() == 0) {
                    out.status = ResourceStatus::Null;
                } else if (!InGpuSpace(out.address, out.size)) {
                    out.status = ResourceStatus::OutOfRange;
                }
            }
            Count(plan, out.status);
            plan.vertices.push_back(out);
        }
    }
}

BindingStart EndOf(const BindingStart& start, const ResourceBindingPlan& plan) {
    BindingStart end = start;
    end.unified += static_cast<u32>(plan.buffers.size() + plan.samplers.size());
    for (const ImageBinding& im : plan.images) {
        end.unified += im.count;
    }
    end.buffer += static_cast<u32>(plan.buffers.size());
    return end;
}

// ---- specialization

void BuildSpecializationKey(const RuntimeParams& runtime, const BindingStart& start, const ShaderReflection& r,
                            const FetchShader* fetch, const ResourceBindingPlan& plan, SpecializationKey& out) {
    auto& w = out.words;
    w.clear();
    w.resize(sizeof(RuntimeParams) / 4);
    std::memcpy(w.data(), &runtime, sizeof(RuntimeParams));
    const bool any = !r.buffers.empty() || !r.images.empty() || !r.samplers.empty();
    w.push_back(any ? start.unified : 0);
    w.push_back(any ? start.buffer : 0);
    // vertex attributes: the fetch shader's form and each V#'s formats (the bridge: number class, swizzle)
    const bool has_fetch = fetch != nullptr && fetch->present;
    w.push_back(has_fetch ? 1u + static_cast<u32>(fetch->attributes.size()) : 0u);
    if (has_fetch) {
        w.push_back(static_cast<u32>(fetch->vertex_offset_sgpr));
        w.push_back(static_cast<u32>(fetch->instance_offset_sgpr));
    }
    for (const VertexBinding& v : plan.vertices) {
        const FetchAttribute& a = v.fetch;
        w.push_back(a.dest_vgpr | (a.num_elements << 8) | (a.sgpr_base << 16) | (a.instance_data << 24));
        w.push_back(a.dword_offset | (a.inst_offset << 16));
        w.push_back(a.data_format | (a.num_format << 8));
        w.push_back(v.status == ResourceStatus::Ok ? (v.desc.raw[3] & 0x7FFFFu)
                                                   : 0x80000000u | static_cast<u32>(v.status));
    }
    for (const BufferBinding& b : plan.buffers) {
        if (b.kind != BufferKind::Guest || b.status != ResourceStatus::Ok) {
            w.push_back(0x80000000u | static_cast<u32>(b.kind) << 8 | static_cast<u32>(b.status));
            continue;
        }
        const BufferDesc& d = b.desc;
        w.push_back(d.Stride() | (u32{d.SwizzleEnable()} << 14) | (u32{d.AddTidEnable()} << 15) |
                    (u32{d.Mtype() == 3} << 16) | (d.IndexStride() << 17) | (d.ElementSize() << 19));
        w.push_back(d.raw[3] & 0x7FFFFu); // DST_SEL, NUM_FORMAT, DATA_FORMAT
    }
    for (const ImageBinding& im : plan.images) {
        if (im.status != ResourceStatus::Ok) {
            w.push_back(0x80000000u | static_cast<u32>(im.status) | (im.depth ? 0x100u : 0u));
            continue;
        }
        const ImageDesc& d = im.desc;
        w.push_back(im.view_type | (d.DataFormat() << 4) | (d.NumFormat() << 10) | (im.count << 14));
        w.push_back(im.storage ? (d.raw[3] & 0xFFFu) : 0u); // a storage image's swizzle is applied by the shader
        w.push_back(im.fmask ? (d.Width() | (d.Height() << 16)) : 0u);
    }
    for (const SamplerBinding& s : plan.samplers) {
        w.push_back(s.status == ResourceStatus::Ok
                        ? (u32{s.desc.ForceUnnormalized()} | (u32{s.desc.ForceDegamma()} << 1))
                        : 0x80000000u | static_cast<u32>(s.status));
    }
    for (const FmaskResource& f : r.fmasks) {
        ImageDesc d;
        const bool got = size_t{f.sharp_idx} + 8 <= plan.flat.size();
        if (got) {
            std::copy_n(plan.flat.data() + f.sharp_idx, 8, d.raw.data());
        }
        w.push_back(got ? (d.Width() | (d.Height() << 16)) : 0xFFFFFFFFu);
    }
    out.hash = XXH3_64bits(w.data(), w.size() * 4);
}

} // namespace SecondSon::Gpu::Shader
