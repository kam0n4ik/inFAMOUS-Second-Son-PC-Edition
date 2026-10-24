"""R10 sysrt: instructions of a game module that address some module-relative data addresses rip-relatively
(read-only; the module is data, not instructions).

    python -I tools/r10/data_refs.py <module> <hex addr> [<hex addr> ...] [--before 6] [--after 6] [--dump 0x9fc60:32]
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
    ap.add_argument("addrs", nargs="*")
    ap.add_argument("--before", type=int, default=6)
    ap.add_argument("--after", type=int, default=6)
    ap.add_argument("--dump", action="append", default=[])
    a = ap.parse_args()
    elf = Ps4Elf(a.module)
    mod = elf.parse_module()
    img, _ = elf.build_image()
    for d in a.dump:
        addr, size = d.split(":")
        addr = int(addr, 16)
        print(f"{addr:#x}: {bytes(img[addr:addr + int(size)]).hex(' ')}")
    targets = {int(x, 16) for x in a.addrs}
    if not targets:
        return 0
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    insns = []
    for p in (p for p in mod.phdrs if p.p_type == PT_LOAD and (p.p_flags & 1)):
        off, end = p.p_vaddr, p.p_vaddr + p.p_filesz
        while off < end:
            got = False
            for i in md.disasm(bytes(img[off:end]), off):
                insns.append(i)
                off = i.address + i.size
                got = True
            if not got or off < end:
                off += 1
    for n, i in enumerate(insns):
        s = i.op_str
        k = s.find("[rip ")
        if k < 0:
            continue
        expr = s[k + 5:s.find("]", k)].replace(" ", "")
        try:
            t = i.address + i.size + int(expr[1:], 16) * (-1 if expr[0] == "-" else 1)
        except ValueError:
            continue
        if t in targets:
            print(f"\n=== {i.address:#x} -> {t:#x}")
            for j in insns[max(0, n - a.before):n + a.after + 1]:
                print(f"{'>>' if j.address == i.address else '  '} {j.address:#08x}  {j.mnemonic} {j.op_str}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
