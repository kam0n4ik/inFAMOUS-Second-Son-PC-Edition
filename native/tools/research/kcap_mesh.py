"""Find geometry records in a KCAP package and export them for visual checking.

Hypothesis under test (from eboot DrawSingle 0x1a1960, docs/native-reconstruction):
  geom+0x58 index buffer pointer (16-bit indices, triangle list)
  geom+0x60 pointer to V# array, geom+0x68 index count, geom+0x6C vertex-buffer count
In a package the pointers are internal links (KNLI kind 0/3) and every V# is a kind-2 link.

usage: py -I -X utf8 kcap_mesh.py <file.xpps> [--obj OUTDIR] [--max N]
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kcap  # noqa: E402


def main():
    path = sys.argv[1]
    data = open(path, 'rb').read()
    h = kcap.parse(data)
    hs = h['header_size']
    segs = kcap.segments(data, h)
    links = []
    for s in segs:
        if s['pool'] != 2:
            continue
        st = hs + s['offset']
        for tag, p, size in kcap.chunks(data, st, st + s['size']):
            if tag == kcap.TAG_KNLI:
                links = kcap.knli(data, p)
    vsharp = {loc for k, loc in links if k == 2}
    ptr = {}
    for k, loc in links:
        if k in (0, 3):
            ptr[loc] = kcap.u32(data, hs + loc)

    def seg_of(off):
        for i, s in enumerate(segs):
            if s['offset'] <= off < s['offset'] + s['size']:
                return i
        return -1

    geoms = []
    for loc, target in ptr.items():
        if target not in vsharp:
            continue
        g = loc - 0x60
        ib = ptr.get(g + 0x58)
        if ib is None:
            continue
        icount = kcap.u32(data, hs + g + 0x68)
        nvb = kcap.u32(data, hs + g + 0x6c)
        geoms.append((g, ib, target, icount, nvb))
    geoms.sort()
    print('geometry candidates: %d' % len(geoms))
    outdir = sys.argv[sys.argv.index('--obj') + 1] if '--obj' in sys.argv else None
    mx = int(sys.argv[sys.argv.index('--max') + 1]) if '--max' in sys.argv else 20
    for gi, (g, ib, vsa, icount, nvb) in enumerate(geoms[:mx]):
        print('geom @%#x  ib=%#x(seg%d) icount=%d  vb@%#x nvb=%#x  | %s' % (
            g, ib, seg_of(ib), icount, vsa, nvb, kcap.hexrow(data, hs + g, 16)))
        streams = []
        for i in range(min(nvb & 0xffff, 16)):
            d0, d1, d2, d3 = struct.unpack_from('<4I', data, hs + vsa + i * 16)
            stride = (d1 >> 16) & 0x3fff
            dfmt = (d3 >> 15) & 0xf
            nfmt = (d3 >> 12) & 0x7
            streams.append((d0, stride, d2, dfmt, nfmt))
            print('    vb%d base=%#x(seg%d) stride=%d records=%d dfmt=%d nfmt=%d linked=%s' % (
                i, d0, seg_of(d0), stride, d2, dfmt, nfmt, (vsa + i * 16) in vsharp))
        if outdir and streams and icount >= 3:
            export_obj(data, hs, os.path.join(outdir, 'geom_%05x.obj' % g), streams, ib, icount, g)


def export_obj(data, hs, out, streams, ib, icount, g):
    # Confirmed on hero.xpps: geom+0x20 AABB min (3 floats), geom+0x2C AABB max (3 floats), in the
    # same units as the positions; 16_16_16_16 snorm positions are raw/32767 (min/max match exactly).
    cx = cy = cz = 0.0
    ex = ey = ez = 1.0
    # Position stream hypothesis: first stream with a float 3/4-component format.
    pos = None
    for base, stride, records, dfmt, nfmt in streams:
        if nfmt == 7 and dfmt in (13, 14):
            pos = (base, stride, records, 12)
            break
        if dfmt == 12 and nfmt in (1, 6):   # 16_16_16_16 snorm
            pos = (base, stride, records, 'snorm16')
            break
    if pos is None:
        return
    base, stride, records, kind = pos
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'w') as f:
        n = records if stride else 0
        for i in range(n):
            o = hs + base + i * stride
            if kind == 'snorm16':
                x, y, z, _ = struct.unpack_from('<4h', data, o)
                f.write('v %f %f %f\n' % (cx + ex * max(x / 32767.0, -1.0),
                                          cy + ey * max(y / 32767.0, -1.0),
                                          cz + ez * max(z / 32767.0, -1.0)))
            else:
                x, y, z = struct.unpack_from('<3f', data, o)
                f.write('v %f %f %f\n' % (x, y, z))
        for t in range(icount // 3):
            a, b, c = struct.unpack_from('<3H', data, hs + ib + t * 6)
            if a < n and b < n and c < n:
                f.write('f %d %d %d\n' % (a + 1, b + 1, c + 1))


if __name__ == '__main__':
    main()
