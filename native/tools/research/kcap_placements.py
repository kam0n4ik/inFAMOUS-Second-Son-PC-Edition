"""Looks for object placements in a KCAP package: affine 4x4 matrices near pointers to model
descriptions (objects whose +0x58 points to a list of geometry records, count at +0x60).

usage: py -I -X utf8 kcap_placements.py <file.xpps> [--max N]
"""
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kcap  # noqa: E402


def load(path):
    data = open(path, 'rb').read()
    h = kcap.parse(data)
    hs = h['header_size']
    segs = kcap.segments(data, h)
    bl = kcap.blocks(data, h)
    links = []
    for s in segs:
        if s['pool'] != 2:
            continue
        for b in bl[s['first_block']:s['first_block'] + s['block_count']]:
            if b['type'] != 2 or b['size'] == 0:
                continue
            st = hs + b['offset']
            for tag, p, size in kcap.chunks(data, st, st + b['size']):
                if tag == kcap.TAG_KNLI:
                    links += kcap.knli(data, p)
    ptr = {loc: kcap.u32(data, hs + loc) for k, loc in links if k in (0, 3)}
    vsh = {loc for k, loc in links if k == 2}
    return data, h, hs, segs, bl, ptr, vsh


def is_geometry(ptr, vsh, g):
    return ptr.get(g + 0x60) in vsh and (g + 0x58) in ptr


def model_defs(data, hs, ptr, vsh):
    out = {}
    for loc, target in ptr.items():
        if loc % 8 or (loc - 0x58) < 0:
            continue
        base = loc - 0x58
        if loc != base + 0x58:
            continue
        count = kcap.u32(data, hs + base + 0x60)
        if not (0 < count < 4096):
            continue
        first = ptr.get(target)
        if first is not None and is_geometry(ptr, vsh, first):
            out[base] = (target, count)
    return out


def affine_at(data, o):
    m = struct.unpack_from('<16f', data, o)
    if not all(math.isfinite(x) for x in m):
        return None
    if abs(m[3]) > 1e-5 or abs(m[7]) > 1e-5 or abs(m[11]) > 1e-5 or abs(m[15] - 1) > 1e-5:
        return None
    rows = [m[0:3], m[4:7], m[8:11]]
    norms = [math.sqrt(sum(c * c for c in r)) for r in rows]
    if min(norms) < 1e-3 or max(norms) / min(norms) > 1.01:
        return None
    for i in range(3):
        for j in range(i + 1, 3):
            d = sum(a * b for a, b in zip(rows[i], rows[j])) / (norms[i] * norms[j])
            if abs(d) > 1e-3:
                return None
    return norms[0], m[12:15]


def basis_at(data, o, row_stride):
    """3x3 rotation*uniform-scale with rows `row_stride` bytes apart; returns (scale, o) or None."""
    rows = [struct.unpack_from('<3f', data, o + i * row_stride) for i in range(3)]
    if not all(math.isfinite(x) for r in rows for x in r):
        return None
    norms = [math.sqrt(sum(c * c for c in r)) for r in rows]
    if min(norms) < 1e-2 or max(norms) > 1e4 or max(norms) / min(norms) > 1.01:
        return None
    for i in range(3):
        for j in range(i + 1, 3):
            if abs(sum(a * b for a, b in zip(rows[i], rows[j])) / (norms[i] * norms[j])) > 1e-3:
                return None
    # Reject the identity-like trivial matrices made of exact 0/1 only when scale is 1 (common
    # default data); keep them counted separately.
    return norms[0]


def scan_bases(data, hs, segs):
    found = {12: [], 16: []}
    for s in segs:
        if s['pool'] != 0:
            continue
        for o in range(s['offset'], s['offset'] + s['size'] - 48, 4):
            for stride in (12, 16):
                sc = basis_at(data, hs + o, stride)
                if sc is not None:
                    found[stride].append((o, sc))
    return found


def main():
    path = sys.argv[1]
    if '--bases' in sys.argv:
        data, h, hs, segs, bl, ptr, vsh = load(path)
        found = scan_bases(data, hs, segs)
        for stride, lst in found.items():
            nontrivial = [(o, sc) for o, sc in lst if abs(sc - 1) > 1e-4 or
                          any(abs(x) not in (0.0, 1.0) for x in struct.unpack_from('<3f', data, hs + o))]
            print('row stride %d: %d bases, %d non-trivial' % (stride, len(lst), len(nontrivial)))
            for o, sc in nontrivial[:12]:
                after = struct.unpack_from('<6f', data, hs + o + 3 * stride)
                print('   @%#x scale %.3f  next floats %s' % (o, sc, ' '.join('%.2f' % x for x in after)))
        return
    mx = int(sys.argv[sys.argv.index('--max') + 1]) if '--max' in sys.argv else 30
    data, h, hs, segs, bl, ptr, vsh = load(path)
    defs = model_defs(data, hs, ptr, vsh)
    print('model descriptions: %d' % len(defs))
    mats = []
    for s in segs:
        if s['pool'] != 0:
            continue
        for o in range(s['offset'], s['offset'] + s['size'] - 64, 16):
            r = affine_at(data, hs + o)
            if r:
                mats.append((o, r))
    print('affine matrices (16-byte aligned): %d' % len(mats))
    # Matrices whose object also points (within -0x100..+0x100) to a model description.
    shown = 0
    for o, (scale, t) in mats:
        near = [(loc, tgt) for loc in range(o - 0x100, o + 0x100, 8) for tgt in [ptr.get(loc)] if tgt in defs]
        if near and shown < mx:
            shown += 1
            print('  matrix @%#x scale %.3f pos (%.2f %.2f %.2f) -> model %s' % (
                o, scale, t[0], t[1], t[2], ', '.join('%#x@%+#x' % (tgt, loc - o) for loc, tgt in near)))
    scales = sorted(r[0] for _, r in mats)
    if scales:
        print('scale range %.3f .. %.3f, median %.3f' % (scales[0], scales[-1], scales[len(scales) // 2]))


if __name__ == '__main__':
    main()
