"""Assembles the static scene of a world chunk package (a#/b#/c#/d#.xpps) for visual checking.

Hypothesis (a1.xpps): the chunk's scene object (root directory entry) holds {ptr, count} pairs
  +0xA8 instances   (8 bytes: u16 a, u16 matrix index, u16 geometry index << 2 | mode, u16 d)
  +0xB8 matrices    (48 bytes: 3x4 row-major, translation in the 4th column)
  +0xC8 geometries  (8-byte pointers to geometry records; null = provided by another package)

usage: py -I -X utf8 kcap_chunk_scene.py <chunk.xpps> <scene_object_offset_hex> <out.obj> [--lod A]
"""
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kcap  # noqa: E402
from kcap_placements import load  # noqa: E402


def main():
    path, obj, out = sys.argv[1], int(sys.argv[2], 16), sys.argv[3]
    want_a = int(sys.argv[sys.argv.index('--lod') + 1]) if '--lod' in sys.argv else None
    data, h, hs, segs, bl, ptr, vsh = load(path)
    inst_p, inst_n = ptr.get(obj + 0xa8), kcap.u32(data, hs + obj + 0xb0)
    mat_p, mat_n = ptr.get(obj + 0xb8), kcap.u32(data, hs + obj + 0xc0)
    geo_p, geo_n = ptr.get(obj + 0xc8), kcap.u32(data, hs + obj + 0xd0)
    print('instances %s x %d, matrices %s x %d, geometries %s x %d' % (
        hex(inst_p or 0), inst_n, hex(mat_p or 0), mat_n, hex(geo_p or 0), geo_n))
    stats = collections.Counter()
    verts_out, faces_out = [], []
    a_values = collections.Counter()
    for i in range(inst_n):
        a, m, c, d = struct.unpack_from('<4H', data, hs + inst_p + i * 8)
        a_values[a] += 1
        if want_a is not None and a != want_a:
            continue
        k = c >> 2
        if m >= mat_n or k >= geo_n:
            stats['index out of range'] += 1
            continue
        g = ptr.get(geo_p + k * 8)
        if g is None:
            stats['geometry in another package'] += 1
            continue
        vsa = ptr.get(g + 0x60)
        ib = ptr.get(g + 0x58)
        if vsa is None or ib is None:
            stats['not a geometry'] += 1
            continue
        d0, d1, d2, d3 = struct.unpack_from('<4I', data, hs + vsa)
        stride = (d1 >> 16) & 0x3fff
        dfmt = (d3 >> 15) & 0xf
        base = kcap.u32(data, hs + vsa)
        if dfmt != 12:
            stats['position format %d' % dfmt] += 1
            continue
        M = struct.unpack_from('<12f', data, hs + mat_p + m * 48)
        icount = kcap.u32(data, hs + g + 0x68)
        # Position dequantisation: centimetres = offset(+0x38, 3 floats) + scale(+0x44) * snorm.
        ox, oy, oz, sc = struct.unpack_from('<4f', data, hs + g + 0x38)
        first = len(verts_out) + 1
        for v in range(d2):
            x, y, z, _ = struct.unpack_from('<4h', data, hs + base + v * stride)
            x, y, z = ox + sc * x / 32767.0, oy + sc * y / 32767.0, oz + sc * z / 32767.0
            verts_out.append((M[0] * x + M[1] * y + M[2] * z + M[3],
                              M[4] * x + M[5] * y + M[6] * z + M[7],
                              M[8] * x + M[9] * y + M[10] * z + M[11]))
        for t in range(icount // 3):
            i0, i1, i2 = struct.unpack_from('<3H', data, hs + ib + t * 6)
            faces_out.append((first + i0, first + i1, first + i2))
        stats['drawn'] += 1
    print('field a distribution:', dict(sorted(a_values.items())))
    print('result:', dict(stats))
    xs = [v[0] for v in verts_out]
    ys = [v[1] for v in verts_out]
    zs = [v[2] for v in verts_out]
    if xs:
        print('bounds x %.1f..%.1f  y %.1f..%.1f  z %.1f..%.1f' % (min(xs), max(xs), min(ys), max(ys), min(zs), max(zs)))
    with open(out, 'w') as f:
        for v in verts_out:
            f.write('v %.4f %.4f %.4f\n' % v)
        for t in faces_out:
            f.write('f %d %d %d\n' % t)
    print('wrote %d vertices, %d triangles' % (len(verts_out), len(faces_out)))


if __name__ == '__main__':
    main()
