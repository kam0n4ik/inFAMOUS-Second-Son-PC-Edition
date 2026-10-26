"""R12 wfx (docs/R12_WEATHER_PROFILE.md): the particle ranking CS 0x2edaa0c00 - a variant of the own translator's SPIR-V
for SSPC_R12_PSORT=1.

What the program does [A] (its GCN through the translation, spirv-cross view): one invocation per particle i of an
emitter (records [base, end), base and count from its constant buffer, the key = dword +0x1c of a record:
hi = key >> 20 arithmetic, lo = key & 0xFFFFF). The loop walks j over ALL records [base, end) and counts
  r273 += hi_j "below" hi_i (bit 19 of hi_i - hi_j clear, hi_j != hi_i)       r272 += hi_j == hi_i
  among hi_j == hi_i, lo_j != lo_i: lo_i "below" lo_j (bit 19 of lo_i - lo_j set): the nearest such lo_j (bit 19 of
  min - lo_j clear, or none yet) -> r143 = lo_j, r274 = j; else r141 += 1 and the nearest lo_j below (bit 19 of
  lo_j - max clear, or none yet) -> r221 = lo_j, r275 = j
then writes the record to end + rank (the region >= end: the keys it reads are never written by the dispatch).
The translation emulates GCN's exec mask with subgroupBallot / InverseBallot: ~40 dependent operations a step, a
dispatch of one workgroup takes ~100 us (80 a frame in the owner's save).

The variant: before the loop, an equivalent per-invocation loop (no ballots, 4 keys loaded at once, the same
compare formulas, the same order of j, so the same results) computes those registers and sets j = end; the
original loop then runs only its exit test (its header, which also makes the exec mask for after the loop, from the
final r274 / r275 as in the original's last pass). Nothing after the loop reads the scratch registers its body wrote
(checked on the dump: _182 _1107 _1146 _1147 _167 _171 _426 _430 are not read after it; _300 _410 _174 _156 _75 are
written before their reads).

  py -I psort_patch.py <in.spvasm> <out.spvasm>
  py -I psort_patch.py --header <shader_2edaa0c00_*.spv> <vk_r12_psort_spv.h>
     (SSPC_DIAG_DUMP_SHADER=2edaa0c00's dump -> spirv-dis, the patch, spirv-as, spirv-val -> the executor's header)
The ids are those of the dump with FNV-1a 976640fd391c29d1 (7110 words); every anchor is checked, the patch fails
loudly on another translation.
"""
import os
import re
import struct
import subprocess
import sys
import tempfile

UNROLL = 4


def need(lines, pattern, count=1):
    hits = [i for i, l in enumerate(lines) if re.fullmatch(pattern, l.strip())]
    if len(hits) != count:
        raise SystemExit(f"anchor {pattern!r}: {len(hits)} matches, expected {count}")
    return hits


def patch(src, dst):
    lines = open(src, encoding="utf-8").read().split("\n")
    # the variables (Function, uint / bool) the patch reads and writes
    for v, t in [("73", "bool"), ("141", "uint"), ("143", "uint"), ("147", "uint"), ("221", "uint"), ("257", "uint"),
                 ("261", "uint"), ("272", "uint"), ("273", "uint"), ("274", "uint"), ("275", "uint"), ("285", "uint")]:
        need(lines, rf"%{v} = OpVariable %_ptr_Function_{t} Function %\w+")
    for c in ["uint_0 = OpConstant %uint 0", "uint_1 = OpConstant %uint 1", "uint_2 = OpConstant %uint 2",
              "uint_8 = OpConstant %uint 8", "uint_14 = OpConstant %uint 14", "uint_16 = OpConstant %uint 16",
              "uint_20 = OpConstant %uint 20", "uint_28 = OpConstant %uint 28", "uint_31 = OpConstant %uint 31",
              "uint_1048575 = OpConstant %uint 1048575", "uint_524288 = OpConstant %uint 524288"]:
        need(lines, "%" + re.escape(c))
    # the one loop: its header %51, exit test _147 < _261
    need(lines, r"OpLoopMerge %\w+ %\w+ None")
    lm = need(lines, r"OpLoopMerge %67 %58 None")[0]
    if lines[lm - 1].strip() != "%51 = OpLabel" or lines[lm - 2].strip() != "OpBranch %51" or \
            lines[lm - 3].strip() != "%277 = OpLabel":
        raise SystemExit("the block before the loop header is not '%277: OpBranch %51'")
    # the key load in the loop body (as the translation addresses it)
    body = "\n".join(lines[lm:lm + 1200])
    for p in [r"OpAccessChain %_ptr_StorageBuffer_uint %34 %uint_0 %uint_1\n\s*%1008 = OpLoad %uint %1007\n\s*"
              r"%1009 = OpBitFieldUExtract %uint %1008 %uint_16 %uint_14",
              r"%1013 = OpAccessChain %_ptr_PushConstant_uint %45 %uint_1 %uint_0\n\s*%1014 = OpLoad %uint %1013\n\s*"
              r"%1015 = OpBitFieldUExtract %uint %1014 %uint_8 %uint_8",
              r"%1018 = OpAccessChain %_ptr_StorageBuffer_uint %18 %uint_0 %1017\n\s*%1019 = OpLoad %uint %1018"]:
        if not re.search(p, body):
            raise SystemExit(f"the loop's key load differs: {p!r}")
    ind = "               "
    n = [0]

    def i(name):
        n[0] += 1
        return f"%w{n[0]}_{name}"

    r = []
    E = r.append
    consts = [f"%w_u{k} = OpConstant %uint {k}" for k in range(3, UNROLL + 1)]
    cu = {0: "%uint_0", 1: "%uint_1", 2: "%uint_2"}
    cu.update({k: f"%w_u{k}" for k in range(3, UNROLL + 1)})
    # every invocation runs the loop (the same trip count: j and end are the same in all), the inactive ones (exec bit
    # clear) with every update masked off - the subgroup stays converged as it enters the original loop's header
    act = i("act")
    E(f"{act} = OpLoad %bool %73")
    s0, s1, st, o0, o1, of, b28 = i("s0"), i("s1"), i("stride"), i("o0"), i("o1"), i("off"), i("b28")
    E(f"{s0} = OpAccessChain %_ptr_StorageBuffer_uint %34 %uint_0 %uint_1")
    E(f"{s1} = OpLoad %uint {s0}")
    E(f"{st} = OpBitFieldUExtract %uint {s1} %uint_16 %uint_14")
    E(f"{o0} = OpAccessChain %_ptr_PushConstant_uint %45 %uint_1 %uint_0")
    E(f"{o1} = OpLoad %uint {o0}")
    E(f"{of} = OpBitFieldUExtract %uint {o1} %uint_8 %uint_8")
    hown, lown, end, last, j0 = i("hown"), i("lown"), i("end"), i("last"), i("j0")
    E(f"{hown} = OpLoad %uint %285")
    E(f"{lown} = OpLoad %uint %257")
    E(f"{end} = OpLoad %uint %261")
    E(f"{last} = OpISub %uint {end} %uint_1")
    E(f"{j0} = OpLoad %uint %147")
    E(f"OpStore %w_j {j0}")
    hdr, cont, merge, cond, bod = i("h"), i("c"), i("lm"), i("cond"), i("body")
    E(f"OpBranch {hdr}")
    E(f"{hdr} = OpLabel")
    E(f"OpLoopMerge {merge} {cont} None")
    E(f"OpBranch {cond}")
    E(f"{cond} = OpLabel")
    j, lt = i("j"), i("lt")
    E(f"{j} = OpLoad %uint %w_j")
    E(f"{lt} = OpULessThan %bool {j} {end}")
    E(f"OpBranchConditional {lt} {bod} {merge}")
    E(f"{bod} = OpLabel")
    # the keys of j .. j+UNROLL-1 (indices clamped to end - 1: the loads stay in [base, end))
    keys, jj, ins = [], [], []
    for k in range(UNROLL):
        jk = j if k == 0 else i(f"j{k}")
        if k:
            E(f"{jk} = OpIAdd %uint {j} {cu[k]}")
        ck, mk, ak, bk, pk, chk = i(f"cl{k}"), i(f"m{k}"), i(f"a{k}"), i(f"b{k}"), i(f"p{k}"), i(f"ch{k}")
        E(f"{ck} = OpExtInst %uint %1 UMin {jk} {last}")
        E(f"{mk} = OpIMul %uint {ck} {st}")         # ((0 + 28) + j * stride) + offset, as the translation
        E(f"{ak} = OpIAdd %uint %uint_28 {mk}")
        E(f"{bk} = OpIAdd %uint {ak} {of}")
        E(f"{pk} = OpShiftRightLogical %uint {bk} %uint_2")
        E(f"{chk} = OpAccessChain %_ptr_StorageBuffer_uint %18 %uint_0 {pk}")
        jj.append(jk)
        keys.append(chk)
    vals = []
    for k, chk in enumerate(keys):
        vk = i(f"key{k}")
        E(f"{vk} = OpLoad %uint {chk}")
        vals.append(vk)
    st_ = {}
    for v in ["272", "273", "141", "143", "274", "221", "275"]:
        x = i(f"r{v}")
        E(f"{x} = OpLoad %uint %{v}")
        st_[v] = x
    for k in range(UNROLL):
        key, jk = vals[k], jj[k]
        if k == 0:
            ink = act
        else:
            ink0, ink = i(f"lt{k}"), i(f"in{k}")
            E(f"{ink0} = OpULessThan %bool {jk} {end}")
            E(f"{ink} = OpLogicalAnd %bool {ink0} {act}")
        ki, sh, hi_i, hj = i("ki"), i("sh"), i("hii"), i("hj")
        E(f"{ki} = OpBitcast %int {key}")
        E(f"{sh} = OpBitwiseAnd %uint %uint_20 %uint_31")
        E(f"{hi_i} = OpShiftRightArithmetic %int {ki} {sh}")
        E(f"{hj} = OpBitcast %uint {hi_i}")
        eq0, eq, ne_h = i("eq0"), i("eq"), i("neh")
        E(f"{eq0} = OpIEqual %bool {hj} {hown}")
        E(f"{eq} = OpLogicalAnd %bool {eq0} {ink}")
        E(f"{ne_h} = OpLogicalNot %bool {eq0}")
        # r273 += (hown - hj) & 0x80000 == 0 and hj != hown
        dh, dh2, ge, inc, inc1, n273 = i("dh"), i("dh2"), i("ge"), i("inc"), i("inc1"), i("n273")
        E(f"{dh} = OpISub %uint {hown} {hj}")
        E(f"{dh2} = OpBitwiseAnd %uint {dh} %uint_524288")
        E(f"{ge} = OpIEqual %bool {dh2} %uint_0")
        E(f"{inc} = OpLogicalAnd %bool {ge} {ne_h}")
        E(f"{inc1} = OpLogicalAnd %bool {inc} {ink}")
        sel273 = i("s273")
        E(f"{sel273} = OpSelect %uint {inc1} %uint_1 %uint_0")
        E(f"{n273} = OpIAdd %uint {st_['273']} {sel273}")
        st_["273"] = n273
        # r272 += hj == hown
        sel272, n272 = i("s272"), i("n272")
        E(f"{sel272} = OpSelect %uint {eq} %uint_1 %uint_0")
        E(f"{n272} = OpIAdd %uint {st_['272']} {sel272}")
        st_["272"] = n272
        lj, nel, eqne = i("lj"), i("nel"), i("eqne")
        E(f"{lj} = OpBitwiseAnd %uint %uint_1048575 {key}")
        E(f"{nel} = OpINotEqual %bool {lj} {lown}")
        E(f"{eqne} = OpLogicalAnd %bool {eq} {nel}")
        dl, dl2, less, nless = i("dl"), i("dl2"), i("less"), i("nless")
        E(f"{dl} = OpISub %uint {lown} {lj}")
        E(f"{dl2} = OpBitwiseAnd %uint {dl} %uint_524288")
        E(f"{less} = OpINotEqual %bool {dl2} %uint_0")
        E(f"{nless} = OpLogicalNot %bool {less}")
        # the nearest lo_j above lo_i: r143 / r274
        cand, a, a2, c1, s274, c2, c, us = i("cand"), i("a"), i("a2"), i("c1"), i("i274"), i("c2"), i("c"), i("us")
        E(f"{cand} = OpLogicalAnd %bool {eqne} {less}")
        E(f"{a} = OpISub %uint {st_['143']} {lj}")
        E(f"{a2} = OpBitwiseAnd %uint {a} %uint_524288")
        E(f"{c1} = OpIEqual %bool {a2} %uint_0")
        E(f"{s274} = OpBitcast %int {st_['274']}")
        E(f"{c2} = OpSLessThan %bool {s274} %int_0")
        E(f"{c} = OpLogicalOr %bool {c2} {c1}")
        E(f"{us} = OpLogicalAnd %bool {cand} {c}")
        n143, n274 = i("n143"), i("n274")
        E(f"{n143} = OpSelect %uint {us} {lj} {st_['143']}")
        E(f"{n274} = OpSelect %uint {us} {jk} {st_['274']}")
        st_["143"], st_["274"] = n143, n274
        # below: r141 += 1, the nearest lo_j below lo_i: r221 / r275
        gr, s141, n141 = i("gr"), i("s141"), i("n141")
        E(f"{gr} = OpLogicalAnd %bool {eqne} {nless}")
        E(f"{s141} = OpSelect %uint {gr} %uint_1 %uint_0")
        E(f"{n141} = OpIAdd %uint {st_['141']} {s141}")
        st_["141"] = n141
        b, b2, d1, s275, d2, d, up = i("b"), i("b2"), i("d1"), i("i275"), i("d2"), i("d"), i("up")
        E(f"{b} = OpISub %uint {lj} {st_['221']}")
        E(f"{b2} = OpBitwiseAnd %uint {b} %uint_524288")
        E(f"{d1} = OpIEqual %bool {b2} %uint_0")
        E(f"{s275} = OpBitcast %int {st_['275']}")
        E(f"{d2} = OpSLessThan %bool {s275} %int_0")
        E(f"{d} = OpLogicalOr %bool {d2} {d1}")
        E(f"{up} = OpLogicalAnd %bool {gr} {d}")
        n221, n275 = i("n221"), i("n275")
        E(f"{n221} = OpSelect %uint {up} {lj} {st_['221']}")
        E(f"{n275} = OpSelect %uint {up} {jk} {st_['275']}")
        st_["221"], st_["275"] = n221, n275
    for v, x in st_.items():
        E(f"OpStore %{v} {x}")
    jn = i("jn")
    E(f"{jn} = OpIAdd %uint {j} {cu[UNROLL]}")
    E(f"OpStore %w_j {jn}")
    E(f"OpBranch {cont}")
    E(f"{cont} = OpLabel")
    E(f"OpBranch {hdr}")
    E(f"{merge} = OpLabel")
    e2 = i("e2")
    E(f"{e2} = OpLoad %uint %261")   # every invocation: j = end (the step index is the same in all of them)
    E(f"OpStore %147 {e2}")
    E("OpBranch %51")
    out = lines[:lm - 2] + [ind + x for x in r] + lines[lm - 1:]
    # constants before the function, the loop variable among the function's variables
    fi = next(k for k, l in enumerate(out) if re.match(r"\s*%48 = OpFunction ", l))
    extra = consts + ([] if any(re.match(r"\s*%int_0 = OpConstant %int 0$", l) for l in out) else
                      ["%int_0 = OpConstant %int 0"])
    out = out[:fi] + extra + out[fi:]
    lab = need(out, r"%71 = OpLabel")[0]
    out = out[:lab + 1] + [ind + "%w_j = OpVariable %_ptr_Function_uint Function %uint_0"] + out[lab + 1:]
    open(dst, "w", encoding="utf-8").write("\n".join(out))
    print(f"patched: the ranking loop computed before it ({UNROLL} keys a step), {len(r)} instructions")


def header(spv, out_h):
    sdk = os.environ.get("VULKAN_SDK_BIN", r"E:\VulkanSDK\Bin")
    data = open(spv, "rb").read()
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    with tempfile.TemporaryDirectory() as d:
        a, b2, o = os.path.join(d, "in.spvasm"), os.path.join(d, "out.spvasm"), os.path.join(d, "out.spv")
        subprocess.run([os.path.join(sdk, "spirv-dis.exe"), spv, "-o", a], check=True)
        patch(a, b2)
        subprocess.run([os.path.join(sdk, "spirv-as.exe"), "--target-env", "vulkan1.3", b2, "-o", o], check=True)
        subprocess.run([os.path.join(sdk, "spirv-val.exe"), "--target-env", "vulkan1.3", o], check=True)
        words = struct.unpack(f"<{os.path.getsize(o) // 4}I", open(o, "rb").read())
    lines = [", ".join(f"0x{w:08x}" for w in words[i:i + 8]) + "," for i in range(0, len(words), 8)]
    nl = "\n"
    text = ("// SecondSonPC R12 wfx - generated by tools/r12/psort_patch.py --header (do not edit): the particle ranking CS" + nl
            + "// 0x2edaa0c00's translation with its ranking loop computed without the exec-mask emulation (SSPC_R12_PSORT)." + nl
            + f"// Source: {os.path.basename(spv)} ({len(data) // 4} words)." + nl
            + "#pragma once" + nl + nl + "#include <cstddef>" + nl + nl + '#include "base/abi.h"' + nl + nl
            + "namespace SecondSon::Gpu::Vk {" + nl + nl
            + f"inline constexpr size_t kR12PsortOrigWords = {len(data) // 4};" + nl
            + f"inline constexpr u64 kR12PsortOrigFnv = 0x{h:016x}ull;" + nl
            + "inline constexpr u32 kR12PsortSpirv[] = {" + nl + nl.join("    " + l for l in lines) + nl + "};" + nl + nl
            + "} // namespace SecondSon::Gpu::Vk" + nl)
    open(out_h, "wb").write(text.encode("utf-8"))
    print(f"header: {out_h}: original {len(data) // 4} words fnv {h:016x}, patched {len(words)} words")


def main():
    if sys.argv[1] == "--header":
        return header(sys.argv[2], sys.argv[3])
    patch(sys.argv[1], sys.argv[2])


if __name__ == "__main__":
    main()
