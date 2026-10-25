# R11 neon (docs/R11_NEON.md): what a GCN (Sea Islands) program does with GDS - every M0 write (GDS/LDS base for the
# DS instructions after it) and every DS instruction that reaches GDS (ds_append / ds_consume / ds_ordered_count /
# ds_gws_*, or any DS op with the GDS bit), in program order. Input: the .gcn files SSPC_DIAG_DUMP_SHADER writes.
#   python -I tools/r11/gds_scan.py <file.gcn | dir> [...]
import os
import struct
import sys

DS_NAMES = {61: "ds_consume", 62: "ds_append", 63: "ds_ordered_count", 25: "ds_gws_init", 26: "ds_gws_sema_v",
            27: "ds_gws_sema_br", 28: "ds_gws_sema_p", 29: "ds_gws_barrier"}
M0 = 124


def sreg(v):
    if v < 104:
        return f"s{v}"
    return {106: "vcc_lo", 107: "vcc_hi", 124: "m0", 126: "exec_lo", 127: "exec_hi", 255: "lit"}.get(v, f"src{v}")


def scan(words):
    i, out = 0, []
    while i < len(words):
        w = words[i]
        size, lit = 1, None
        top9, top7, top6 = w >> 23, w >> 25, w >> 26
        if top9 == 0x17F:  # SOPP
            if ((w >> 16) & 0x7F) in (1,):  # s_endpgm
                out.append((i, "s_endpgm"))
                break
        elif top9 in (0x17D, 0x17E):  # SOP1 / SOPC
            src = w & 0xFF
            if src == 255:
                size, lit = 2, words[i + 1] if i + 1 < len(words) else 0
            if top9 == 0x17D and ((w >> 16) & 0x7F) == M0:
                op = (w >> 8) & 0xFF
                out.append((i, f"sop1 op{op} m0 <- {sreg(src)}" + (f" {lit:#x} (base {lit >> 16:#x})" if lit is not None else "")))
        elif (w >> 28) == 0xB:  # SOPK
            op = (w >> 23) & 0x1F
            if op == 21:
                size = 2
            if ((w >> 16) & 0x7F) == M0:
                out.append((i, f"sopk op{op} m0 <- {w & 0xFFFF:#x}"))
        elif (w >> 30) == 2:  # SOP2
            if (w & 0xFF) == 255 or ((w >> 8) & 0xFF) == 255:
                size, lit = 2, words[i + 1] if i + 1 < len(words) else 0
            if ((w >> 16) & 0x7F) == M0:
                out.append((i, f"sop2 op{(w >> 23) & 0x7F} m0 <- {sreg(w & 0xFF)}, {sreg((w >> 8) & 0xFF)}" +
                            (f" lit {lit:#x}" if lit is not None else "")))
        elif (w >> 27) == 0x18:  # SMRD
            if ((w >> 8) & 1) == 0 and (w & 0xFF) == 255:
                size = 2
        elif top6 in (0x34, 0x36, 0x37, 0x38, 0x3A, 0x3C, 0x3E):  # VOP3 DS FLAT MUBUF MTBUF MIMG EXP
            size = 2
            if top6 == 0x36:
                op, gds = (w >> 18) & 0xFF, (w >> 17) & 1
                if op in DS_NAMES or gds:
                    off = ((w >> 8) & 0xFF) << 8 | (w & 0xFF)
                    w1 = words[i + 1] if i + 1 < len(words) else 0
                    out.append((i, f"{DS_NAMES.get(op, f'ds op{op}')} gds={gds} offset {off:#x} addr v{w1 & 0xFF} "
                                   f"data v{(w1 >> 8) & 0xFF} vdst v{w1 >> 24}"))
        elif top6 == 0x32:  # VINTRP
            pass
        elif top7 in (0x3F, 0x3E) or (w >> 31) == 0:  # VOP1 / VOPC / VOP2
            if (w & 0x1FF) == 255:
                size = 2
        i += size
    return out


def main():
    files = []
    for a in sys.argv[1:]:
        if os.path.isdir(a):
            files += [os.path.join(a, f) for f in sorted(os.listdir(a)) if f.endswith(".gcn")]
        else:
            files.append(a)
    for f in files:
        data = open(f, "rb").read()
        words = list(struct.unpack(f"<{len(data) // 4}I", data[: len(data) // 4 * 4]))
        res = scan(words)
        uses = [r for r in res if r[1].startswith("ds") or "gds=1" in r[1]]
        print(f"== {os.path.basename(f)}: {len(words)} words, GDS instructions {len(uses)}")
        for off, text in res:
            if text != "s_endpgm":
                print(f"  {off * 4:6x}: {text}")


if __name__ == "__main__":
    main()
