"""Surveys texture descriptors (T#) in a KCAP package.

Hypothesis: KNLI kind-4 links (u32 = address >> 8) patch T# dword0 (BASE_ADDRESS[39:8]).
T# fields (AMD GCN3 image resource): dw1 DATA_FORMAT[25:20] NUM_FORMAT[29:26];
dw2 WIDTH-1[13:0] HEIGHT-1[27:14]; dw3 DST_SEL[11:0] BASE_LEVEL[15:12] LAST_LEVEL[19:16]
TILING_INDEX[24:20] TYPE[31:28]; dw4 DEPTH-1[12:0] PITCH-1[26:13].

usage: py -I -X utf8 kcap_textures.py <file.xpps> [--list N]
"""
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kcap  # noqa: E402
from kcap_placements import load  # noqa: E402

DFMT = {1: '8', 2: '16', 3: '8_8', 4: '32', 5: '16_16', 6: '10_11_11', 7: '11_11_10', 8: '10_10_10_2',
        9: '2_10_10_10', 10: '8_8_8_8', 11: '32_32', 12: '16_16_16_16', 13: '32_32_32', 14: '32_32_32_32',
        16: '5_6_5', 17: '1_5_5_5', 18: '5_5_5_1', 19: '4_4_4_4', 20: '8_24', 21: '24_8', 22: 'X24_8_32',
        34: '5_9_9_9', 35: 'BC1', 36: 'BC2', 37: 'BC3', 38: 'BC4', 39: 'BC5', 40: 'BC6', 41: 'BC7'}
NFMT = {0: 'unorm', 1: 'snorm', 4: 'uint', 5: 'sint', 7: 'float', 9: 'srgb'}


def links_all(path):
    data, h, hs, segs, bl, ptr, vsh = load(path)
    # Re-read the KNLI table to get kind-4 locations (load() only keeps pointers and V#).
    locs = []
    for s in segs:
        if s['pool'] != 2:
            continue
        for b in bl[s['first_block']:s['first_block'] + s['block_count']]:
            if b['type'] != 2 or b['size'] == 0:
                continue
            st = hs + b['offset']
            for tag, p, size in kcap.chunks(data, st, st + b['size']):
                if tag == kcap.TAG_KNLI:
                    locs += [loc for k, loc in kcap.knli(data, p) if k == 4]
    return data, h, hs, segs, locs


def decode(data, o):
    dw = struct.unpack_from('<8I', data, o)
    return {
        'dfmt': (dw[1] >> 20) & 0x3f, 'nfmt': (dw[1] >> 26) & 0xf,
        'w': (dw[2] & 0x3fff) + 1, 'h': ((dw[2] >> 14) & 0x3fff) + 1,
        'base_level': (dw[3] >> 12) & 0xf, 'last_level': (dw[3] >> 16) & 0xf,
        'tiling': (dw[3] >> 20) & 0x1f, 'type': (dw[3] >> 28) & 0xf,
        'depth': (dw[4] & 0x1fff) + 1, 'pitch': ((dw[4] >> 13) & 0x3fff) + 1,
        'dst_sel': dw[3] & 0xfff, 'target': dw[0],
    }


def main():
    path = sys.argv[1]
    data, h, hs, segs, locs = links_all(path)
    stats = collections.Counter()
    kinds = collections.Counter()
    shown = 0
    nlist = int(sys.argv[sys.argv.index('--list') + 1]) if '--list' in sys.argv else 15
    for loc in sorted(locs):
        t = decode(data, hs + loc)
        kinds[t['type']] += 1
        if t['type'] < 8:   # not an image resource (buffer or something else)
            continue
        key = (DFMT.get(t['dfmt'], t['dfmt']), NFMT.get(t['nfmt'], t['nfmt']), t['tiling'], t['type'])
        stats[key] += 1
        if shown < nlist:
            shown += 1
            print('T# @%#x -> %#x  %dx%d pitch %d mips %d..%d  %s/%s tiling %d type %d dst %03x' % (
                loc, t['target'] << 8, t['w'], t['h'], t['pitch'], t['base_level'], t['last_level'],
                DFMT.get(t['dfmt'], t['dfmt']), NFMT.get(t['nfmt'], t['nfmt']), t['tiling'], t['type'],
                t['dst_sel']))
    print('kind-4 links: %d; T# type field distribution: %s' % (len(locs), dict(sorted(kinds.items()))))
    for (df, nf, tile, ty), c in stats.most_common(20):
        print('  %-10s %-6s tiling %2d type %2d : %d' % (df, nf, tile, ty, c))


if __name__ == '__main__':
    main()
