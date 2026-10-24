# R10 pacing: per frame of the deep window - the GPU end of the frame's last batch (FlipMarker a = its timeline value),
# the interval to the previous one, and in that interval: main queue busy, lane busy, both at once, neither (idle);
# when the frame's first main batch started on the GPU; the game's wait A (blocked) overlapping it.
#   python -I -X utf8 tools/r10/gpuframes.py <run> [--rows N]
import os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "p412"))
import p412_analyze as pa
EV = pa.EV
tr = pa.load_trace(os.path.join(sys.argv[1], "p412_trace.bin"))
rows = int(sys.argv[sys.argv.index("--rows") + 1]) if "--rows" in sys.argv else 40
hz = tr["tsc_hz"]; T0 = tr["start_tsc"]
ms = lambda t: (float(t) - T0) / hz * 1e3
allev = np.concatenate([t["ev"] for t in tr["threads"]])
gc = allev[allev["type"] == EV["GpuCompleted"]]
o = np.argsort(gc["a"])
conv, info = pa.calibrate(tr, (list(gc["a"][o].astype(np.int64)), list(gc["t0"][o].astype(np.int64))))
B = {}
for x in tr["batches"]:
    B[int(x["value"])] = (ms(conv(x["gpu_t0"])), ms(conv(x["gpu_t1"])), int(x["pad"]) & 1)
fm = allev[allev["type"] == EV["FlipMarker"]]
fm = fm[np.argsort(fm["t0"])]
waits = allev[allev["type"] == EV["GameComputeWait"]]
waits = [(ms(w["t0"]), ms(w["t1"])) for w in waits if int(w["c"]) == 1]
ends = []
for f in fm:
    v = int(f["a"])
    if v in B:
        ends.append((v, int(f["b"]), B[v][1]))
vals = sorted(B)
def busy(lo, hi, lane):
    iv = sorted((max(a, lo), min(b, hi)) for v, (a, b, l) in B.items() if l == lane and b > lo and a < hi)
    out = [];
    for a, b in iv:
        if out and a <= out[-1][1]: out[-1][1] = max(out[-1][1], b)
        else: out.append([a, b])
    return out
def total(iv): return sum(b - a for a, b in iv)
def inter(x, y):
    i = j = 0; s = 0.0
    while i < len(x) and j < len(y):
        a = max(x[i][0], y[j][0]); b = min(x[i][1], y[j][1])
        if b > a: s += b - a
        if x[i][1] < y[j][1]: i += 1
        else: j += 1
    return s
print(f"{'frame':>6} {'G end':>9} {'dt':>6} | {'main':>5} {'lane':>5} {'both':>5} {'idle':>5} | {'waitA':>5}")
res = []
for (v0, f0, e0), (v1, f1, e1) in zip(ends, ends[1:]):
    m = busy(e0, e1, 0); l = busy(e0, e1, 1)
    tm, tl, tb = total(m), total(l), inter(m, l)
    idle = (e1 - e0) - (tm + tl - tb)
    wa = sum(max(0, min(b, e1) - max(a, e0)) for a, b in waits)
    res.append((f1, e1, e1 - e0, tm, tl, tb, idle, wa))
for r in res[:rows]:
    print(f"{r[0]:6d} {r[1]:9.1f} {r[2]:6.1f} | {r[3]:5.1f} {r[4]:5.1f} {r[5]:5.1f} {r[6]:5.1f} | {r[7]:5.1f}")
a = np.array([r[2:] for r in res])
if len(a):
    sh = a[:, 0] < 22; lg = a[:, 0] > 36
    for name, m in (("all", np.ones(len(a), bool)), ("dt<22", sh), ("22-36", ~sh & ~lg), ("dt>36", lg)):
        if m.any():
            mm = a[m].mean(0)
            print(f"{name:6s} n {m.sum():4d}: dt {mm[0]:5.1f} main {mm[1]:5.1f} lane {mm[2]:5.1f} both {mm[3]:5.1f} idle {mm[4]:5.1f} waitA {mm[5]:5.1f}")
