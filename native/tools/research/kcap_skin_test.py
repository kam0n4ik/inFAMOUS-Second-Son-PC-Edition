"""Tests skin-weight hypotheses on hero.xpps by bending one joint and measuring edge stretch.

Skeleton (hero.xpps): 653 joints, bind (model space, row-major 4x4, translation in row 3, cm) at
0xb6b0, parents (int16) at 0x1e684. Vertex streams: +8 sint16x4 joint indices, +16 unorm8x4 weights.

H1: weights[0..2] belong to joints[0..2], joint[3] gets 255 - sum.
H2: weights[0..3] as stored, normalised by their sum (no implicit weight).
H3: joint[0] gets 255 - sum(weights[0..2]), weights[0..2] belong to joints[1..3].

usage: py -I -X utf8 kcap_skin_test.py <hero.xpps> <joint> <degrees>
"""
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kcap_placements import load  # noqa: E402

N = 653


def rot_about(p, axis, deg):
    a = np.radians(deg)
    axis = np.asarray(axis, float) / np.linalg.norm(axis)
    x, y, z = axis
    c, s = np.cos(a), np.sin(a)
    R = np.array([[c + x * x * (1 - c), x * y * (1 - c) - z * s, x * z * (1 - c) + y * s],
                  [y * x * (1 - c) + z * s, c + y * y * (1 - c), y * z * (1 - c) - x * s],
                  [z * x * (1 - c) - y * s, z * y * (1 - c) + x * s, c + z * z * (1 - c)]])
    A = np.eye(4)
    A[:3, :3] = R
    A[:3, 3] = p - R @ p
    return A


def main():
    path, joint, deg = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
    data, h, hs, segs, bl, ptr, vsh = load(path)
    W = np.frombuffer(data, np.float32, N * 16, hs + 0xb6b0).reshape(N, 4, 4)
    P = np.frombuffer(data, np.int16, N, hs + 0x1e684)
    pos = W[:, 3, :3].astype(float)
    sub = {joint}
    for i in range(N):
        if P[i] in sub:
            sub.add(i)
    A = rot_about(pos[joint], [0, 1, 0], deg)
    joint_xform = np.array([A if i in sub else np.eye(4) for i in range(N)])
    results = {}
    for i in range(41):
        g = struct.unpack_from('<I', data, hs + 0xd70 + i * 8)[0]
        vsa = ptr.get(g + 0x60)
        n = data[hs + g + 0x6c]
        st = []
        for k in range(n):
            d1, d2, d3 = struct.unpack_from('<3I', data, hs + vsa + k * 16 + 4)
            st.append((struct.unpack_from('<I', data, hs + vsa + k * 16)[0], (d1 >> 16) & 0x3fff, d2, (d3 >> 15) & 0xf, (d3 >> 12) & 7))
        pos_s = st[0]
        ji = [s for s in st if s[3] == 12 and s[4] == 5]
        wi = [s for s in st if s[3] == 10 and s[4] == 0 and s[0] == ji[0][0] + 8] if ji else []
        if not ji or not wi:
            continue
        cnt = pos_s[2]
        J = np.array([struct.unpack_from('<4h', data, hs + ji[0][0] + v * ji[0][1]) for v in range(cnt)])
        if not np.isin(J, list(sub)).any():
            continue
        Wt = np.array([struct.unpack_from('<4B', data, hs + wi[0][0] + v * wi[0][1]) for v in range(cnt)], float)
        ox, oy, oz, sc = struct.unpack_from('<4f', data, hs + g + 0x38)
        V = np.array([struct.unpack_from('<3h', data, hs + pos_s[0] + v * pos_s[1]) for v in range(cnt)], float)
        V = np.array([ox, oy, oz]) + sc * V / 32767.0
        icount = struct.unpack_from('<I', data, hs + g + 0x68)[0]
        ib = ptr[g + 0x58]
        I = np.frombuffer(data, np.uint16, icount, hs + ib).reshape(-1, 3)
        hyps = {}
        rest = 255 - Wt[:, :3].sum(1)
        hyps['H1'] = (J, np.stack([Wt[:, 0], Wt[:, 1], Wt[:, 2], rest], 1))
        s4 = np.maximum(Wt.sum(1, keepdims=True), 1)
        hyps['H2'] = (J, Wt / s4 * 255)
        hyps['H3'] = (J, np.stack([rest, Wt[:, 0], Wt[:, 1], Wt[:, 2]], 1))
        Vh = np.c_[V, np.ones(len(V))]
        for name, (jj, ww) in hyps.items():
            ww = ww / 255.0
            out = np.zeros_like(V)
            for k in range(4):
                Ak = joint_xform[jj[:, k]]  # (n,4,4) column-vector convention: A @ v
                out += ww[:, k:k + 1] * np.einsum('nij,nj->ni', Ak, Vh)[:, :3]
            e0 = np.linalg.norm(V[I[:, 0]] - V[I[:, 1]], axis=1) + 1e-6
            e1 = np.linalg.norm(out[I[:, 0]] - out[I[:, 1]], axis=1)
            stretch = np.abs(e1 / e0 - 1)
            results.setdefault(name, []).append((i, float(np.percentile(stretch, 99)), float(stretch.max())))
    for name, lst in results.items():
        print(name, ' '.join('part%d p99 %.3f max %.2f' % x for x in lst))


if __name__ == '__main__':
    main()
