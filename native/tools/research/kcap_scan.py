"""Checks KCAP header/table hypotheses on every package of a directory (headers only).

usage: py -I -X utf8 kcap_scan.py <dir>
"""
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kcap  # noqa: E402


def check(path):
    with open(path, 'rb') as f:
        head = f.read(0x10000)
        size = os.path.getsize(path)
    if len(head) < 0xc8 or head[:4] != b'KCAP':
        return 'not KCAP', None
    h = kcap.parse(head)
    if h['f18'] + h['f1c'] != h['header_size']:
        return 'header size != f18 + f1c', h
    if h['header_size'] + h['data_size'] != size:
        return 'header+data != file size', h
    end = kcap.SEG_TABLE + h['seg_count'] * kcap.SEG_SIZE + h['block_count'] * kcap.BLOCK_SIZE
    if end > h['header_size']:
        return 'tables exceed header', h
    blocks = kcap.blocks(head, h)
    for b in blocks:
        if b['offset'] + b['size'] > h['data_size']:
            return 'block outside data', h
    for s in kcap.segments(head, h):
        if s['offset'] + s['size'] > h['data_size']:
            return 'segment outside data', h
        if s['first_block'] + s['block_count'] > h['block_count']:
            return 'segment block range', h
        for b in blocks[s['first_block']:s['first_block'] + s['block_count']]:
            if not (s['offset'] <= b['offset'] and b['offset'] + b['size'] <= s['offset'] + s['size']):
                return 'block not inside its segment', h
    return 'ok', h


def main():
    d = sys.argv[1]
    res = collections.Counter()
    tails = collections.Counter()
    types_by_pool = collections.defaultdict(collections.Counter)
    examples = {}
    for name in sorted(os.listdir(d)):
        if not name.endswith('.xpps'):
            continue
        r, h = check(os.path.join(d, name))
        res[r] += 1
        examples.setdefault(r, name)
        if h and r == 'ok':
            with open(os.path.join(d, name), 'rb') as f:
                head = f.read(h['header_size'])
            end = kcap.SEG_TABLE + h['seg_count'] * kcap.SEG_SIZE + h['block_count'] * kcap.BLOCK_SIZE
            tails[h['header_size'] - end] += 1
            bl = kcap.blocks(head, h)
            for s in kcap.segments(head, h):
                for b in bl[s['first_block']:s['first_block'] + s['block_count']]:
                    types_by_pool[s['pool']][b['type']] += 1
    for k, v in res.most_common():
        print('%-32s %5d   e.g. %s' % (k, v, examples[k]))
    print('bytes after the block table:', dict(tails))
    for pool, c in sorted(types_by_pool.items()):
        print('pool %d block types: %s' % (pool, dict(sorted(c.items()))))


if __name__ == '__main__':
    main()
