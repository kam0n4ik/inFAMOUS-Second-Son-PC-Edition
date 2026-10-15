"""Lists the game objects of a KCAP package by the root catalog (block type 6) - research only.

Object header (measured on msn_vor.sprig / confirmed by the DIC fixups, see MISSION_FORMAT_RESEARCH.md):
  +0x10 u64  type index (a DIC fixup: the file stores 0, the record carries the type-name hash)
  +0x18 u64  OID (64-bit object id)
  +0x30 ptr  back pointer into the root catalog
usage: py -I -X utf8 objects.py <package-name-without-.xpps> [--types] [--grep TYPE]
"""
import collections
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(HERE, '..'), os.path.join(HERE, '..', 'anim')]
import ebootimg as E  # noqa: E402
from pkg import Pkg  # noqa: E402

_NAMES = None
_TAB = None


def type_names():
    global _NAMES, _TAB
    if _NAMES is None:
        img = E.Image()
        _NAMES = []
        for i in range(2449):
            p = img.u64(0xdac130 + 8 * i)
            _NAMES.append(img.cstr(p) if p else '')
        _TAB = dict(img.enum_table())
    return _NAMES, _TAB


def dic_values(p):
    """{location: (value, enum-name-or-None)} for every DIC fixup of the package."""
    names, tab = type_names()
    en = E.enums()
    out = {}
    for loc, h in p.dic:
        if h in en:
            out[loc] = (en[h][2], '%s_%s' % (en[h][0], en[h][1]))
        else:
            v = tab.get(h)
            out[loc] = (v, names[v] if v is not None and v < len(names) else None)
    return out


def catalog(p):
    root = [b for b in p.bl if b['type'] == 6]
    if not root:
        return []
    r = root[0]['offset']
    n = p.u32(r)
    objs = []
    dv = dic_values(p)
    for i in range(n):
        a = p.ptr(r + 8 + 8 * i)  # {u32 n, u32 n, n pointers}: block size 8 + 8n
        if a is None:
            continue
        t = dv.get(a + 0x10, (None, None))
        objs.append({'addr': a, 'type': t[1], 'tindex': t[0], 'oid': p.u64(a + 0x18)})
    return objs


def main():
    p = Pkg(sys.argv[1])
    objs = catalog(p)
    print('%s: %d objects' % (sys.argv[1], len(objs)))
    if '--types' in sys.argv:
        for t, c in collections.Counter(o['type'] for o in objs).most_common():
            print('  %5d %s' % (c, t))
    if '--grep' in sys.argv:
        want = sys.argv[sys.argv.index('--grep') + 1].upper()
        for o in objs:
            if o['type'] and want in o['type']:
                print('  %8x %-32s oid %016x' % (o['addr'], o['type'], o['oid']))


if __name__ == '__main__':
    main()
