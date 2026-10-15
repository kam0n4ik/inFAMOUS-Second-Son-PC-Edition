"""PLPS chunk = link records of compiled splice (script) code - research only.

Layout from the eboot linker 0x30b1c0 (payload = u64 words q[]):
  q[0] lo32 n1 (member refs), hi32 n2 (method calls); q[1] lo32 n3 (global function calls)
  member ref  (0x18): {u32 loc, u32 0, u64 class-type hash, u64 member-name hash}
  method call (0x30): {u32 loc, u32 args_off, u64 class-type hash, u64 member hash, u64 ret kind, u64 ret type,
                       u8 argc}
  global call (0x28): {u32 loc, u32 args_off, u64 function hash (table 0xda1790), u64 ret kind, u64 ret type... u8 argc}
  args area (0x18 per argument) starts after the three tables; args_off is relative to it.
The byte at loc is a splice opcode (0x0e/0x0f/0x10 property get/set, 0x14/0x15 field, rewritten to direct
accessors; method call -> 0x1c, global call -> 0x1f).
usage: py -I -X utf8 plps.py <package> [--summary]
"""
import collections
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(HERE, '..'), os.path.join(HERE, '..', 'anim')]
import ebootimg as E  # noqa: E402
import kcap  # noqa: E402
import objects as O  # noqa: E402
from pkg import Pkg  # noqa: E402

_MEMBER = None
_FUNCS = None


def member_names():
    global _MEMBER, _FUNCS
    if _MEMBER is None:
        img = E.Image()
        strs = set(img.strings(0x900000, 0xb10000, 1).values())
        _MEMBER = {E.name_hash(s): s for s in strs}
        _FUNCS = {}
        for i in range(0x29d):
            a = 0xda1790 + i * 0x20
            _FUNCS[img.u64(a)] = i
    return _MEMBER, _FUNCS


def parse(p):
    names, tab = O.type_names()
    mem, funcs = member_names()
    out = {'member': [], 'method': [], 'global': []}
    for b in p.bl:
        if b['type'] != 2:
            continue
        st = p.hs + b['offset']
        for tag, pl, size in kcap.chunks(p.d, st, st + b['size']):
            if tag != kcap.TAG_PLPS:
                continue
            n1, n2 = struct.unpack_from('<II', p.d, pl)
            n3 = struct.unpack_from('<I', p.d, pl + 8)[0]
            o = pl + 0x10

            def tname(h):
                v = tab.get(h)
                return names[v] if v is not None and v < len(names) else '%016x' % h
            for i in range(n1):
                loc, _, th, mh = struct.unpack_from('<IIQQ', p.d, o)
                out['member'].append({'loc': loc, 'cls': tname(th), 'name': mem.get(mh, '%016x' % mh),
                                      'op': p.d[p.hs + loc] if p.hs + loc < len(p.d) else None})
                o += 0x18
            for i in range(n2):
                loc, aoff, th, mh, rk, rt, argc = struct.unpack_from('<IIQQQQQ', p.d, o)
                out['method'].append({'loc': loc, 'cls': tname(th), 'name': mem.get(mh, '%016x' % mh),
                                      'argc': argc & 0xff})
                o += 0x30
            for i in range(n3):
                loc, aoff, fh, rk, rt = struct.unpack_from('<IIQQQ', p.d, o)
                argc = p.d[o + 0x20]
                out['global'].append({'loc': loc, 'fn': mem.get(fh, 'fn#%s' % funcs.get(fh, '%016x' % fh)),
                                      'argc': argc})
                o += 0x28
    return out


def main():
    p = Pkg(sys.argv[1])
    r = parse(p)
    print('member refs %d, method calls %d, global calls %d' % (
        len(r['member']), len(r['method']), len(r['global'])))
    if '--summary' in sys.argv:
        c = collections.Counter('%s.%s' % (x['cls'], x['name']) for x in r['member'] + r['method'])
        for k, v in c.most_common(80):
            print('  %4d %s' % (v, k))
        c = collections.Counter(x['fn'] for x in r['global'])
        print('globals:', ', '.join('%s:%d' % kv for kv in c.most_common(60)))


if __name__ == '__main__':
    main()
