import struct, math
from hkpack import Packfile

def decode_cms(d, st, o):
    """hknpCompressedMeshShape at data-section offset o -> list of triangles (metres, world)."""
    lf_ptr = lambda off: None
    def arr(field):
        return struct.unpack_from('<Q', d, st + o + field)[0], struct.unpack_from('<I', d, st + o + field + 8)[0]
    return None

def triangles(pf, di, o):
    d = pf.d; st = pf.sec_abs(di); lf = pf.local_fixups(di)
    def arr(field):
        return lf.get(o + field), struct.unpack_from('<I', d, st + o + field + 8)[0]
    dmin = struct.unpack_from('<3f', d, st + o + 0xa0); dmax = struct.unpack_from('<3f', d, st + o + 0xb0)
    sec_p, nsec = arr(0xd0); prim_p, nprim = arr(0xe0); sidx_p, nsidx = arr(0xf0)
    pv_p, npv = arr(0x100); sv_p, nsv = arr(0x110)
    sv_scale = [(dmax[k] - dmin[k]) / ((1 << (21 if k < 2 else 22)) - 1) for k in range(3)]
    def shared(i):
        v = struct.unpack_from('<Q', d, st + sv_p + 8 * i)[0]
        q = (v & 0x1fffff, (v >> 21) & 0x1fffff, v >> 42)
        return tuple(dmin[k] + q[k] * sv_scale[k] for k in range(3))
    tris = []
    for s in range(nsec):
        so = sec_p + 0x60 * s
        cp = struct.unpack_from('<6f', d, st + so + 0x30)
        fpv, svd, prd, drd = struct.unpack_from('<4I', d, st + so + 0x48)
        npk = svd & 0xff; fsh = svd >> 8
        def vert(i):
            if i < npk:
                v = struct.unpack_from('<I', d, st + pv_p + 4 * (fpv + i))[0]
                q = (v & 0x7ff, (v >> 11) & 0x7ff, v >> 22)
                return tuple(cp[k] + q[k] * cp[3 + k] for k in range(3))
            si = struct.unpack_from('<H', d, st + sidx_p + 2 * (fsh + i - npk))[0]
            return shared(si)
        for p in range(prd >> 8, (prd >> 8) + (prd & 0xff)):
            a, b, c, dd = d[st + prim_p + 4 * p: st + prim_p + 4 * p + 4]
            tris.append((vert(a), vert(b), vert(c)))
            if c != dd:
                tris.append((vert(a), vert(c), vert(dd)))
    return tris
