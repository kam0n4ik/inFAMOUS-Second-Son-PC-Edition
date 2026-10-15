import struct
from pkg import Pkg
from anim_clip import Clip, track_curve
from anim_math import compose
_hero = None
def hero_rig():
    global _hero
    if _hero is None:
        h = Pkg('hero')
        s = 0xf80; n = 653
        pose = s + 0x18 + h.u32(s + 0x18)
        q = [struct.unpack_from('<4f', h.d, h.hs + pose + j * 0x30) for j in range(n)]
        t = [struct.unpack_from('<3f', h.d, h.hs + pose + j * 0x30 + 16) for j in range(n)]
        par = list(struct.unpack_from('<%dh' % n, h.d, h.hs + 0x1e684))
        _hero = (q, t, par)
    return _hero

def clip_poses(c, joints=413):
    """Local (q, t) per joint for every integer frame."""
    q0, t0, par = hero_rig()
    N = c.keys
    rot = [[q0[j]] * N for j in range(joints)]
    tr = [[t0[j]] * N for j in range(joints)]
    for j, v in c.const_rot().items():
        rot[j] = [v] * N
    for j, v in c.const_vec(1).items():
        tr[j] = [v] * N
    for k, j in enumerate(c.lists[4]):
        rot[j] = track_curve(c, 0, k)
    for k, j in enumerate(c.lists[5]):
        tr[j] = track_curve(c, 1, k)
    frames = []
    for f in range(N):
        frames.append(compose([(rot[j][f], tr[j][f]) for j in range(joints)], par[:joints]))
    return frames
