"""Decodes level 0 of textures (tiling 13, BC1/BC3/BC4/BC5) to PNG for visual checking.

Detiling hypothesis (thin 1D, AMD micro tile "thin"): elements are 4x4 BC blocks; 8x8 elements per
micro tile with Morton order (x0 y0 x1 y1 x2 y2); micro tiles row-major over the pitch.

usage: py -I -X utf8 kcap_texdump.py <file.xpps> <outdir> [N]
"""
import os
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kcap_textures import links_all, decode  # noqa: E402

BLOCK_BYTES = {35: 8, 36: 16, 37: 16, 38: 8, 39: 16}


def morton8(i):
    x = (i & 1) | ((i >> 1) & 2) | ((i >> 2) & 4)
    y = ((i >> 1) & 1) | ((i >> 2) & 2) | ((i >> 3) & 4)
    return x, y


def detile(raw, bw, bh, pitch_blocks, bpe):
    """Returns a (bh, bw, bpe) array of blocks in linear order."""
    out = np.zeros((bh, bw, bpe), np.uint8)
    tiles_x = (pitch_blocks + 7) // 8
    for ty in range((bh + 7) // 8):
        for tx in range(tiles_x):
            base = (ty * tiles_x + tx) * 64 * bpe
            for i in range(64):
                x, y = morton8(i)
                X, Y = tx * 8 + x, ty * 8 + y
                if X < bw and Y < bh:
                    o = base + i * bpe
                    out[Y, X] = np.frombuffer(raw[o:o + bpe], np.uint8)
    return out


def rgb565(c):
    return np.array([((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31])


def bc1_block(b):
    c0, c1, bits = struct.unpack('<HHI', bytes(b[:8]))
    p0, p1 = rgb565(c0), rgb565(c1)
    if c0 > c1:
        pal = [p0, p1, (2 * p0 + p1) // 3, (p0 + 2 * p1) // 3]
    else:
        pal = [p0, p1, (p0 + p1) // 2, np.array([0, 0, 0])]
    out = np.zeros((4, 4, 3), np.uint8)
    for i in range(16):
        out[i // 4, i % 4] = pal[(bits >> (2 * i)) & 3]
    return out


def bc4_channel(b):
    a0, a1 = b[0], b[1]
    bits = int.from_bytes(bytes(b[2:8]), 'little')
    if a0 > a1:
        pal = [a0, a1] + [((6 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
    else:
        pal = [a0, a1] + [((4 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
    out = np.zeros((4, 4), np.uint8)
    for i in range(16):
        out[i // 4, i % 4] = pal[(bits >> (3 * i)) & 7]
    return out


def decode_blocks(blocks, dfmt):
    bh, bw, _ = blocks.shape
    img = np.zeros((bh * 4, bw * 4, 3), np.uint8)
    for y in range(bh):
        for x in range(bw):
            b = blocks[y, x]
            if dfmt == 35:
                px = bc1_block(b)
            elif dfmt == 37:
                px = bc1_block(b[8:16])
            elif dfmt == 38:
                g = bc4_channel(b)
                px = np.stack([g, g, g], -1)
            else:  # 39 BC5: R, G -> show as RG normal map
                r, g = bc4_channel(b[:8]), bc4_channel(b[8:16])
                px = np.stack([r, g, np.full_like(r, 255)], -1)
            img[y * 4:y * 4 + 4, x * 4:x * 4 + 4] = px
    return img


def main():
    path, outdir = sys.argv[1], sys.argv[2]
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    os.makedirs(outdir, exist_ok=True)
    data, h, hs, segs, locs = links_all(path)
    done = 0
    for loc in sorted(locs):
        t = decode(data, hs + loc)
        if t['type'] != 9 or t['dfmt'] not in BLOCK_BYTES or t['tiling'] != 13:
            continue
        bpe = BLOCK_BYTES[t['dfmt']]
        bw, bh = (t['w'] + 3) // 4, (t['h'] + 3) // 4
        pitch_blocks = (t['pitch'] + 3) // 4
        size = ((pitch_blocks + 7) // 8 * 8) * ((bh + 7) // 8 * 8) * bpe
        off = hs + t['target']
        raw = data[off:off + size]
        img = decode_blocks(detile(raw, bw, bh, pitch_blocks, bpe), t['dfmt'])
        name = os.path.join(outdir, 'tex_%06x_%dx%d_fmt%d.png' % (loc, t['w'], t['h'], t['dfmt']))
        Image.fromarray(img).save(name)
        print('wrote', name)
        done += 1
        if done >= n:
            break


if __name__ == '__main__':
    main()
