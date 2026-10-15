"""Research decoder for inFAMOUS Second Son animation clips ('70AE' blobs).

Layout reconstructed from the eboot clip sampler (0x71d50), the clip decoder (0x887e60) and its
track decoders (0x88a070/0x88a520 default rotation, 0x88c250 constant translation, 0x88f200 /
0x88fd70 / 0x891fe0 / 0x892a80 variable-width formats). Research only: no game code is run.
"""
import math
import struct


def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]


class BitReader:
    """MSB-first big-endian bit stream (pshufb byte reversal + shifts in the decoders)."""

    def __init__(self, data, base):
        self.d = data
        self.base = base

    def bits(self, bitpos, n):
        if n == 0:
            return 0
        o = self.base + (bitpos >> 3)
        sh = bitpos & 7
        nbytes = (sh + n + 7) >> 3
        v = int.from_bytes(self.d[o:o + nbytes], 'big')
        return (v >> (nbytes * 8 - sh - n)) & ((1 << n) - 1)


SQRT2_15 = 4.315969e-05  # sqrt(2)/32767


def quat48(b, o):
    """48-bit smallest-three quaternion (0x88a070): BE16 A (15 bits used), BE32 = B:15 C:15 idx:2."""
    a = int.from_bytes(b[o:o + 2], 'big')
    w32 = int.from_bytes(b[o + 2:o + 6], 'big')
    bb = w32 >> 17
    cc = (w32 >> 2) & 0x7fff
    idx = w32 & 3
    vals = [a * SQRT2_15 - 0.70710677, bb * SQRT2_15 - 0.70710677, cc * SQRT2_15 - 0.70710677]
    return fill_quat(vals, idx)


def fill_quat(vals, idx):
    s = 1.0 - vals[0] * vals[0] - vals[1] * vals[1] - vals[2] * vals[2]
    w = math.sqrt(min(max(s, 1.17549435e-38), 1.0))
    q = []
    it = iter(vals)
    for c in range(4):
        q.append(w if c == idx else next(it))
    return tuple(q)


class Fmt:
    """32-bit format word of a variable-width track: per component {m:5, e:4, s:1}, idx:2."""

    def __init__(self, f):
        self.f = f
        self.idx = f & 3
        self.comp = []
        for c in range(3):
            base = 2 + 10 * c
            m = (f >> base) & 31
            e = (f >> (base + 5)) & 15
            s = (f >> (base + 9)) & 1
            self.comp.append((m, e, s))
        self.bits = sum(m + e + s for m, e, s in self.comp)

    def decode(self, br, bitpos):
        """Returns the three values in stream order (component 2 first)."""
        out = []
        for c in (2, 1, 0):
            m, e, s = self.comp[c]
            sign = br.bits(bitpos, s); bitpos += s
            ex = br.bits(bitpos, e); bitpos += e
            mant = br.bits(bitpos, m); bitpos += m
            if e == 0:
                scale = 1.0 / ((1 << m) - 1) if m else 0.0
                v = mant - (1 << m) if sign else mant
                out.append(v * scale)
            else:
                expf = ex + 128 - (1 << (e - 1))
                bits = (sign << 31) | ((expf & 0xff) << 23) | ((mant << (23 - m)) & 0x7fffff)
                out.append(struct.unpack('<f', struct.pack('<I', bits))[0])
        return out


class Clip:
    def __init__(self, data, off):
        b = data
        self.d = b
        self.o = off
        assert b[off:off + 4] == b'70AE'
        self.frames = struct.unpack_from('<f', b, off + 4)[0]
        self.rate = struct.unpack_from('<f', b, off + 8)[0]
        self.header_size = u16(b, off + 0xc)
        self.joints = u16(b, off + 0xe)
        self.keys = u16(b, off + 0x10)
        self.segments = u16(b, off + 0x12)
        (self.n_const_rot, self.n_const_trans, self.n_const_scale, self.n_const_float,
         self.n_rot, self.n_trans, self.n_scale, self.n_float) = struct.unpack_from('<8H', b, off + 0x16)
        self.flags = u32(b, off + 0x2c)
        self.f28 = u16(b, off + 0x28)

        def rel(field):
            v = u32(b, off + field)
            return off + field + v if v else None
        self.p_mask = rel(0x30)
        self.p_seg_offsets = rel(0x38)
        self.p_seg_frames = rel(0x3c)
        self.p_const = [rel(0x40), rel(0x44), rel(0x48), rel(0x4c)]
        self.p_fmt = rel(0x50)
        # Joint index lists at +0x60 (u16); constant rotation list padded to 8, the rest to 4.
        counts = [self.n_const_rot, self.n_const_trans, self.n_const_scale, self.n_const_float,
                  self.n_rot, self.n_trans, self.n_scale, self.n_float]
        pads = [8, 4, 4, 4, 4, 4, 4, 4]
        p = off + 0x60
        self.lists = []
        for n, pad in zip(counts, pads):
            self.lists.append(list(struct.unpack_from('<%dH' % n, b, p)))
            p += ((n + pad - 1) // pad * pad) * 2
        self.seg_start = []
        self.seg_len = []
        for i in range(self.segments):
            s, l = struct.unpack_from('<HH', b, self.p_seg_frames + 4 * i)
            self.seg_start.append(s)
            self.seg_len.append(l)
        self.seg_off = [off + u32(b, self.p_seg_offsets + 8 * i + 4) for i in range(self.segments)]
        # Format words (variable-width formats only): per kind {const word, word per animated track}.
        self.fmt = {}
        p = self.p_fmt
        for kind, bit, n in ((0, 1, self.n_rot), (1, 2, self.n_trans), (2, 4, self.n_scale), (3, 8, self.n_float)):
            if self.flags & bit:
                self.fmt[kind] = [Fmt(u32(b, p + 4 * i)) for i in range(n + 1)]
                p += 4 * (n + 1)

    # --- constant tracks -------------------------------------------------------------------
    def const_rot(self):
        out = {}
        idx = self.lists[0]
        if not idx:
            return out
        p = self.p_const[0]
        if self.flags & 1:
            f = self.fmt[0][0]
            br = BitReader(self.d, p)
            for i, j in enumerate(idx):
                out[j] = fill_quat(f.decode(br, i * f.bits), f.idx)
        else:
            for i, j in enumerate(idx):
                out[j] = quat48(self.d, p + 6 * i)
        return out

    def const_vec(self, kind):
        out = {}
        idx = self.lists[kind]
        if not idx:
            return out
        p = self.p_const[kind]
        bit = 1 << kind
        if self.flags & bit:
            f = self.fmt[kind][0]
            br = BitReader(self.d, p)
            for i, j in enumerate(idx):
                out[j] = tuple(f.decode(br, i * f.bits))
        else:
            for i, j in enumerate(idx):
                out[j] = struct.unpack_from('<3f', self.d, p + 12 * i)
        return out

    # --- animated tracks -------------------------------------------------------------------
    def segment_layout(self, s):
        b = self.d
        p = self.seg_off[s]
        sz = struct.unpack_from('<8I', b, p)
        info = [p + 0x20]
        info.append(info[0] + sz[0])
        info.append(info[1] + sz[1])
        info.append(info[2] + sz[2])
        bitset = info[3] + sz[3]
        ln = self.seg_len[s]
        ntr = self.n_rot + self.n_trans + self.n_scale + self.n_float
        keys = [bitset + ((ntr * ln + 7) >> 3)]
        keys.append(keys[0] + sz[4])
        keys.append(keys[1] + sz[5])
        keys.append((keys[2] + sz[6] + 3) & ~3)
        return info, bitset, keys

    def track_values(self, s, kind, k):
        """All key values of track k of a kind in segment s: [(frame, value)], frame 0..len+1."""
        info, bitset, keys = self.segment_layout(s)
        ln = self.seg_len[s]
        counts = [self.n_rot, self.n_trans, self.n_scale, self.n_float]
        kind_base = sum(counts[:kind]) * ln
        br_set = BitReader(self.d, bitset)
        # Key counts of all tracks of this kind (track-major key data).
        def track_bits(t):
            return [br_set.bits(kind_base + t * ln + j, 1) for j in range(ln)]
        variable = bool(self.flags & (1 << kind))
        nxt = None
        if s + 1 < self.segments:
            nxt = self.segment_layout(s + 1)[0]
        frames = [0]
        bits = track_bits(k)
        frames += [j + 1 for j in range(ln) if bits[j]]
        vals = []
        if variable:
            fmts = self.fmt[kind][1:]
            info_bit = sum(f.bits for f in fmts[:k])
            key_bit = sum(sum(track_bits(t)) * fmts[t].bits for t in range(k))
            f = fmts[k]
            def dec(base, bit):
                v = f.decode(BitReader(self.d, base), bit)
                return fill_quat(v, f.idx) if kind == 0 else tuple(v)
            vals.append(dec(info[kind], info_bit))
            for i in range(len(frames) - 1):
                vals.append(dec(keys[kind], key_bit + i * f.bits))
            if nxt is not None:
                frames.append(ln + 1)
                vals.append(dec(nxt[kind], info_bit))
        else:
            if kind == 0:
                vals.append(quat48(self.d, info[0] + 6 * k))
                key_byte = 6 * sum(sum(track_bits(t)) for t in range(k))
                for i in range(len(frames) - 1):
                    vals.append(quat48(self.d, keys[0] + key_byte + 6 * i))
                if nxt is not None:
                    frames.append(ln + 1)
                    vals.append(quat48(self.d, nxt[0] + 6 * k))
            else:
                raise NotImplementedError('default translation/scale animated format')
        return frames, vals

    def sample_track(self, kind, k, t):
        """Value of animated track k at clip phase t in [0, 1] (sampler 0x71d50)."""
        frame = (self.keys - 1) * t
        fi = int(frame)
        s = 0
        for i in range(self.segments):
            if self.seg_start[i] <= fi:
                s = i
        local = fi - self.seg_start[s]
        frac = frame - fi
        if local > self.seg_len[s]:
            local = self.seg_len[s]
            frac = 1.0
        frames, vals = self.track_values(s, kind, k)
        x = local + frac
        prev = max(i for i, f in enumerate(frames) if f <= local)
        if prev + 1 >= len(frames):
            return vals[prev]
        f0, f1 = frames[prev], frames[prev + 1]
        w = (x - f0) / (f1 - f0)
        a, b = vals[prev], vals[prev + 1]
        if kind == 0:
            return slerp(a, b, w)
        return tuple(a[i] + (b[i] - a[i]) * w for i in range(len(a)))


def slerp(a, b, w):
    d = sum(a[i] * b[i] for i in range(4))
    if d < 0:
        b = tuple(-x for x in b)
        d = -d
    if d > 0.9995:
        r = tuple(a[i] + (b[i] - a[i]) * w for i in range(4))
    else:
        th = math.acos(min(d, 1.0))
        s = math.sin(th)
        wa = math.sin((1 - w) * th) / s
        wb = math.sin(w * th) / s
        r = tuple(a[i] * wa + b[i] * wb for i in range(4))
    n = math.sqrt(sum(x * x for x in r))
    return tuple(x / n for x in r)


def track_curve(clip, kind, k):
    """Values of animated track k at every integer frame 0..keys-1 (frac = 0)."""
    out = []
    for s in range(clip.segments):
        frames, vals = clip.track_values(s, kind, k)
        ln = clip.seg_len[s]
        last = ln if s + 1 < clip.segments else ln
        for local in range(0, last + 1):
            if clip.seg_start[s] + local >= clip.keys:
                break
            prev = max(i for i, f in enumerate(frames) if f <= local)
            if frames[prev] == local or prev + 1 >= len(frames):
                out.append(vals[prev])
                continue
            f0, f1 = frames[prev], frames[prev + 1]
            w = (local - f0) / (f1 - f0)
            a, b = vals[prev], vals[prev + 1]
            out.append(slerp(a, b, w) if kind == 0 else tuple(a[i] + (b[i] - a[i]) * w for i in range(3)))
    return out
