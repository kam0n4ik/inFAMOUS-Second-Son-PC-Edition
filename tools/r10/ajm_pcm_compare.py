"""R10 AJM (docs/R10_AJM.md): compares the PCM dumps of SSPC_R10_AJM=verify + SSPC_R10_AJM_DUMP=<dir>.

Every instance leaves ajm_c<context>_<instance>_own.pcm (SecondSonPC's AJM) and ..._oracle.pcm (shadPS4's) - S16
interleaved, what each one wrote into the output buffers, in order. For each pair: sizes, equal bytes, the first
difference, the largest sample difference; with --wav <channels> <rate> both are also written as .wav for listening.
  python -I tools/r10/ajm_pcm_compare.py <dir> [--wav 2 48000] [--json out.json]
"""
import argparse
import glob
import json
import os
import struct
import sys


def wav(path, pcm, channels, rate):
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, channels, rate, rate * channels * 2, channels * 2, 16))
        f.write(b"data" + struct.pack("<I", len(pcm)) + pcm)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--wav", nargs=2, type=int, metavar=("CHANNELS", "RATE"))
    ap.add_argument("--json")
    a = ap.parse_args()
    rows = []
    for own in sorted(glob.glob(os.path.join(a.dir, "ajm_*_own.pcm"))):
        oracle = own[: -len("_own.pcm")] + "_oracle.pcm"
        x = open(own, "rb").read()
        y = open(oracle, "rb").read() if os.path.exists(oracle) else b""
        n = min(len(x), len(y))
        first = next((i for i in range(n) if x[i] != y[i]), None) if x[:n] != y[:n] else None
        maxd = 0
        if first is not None:
            sx = struct.unpack("<%dh" % (n // 2), x[: n // 2 * 2])
            sy = struct.unpack("<%dh" % (n // 2), y[: n // 2 * 2])
            maxd = max(abs(p - q) for p, q in zip(sx, sy))
        r = {"stream": os.path.basename(own)[: -len("_own.pcm")], "own_bytes": len(x), "oracle_bytes": len(y),
             "equal": len(x) == len(y) and first is None, "first_difference": first, "max_abs_sample_diff": maxd}
        rows.append(r)
        print("%-24s own %9d B, oracle %9d B, %s" % (r["stream"], len(x), len(y),
              "IDENTICAL" if r["equal"] else "DIFFER (first at %s, max |diff| %d)" % (first, maxd)))
        if a.wav:
            ch, rate = a.wav
            wav(own[:-4] + ".wav", x, ch, rate)
            wav(oracle[:-4] + ".wav", y, ch, rate)
    if a.json:
        json.dump(rows, open(a.json, "w"), indent=1)
    if not rows:
        print("no dumps in", a.dir)
        return 1
    return 0 if all(r["equal"] for r in rows) else 2


if __name__ == "__main__":
    sys.exit(main())
