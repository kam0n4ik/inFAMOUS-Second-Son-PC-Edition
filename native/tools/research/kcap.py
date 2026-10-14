"""KCAP (.xpps) research reader for inFAMOUS Second Son packages.

Research tool only: used to confirm format hypotheses on real packages before
they are implemented in C++ (native/engine/resources). Prints the header,
segment table and block table.

usage: py -I -X utf8 kcap.py <file.xpps> [--blocks N] [--hex OFF LEN]
"""
import struct
import sys


def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def u64(b, o): return struct.unpack_from('<Q', b, o)[0]


def parse(data):
    if data[:4] != b'KCAP':
        raise ValueError('not KCAP')
    h = {
        'version': u16(data, 4), 'kind': u16(data, 6),
        'f08': u32(data, 8), 'f0c': u32(data, 0xc), 'f10': u32(data, 0x10),
        'f14': u32(data, 0x14), 'f18': u32(data, 0x18), 'f1c': u32(data, 0x1c),
        'header_size': u32(data, 0x28), 'data_size': u32(data, 0x2c),
        'hash': data[0x78:0x88].hex(),
        'seg_count': u32(data, 0xa0), 'block_count': u32(data, 0xb0),
    }
    return h


SEG_TABLE = 0xc8
SEG_SIZE = 0x28
BLOCK_SIZE = 12


def segments(data, h):
    out = []
    for i in range(h['seg_count']):
        o = SEG_TABLE + i * SEG_SIZE
        out.append({'flags': data[o], 'tag': data[o + 1], 'pool': u16(data, o + 2),
                    'size': u32(data, o + 4), 'offset': u64(data, o + 8),
                    'z': u64(data, o + 0x10), 'first_block': u64(data, o + 0x18),
                    'block_count': u64(data, o + 0x20)})
    return out


def blocks(data, h):
    # Layout read from the package loader (eboot 0x30fc00): the block table follows the segment
    # table directly; entry = {u32 size, u32 offset, u16 align, u16 type}.
    base = SEG_TABLE + h['seg_count'] * SEG_SIZE
    out = []
    for i in range(h['block_count']):
        o = base + i * BLOCK_SIZE
        out.append({'size': u32(data, o), 'offset': u32(data, o + 4),
                    'align': u16(data, o + 8), 'type': u16(data, o + 10)})
    return out


TAG_DIC = 0x43494420   # ' DIC' - type-hash fixups (u32 type index written at a location)
TAG_KNLI = 0x494c4e4b  # 'KNLI' - internal pointer fixups
TAG_KNLX = 0x584c4e4b  # 'KNLX' - external (cross-package) links
TAG_PYTO = 0x4f545950  # 'PYTO' - type-index fixups with width/flags
TAG_PLPS = 0x53504c50  # 'PLPS' - splice/property patches (eboot 0x30b1c0)
TAG_END = 0x454e4420   # ' DNE' - end of stream
TAG_NAMES = {TAG_DIC: 'DIC', TAG_KNLI: 'KNLI', TAG_KNLX: 'KNLX', TAG_PYTO: 'PYTO',
             TAG_PLPS: 'PLPS', TAG_END: 'END'}


def chunks(data, start, limit):
    """Chunk stream walked by eboot 0x30a310: {u32 tag, u32 size, payload[size]}."""
    o = start
    while o + 8 <= limit:
        tag, size = u32(data, o), u32(data, o + 4)
        yield tag, o + 8, size
        if tag == TAG_END:
            return
        o += 8 + size


def link_offsets(data, o, end):
    """KNLI stream decoder (eboot 0x5d2260): u16 codes -> 4-byte-granular data offsets."""
    base = -1 & 0xffffffff
    last = base
    while o < end:
        v = struct.unpack_from('<h', data, o)[0]
        o += 2
        if v >= 0:
            last = (v * 4 + base) & 0xffffffff
            yield last
        elif (v & 0x4000) == 0:
            for _ in range((v & 0x3fff)):
                last = (last + 4) & 0xffffffff
                yield last
        else:
            base = ((v & 0x3fff) << 17) & 0xffffffff


def knli(data, payload):
    """Returns [(kind, location_offset)] for every internal link (eboot 0x30a4a0)."""
    n = u32(data, payload)
    stream = payload + 4 + n * 8
    out = []
    for i in range(n):
        kind = u32(data, payload + 4 + i * 8)
        ln = struct.unpack_from('<i', data, payload + 8 + i * 8)[0]
        for loc in link_offsets(data, stream, stream + ln * 2):
            out.append((kind, loc))
        stream += ln * 2
    return out


def hexrow(data, o, n=16):
    row = data[o:o + n]
    return '%s  %s' % (' '.join('%02x' % c for c in row),
                       ''.join(chr(c) if 32 <= c < 127 else '.' for c in row))


def main():
    path = sys.argv[1]
    data = open(path, 'rb').read()
    h = parse(data)
    for k, v in h.items():
        print('%-12s %s' % (k, hex(v) if isinstance(v, int) else v))
    print('file size    %s  header+data %s' % (hex(len(data)), hex(h['header_size'] + h['data_size'])))
    segs = segments(data, h)
    print('--- segments')
    for i, s in enumerate(segs):
        print('  #%d flags=%02x tag=%02x pool=%d size=%#x off=%#x z=%#x blocks=[%d..+%d]' % (
            i, s['flags'], s['tag'], s['pool'], s['size'], s['offset'], s['z'],
            s['first_block'], s['block_count']))
    pre = u64(data, SEG_TABLE + h['seg_count'] * SEG_SIZE)
    print('--- u64 after segments: %#x' % pre)
    bl = blocks(data, h)
    hist = {}
    for b in bl:
        hist.setdefault(b['type'], [0, 0])
        hist[b['type']][0] += 1
        hist[b['type']][1] += b['size']
    print('--- block types: ' + ', '.join('%d:%dx/%#x' % (t, c, s) for t, (c, s) in sorted(hist.items())))
    hs = h['header_size']
    # The service stream lives in the pool-2 segment (block type 7).
    svc = [s for s in segs if s['pool'] == 2]
    links = []
    for s in svc:
        st = hs + s['offset']
        for tag, p, size in chunks(data, st, st + s['size']):
            name = TAG_NAMES.get(tag, '%08x' % tag)
            extra = ''
            if tag in (TAG_DIC, TAG_PYTO):
                extra = 'entries=%d' % u32(data, p)
            elif tag == TAG_KNLI:
                links = knli(data, p)
                kinds = {}
                for k, _ in links:
                    kinds[k] = kinds.get(k, 0) + 1
                extra = 'links=%d by kind %s' % (len(links), kinds)
            elif tag == TAG_KNLX:
                extra = 'entries=%d' % struct.unpack_from('<i', data, p + 8)[0]
            print('--- chunk %-4s at %#x size %#x %s' % (name, p - 8 - hs, size, extra))
    if '--vsharp' in sys.argv:
        dfmt_names = {1: '8', 2: '16', 3: '8_8', 4: '32', 5: '16_16', 6: '10_11_11', 7: '11_11_10',
                      8: '10_10_10_2', 9: '2_10_10_10', 10: '8_8_8_8', 11: '32_32', 12: '16_16_16_16',
                      13: '32_32_32', 14: '32_32_32_32'}
        nfmt_names = {0: 'unorm', 1: 'snorm', 2: 'uscaled', 3: 'sscaled', 4: 'uint', 5: 'sint',
                      6: 'snorm_nz', 7: 'float'}
        stat = {}
        for kind, loc in links:
            if kind != 2:
                continue
            d0, d1, d2, d3 = struct.unpack_from('<4I', data, hs + loc)
            stride = (d1 >> 16) & 0x3fff
            dfmt = (d3 >> 15) & 0xf
            nfmt = (d3 >> 12) & 0x7
            key = (stride, dfmt_names.get(dfmt, dfmt), nfmt_names.get(nfmt, nfmt))
            stat[key] = stat.get(key, 0) + 1
        for (stride, df, nf), c in sorted(stat.items(), key=lambda x: -x[1])[:30]:
            print('  V# stride=%3d fmt=%s/%s x%d' % (stride, df, nf, c))
    nb = int(sys.argv[sys.argv.index('--blocks') + 1]) if '--blocks' in sys.argv else 0
    for i, b in enumerate(bl[:nb]):
        print('  blk%3d align=%#x type=%2d size=%#8x off=%#9x | %s' % (
            i, b['align'], b['type'], b['size'], b['offset'], hexrow(data, hs + b['offset'], 32)))
    if '--hex' in sys.argv:
        i = sys.argv.index('--hex')
        off = int(sys.argv[i + 1], 0)
        ln = int(sys.argv[i + 2], 0)
        for o in range(off, off + ln, 16):
            row = data[o:o + 16]
            print('%08x: %s  %s' % (o, ' '.join('%02x' % c for c in row),
                                    ''.join(chr(c) if 32 <= c < 127 else '.' for c in row)))


if __name__ == '__main__':
    main()
