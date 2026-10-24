"""R10 sysrt: where a game module uses some of its imports (read-only; the module is data, not instructions).

    python -I tools/r10/import_sites.py <module.prx|eboot.bin> <nid> [<nid> ...] [--before 24] [--after 8]

For each NID: its relocation slots (JUMP_SLOT / GLOB_DAT / 64), the PLT stubs that jump through a slot, and every
instruction of the executable segments that calls such a stub or reads such a slot rip-relatively, with the
instructions around it (capstone, linear sweep of the code segment - good enough to read the call sites).
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from ps4elf import PT_LOAD, Ps4Elf  # noqa: E402

import capstone  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("module", type=Path)
    ap.add_argument("nids", nargs="+")
    ap.add_argument("--before", type=int, default=24)
    ap.add_argument("--after", type=int, default=8)
    a = ap.parse_args()

    elf = Ps4Elf(a.module)
    mod = elf.parse_module()
    img, _top = elf.build_image()
    slots = {}
    for r in mod.relocs:
        if 0 <= r.sym_index < len(mod.symbols) and mod.symbols[r.sym_index].nid in a.nids:
            slots[r.offset] = mod.symbols[r.sym_index]
            print(f"slot {r.offset:#x} type {r.type} addend {r.addend} <- {mod.symbols[r.sym_index].raw_name}")

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = False
    code = [p for p in mod.phdrs if p.p_type == PT_LOAD and (p.p_flags & 1)]
    insns = []
    for p in code:
        start, end = p.p_vaddr, p.p_vaddr + p.p_filesz
        off = start
        while off < end:
            got = False
            for i in md.disasm(bytes(img[off:end]), off):
                insns.append(i)
                off = i.address + i.size
                got = True
            if not got or off < end:
                off += 1  # undecodable byte: skip it
    # rip-relative memory operand target of an instruction (from the text form "[rip + 0x...]")
    def rip_target(i) -> int | None:
        s = i.op_str
        k = s.find("[rip ")
        if k < 0:
            return None
        e = s.find("]", k)
        expr = s[k + 5:e].replace(" ", "")
        try:
            disp = int(expr[1:], 16) * (-1 if expr[0] == "-" else 1)
        except ValueError:
            return None
        return i.address + i.size + disp

    stubs = {}
    for i in insns:
        t = rip_target(i)
        if t in slots and i.mnemonic == "jmp":
            stubs[i.address] = slots[t]
    for addr, s in stubs.items():
        print(f"plt stub {addr:#x} -> {s.raw_name}")
    index = {i.address: n for n, i in enumerate(insns)}
    for n, i in enumerate(insns):
        hit = None
        t = rip_target(i)
        if t in slots and i.address not in stubs:
            hit = f"reads slot of {slots[t].raw_name}"
        elif i.mnemonic in ("call", "jmp") and i.op_str.startswith("0x"):
            try:
                tgt = int(i.op_str, 16)
            except ValueError:
                tgt = None
            if tgt in stubs:
                hit = f"{i.mnemonic}s {stubs[tgt].raw_name}"
        if hit:
            print(f"\n=== {i.address:#x}: {hit}")
            for j in insns[max(0, n - a.before):n + a.after + 1]:
                mark = ">>" if j.address == i.address else "  "
                print(f"{mark} {j.address:#08x}  {j.mnemonic} {j.op_str}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
