"""Annotated dump of a package object (research only).

Every 8-byte slot is printed with what the fixup streams say about it: KNLI pointer (target object type /
string), DIC value (enum name or type name), otherwise ints/floats.
usage: py -I -X utf8 dump.py <package> <hex-address | TYPE_NAME[#n]> [size]
"""
import bisect
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(HERE, '..'), os.path.join(HERE, '..', 'anim')]
import objects as O  # noqa: E402
from pkg import Pkg  # noqa: E402


class Annotator:
    def __init__(self, p):
        self.p = p
        self.objs = O.catalog(p)
        self.by_addr = {o['addr']: o for o in self.objs}
        self.addrs = sorted(self.by_addr)
        self.dic = O.dic_values(p)

    def owner(self, a):
        i = bisect.bisect_right(self.addrs, a) - 1
        if i < 0:
            return None
        return self.by_addr[self.addrs[i]]

    def object_size(self, a):
        i = bisect.bisect_right(self.addrs, a)
        bi, b = self.p.block_of(a)
        end = b['offset'] + b['size']
        if i < len(self.addrs):
            end = min(end, self.addrs[i])
        return end - a

    def string_at(self, a):
        raw = self.p.d[self.p.hs + a:self.p.hs + a + 160]
        s = raw.split(b'\0')[0]
        if len(s) >= 2 and re.fullmatch(rb'[\x20-\x7e]+', s):
            return s.decode()
        return None

    def describe_target(self, t):
        if t in self.by_addr:
            o = self.by_addr[t]
            return 'OBJ %s oid=%016x' % (o['type'], o['oid'])
        s = self.string_at(t)
        if s:
            return 'STR %r' % s
        o = self.owner(t)
        if o:
            return 'in %s@%x+%#x' % (o['type'], o['addr'], t - o['addr'])
        return ''

    def lines(self, a, size):
        p = self.p
        out = []
        for o in range(a, a + size, 8):
            raw = p.d[p.hs + o:p.hs + o + 8]
            if len(raw) < 8:
                break
            q = struct.unpack('<Q', raw)[0]
            lo, hi = struct.unpack('<II', raw)
            f0, f1 = struct.unpack('<ff', raw)
            note = ''
            if o in p.linkset:
                kind = p.linkset[o]
                note = 'PTR(k%d) -> %#x %s' % (kind, lo, self.describe_target(lo) if kind in (0, 3) else '')
            elif o in self.dic:
                v, n = self.dic[o]
                note = 'DIC %s = %s' % (n, v)
            elif o + 4 in p.linkset:
                note = 'lo=%d  PTR@+4' % lo
            else:
                parts = []
                if q == 0:
                    parts.append('0')
                else:
                    for v, f in ((lo, f0), (hi, f1)):
                        if v == 0:
                            parts.append('0')
                        elif 1e-4 < abs(f) < 1e6 and (v & 0x7f800000) != 0x7f800000:
                            parts.append('%g' % f)
                        elif v == 0xffffffff:
                            parts.append('-1')
                        else:
                            parts.append('%#x' % v)
                note = ' '.join(parts)
            out.append('  +%04x %8x  %s  %s' % (o - a, o, raw.hex(' '), note))
        return out


def find(ann, spec):
    if re.fullmatch(r'[0-9a-fA-F]+', spec):
        return int(spec, 16)
    name, _, nth = spec.partition('#')
    hits = [o for o in ann.objs if o['type'] == name]
    return hits[int(nth or 0)]['addr']


def main():
    p = Pkg(sys.argv[1])
    ann = Annotator(p)
    a = find(ann, sys.argv[2])
    size = int(sys.argv[3], 0) if len(sys.argv) > 3 else min(ann.object_size(a), 0x400)
    o = ann.by_addr.get(a)
    print('object %x %s size %#x' % (a, o['type'] if o else '?', ann.object_size(a)))
    print('\n'.join(ann.lines(a, size)))


if __name__ == '__main__':
    main()
