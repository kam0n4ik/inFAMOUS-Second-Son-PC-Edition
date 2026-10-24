# R10 pacing: which guest queues the lane (AQ) batches carry, their GPU time, and what C1's lane batch queued behind
import os, sys, collections
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "p412"))
import p412_analyze as pa
EV = pa.EV
tr = pa.load_trace(os.path.join(sys.argv[1], "p412_trace.bin"))
hz = tr["tsc_hz"]
allev = np.concatenate([t["ev"] for t in tr["threads"]])
gc = allev[allev["type"] == EV["GpuCompleted"]]
o = np.argsort(gc["a"])
conv, info = pa.calibrate(tr, (list(gc["a"][o].astype(np.int64)), list(gc["t0"][o].astype(np.int64))))
aqs = allev[allev["type"] == EV["AqSubmit"]]
sub = {int(x["a"]): x for x in aqs}
b = tr["batches"]
ms = lambda x: x / hz * 1e3
by = collections.defaultdict(lambda: [0, 0.0])
for x in b:
    s = sub.get(int(x["value"]))
    if s is None: continue
    lane = int(x["pad"]) & 1
    mask = int(s["c"]) >> 8
    k = (lane, mask)
    by[k][0] += 1
    by[k][1] += ms(conv(x["gpu_t1"]) - conv(x["gpu_t0"]))
span = ms(float(b["submit_t1"].max()) - float(b["submit_t0"].min())) / 1000
print(f"window {span:.1f} s")
for (lane, mask), (n, t) in sorted(by.items(), key=lambda kv: -kv[1][1]):
    qs = [q for q in range(8) if mask >> q & 1]
    print(f"  {'lane' if lane else 'main'} queues {qs}: batches {n} ({n/span:.0f}/s), GPU {t:.0f} ms ({t/span:.1f} ms/s), mean {t/n:.2f} ms")
fh = allev[allev["type"] == EV["FenceHeld"]]
print("fence queues held:", collections.Counter(int(x) for x in fh["c"]).most_common(8))
dd = allev[allev["type"] == EV["GameDingDong"]]
print("dingdong vqids:", collections.Counter(int(x) for x in dd["a"]).most_common(8))
