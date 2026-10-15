"""Read-only view of the eboot image dumped from the Ghidra project (research only).

The image is produced once by dump_image.py (run through the Ghidra run.py wrapper) into
native/build/eboot_image/{0,cc8000,da0000}.bin. Nothing here executes game code.

Name hash used by the engine for enum values, field names and type names (verified on the enum
dictionary at 0xa5ff70, see MISSION_FORMAT_RESEARCH.md): h = h * 101 + lower(c), 64-bit wrap.
Enum value keys are "<ENUM>_<Value>", e.g. "cid_MISSION".
"""
import json
import os
import re
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
IMAGE_DIR = os.environ.get('SSPC_EBOOT_IMAGE',
                           os.path.join(HERE, '..', '..', '..', 'build', 'eboot_image'))
M64 = (1 << 64) - 1
ENUM_TABLE = 0xa5ff70      # {u64 hash, u64 value} x 0x1bac, sorted as signed (eboot 0x2a6e40)
ENUM_COUNT = 0x1bac
FIELD_TABLE = 0xd057a0     # per type index: {i32 count, ptr -> field descriptor pointers} (0x4cfcb0)


def name_hash(s, h=0):
    for ch in s.lower().encode('latin1'):
        h = (h * 101 + ch) & M64
    return h


class Image:
    def __init__(self):
        self.segs = []
        for name in ('0', 'cc8000', 'da0000'):
            with open(os.path.join(IMAGE_DIR, name + '.bin'), 'rb') as f:
                self.segs.append((int(name, 16), f.read()))

    def read(self, a, n):
        for b, s in self.segs:
            if b <= a < b + len(s):
                return s[a - b:a - b + n]
        return None

    def u32(self, a): return struct.unpack('<I', self.read(a, 4))[0]
    def i32(self, a): return struct.unpack('<i', self.read(a, 4))[0]
    def u64(self, a): return struct.unpack('<Q', self.read(a, 8))[0]

    def cstr(self, a, limit=256):
        x = self.read(a, limit)
        if x is None:
            return None
        return x.split(b'\0')[0].decode('latin1')

    def strings(self, lo=0x900000, hi=0xb10000, minlen=1):
        text = self.segs[0][1]
        pat = rb'[\x20-\x7e]{%d,120}(?=\0)' % minlen
        return {m.start() + lo: m.group().decode('latin1') for m in re.finditer(pat, text[lo:hi])}

    def enum_table(self):
        return [(self.u64(ENUM_TABLE + i * 16), self.u64(ENUM_TABLE + i * 16 + 8))
                for i in range(ENUM_COUNT)]


def decompose(hashes, candidates, prefixes, seps=('_',)):
    """For each hash find (prefix, sep, suffix) with name_hash(prefix+sep+suffix) == hash.

    Uses linearity: H(p + s) = H(p) * 101^len(s) + H(s) (mod 2^64), 101 is invertible."""
    import numpy as np
    hs = np.array(sorted(set(hashes)), dtype=np.uint64)
    pre = {}
    for p in prefixes:
        for sp in seps:
            pre[name_hash(p + sp)] = (p, sp)
    parr = np.array(sorted(pre), dtype=np.uint64)
    hits = {}
    for s in candidates:
        inv = pow(pow(101, len(s), 1 << 64), -1, 1 << 64)
        ph = (hs - np.uint64(name_hash(s))) * np.uint64(inv)
        for i in np.nonzero(np.isin(ph, parr))[0]:
            p, sp = pre[int(ph[i])]
            hits.setdefault(int(hs[i]), set()).add((p, sp, s))
    return hits


def enums(img=None, cache=True):
    """{hash: (ENUM, Value, number)} for every decomposable entry of the enum dictionary."""
    path = os.path.join(IMAGE_DIR, 'enums.json')
    if cache and os.path.exists(path):
        with open(path, encoding='utf-8') as f:
            return {int(k): tuple(v) for k, v in json.load(f).items()}
    img = img or Image()
    tab = dict(img.enum_table())
    strs = sorted(set(img.strings().values()))
    hits = decompose(tab.keys(), strs, strs)
    out = {}
    for h, opts in hits.items():
        # prefer an exact-case candidate pair that exists as separate strings; pick shortest prefix
        p, sp, s = sorted(opts, key=lambda x: (len(x[0]), x[0], x[2]))[0]
        out[h] = (p, s, tab[h])
    if cache:
        with open(path, 'w', encoding='utf-8') as f:
            json.dump({str(k): v for k, v in out.items()}, f)
    return out
