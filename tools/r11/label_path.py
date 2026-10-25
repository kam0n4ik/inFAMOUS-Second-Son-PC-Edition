"""R11: where a blocked game compute wait's label spends "GPU end -> label written" (P4.12 deep + P5.4 trace).

  py -I -X utf8 tools/r11/label_path.py <p412_trace.bin>

For each blocked GameComputeWait (as tools/r11/cc_chain.py): its batch (the one the processor submitted with the label's
held fence), the first GpuCompleted instant (a Poll that saw the timeline at or past the batch) after the batch's GPU
end, and the FenceWrite of the label. Splits "GPU end -> label" into "GPU end -> a poll saw it" and "seen -> written",
names the thread that saw it and the one that wrote, and sums what thread 1 (CP) was doing meanwhile (its spans).
"""
import collections
import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "p412"))
import p412_analyze as pa  # noqa: E402

tr = pa.load_trace(sys.argv[1])
hz = tr["tsc_hz"]
EV = pa.EV
NAMES = {v: k for k, v in EV.items()}
conv, info = pa.calibrate(tr, ([], []))
allev = np.sort(np.concatenate([t["ev"] for t in tr["threads"]]), order="t0")
game = np.sort(np.concatenate([t["ev"] for t in tr["threads"] if t["role"] == "Game"]), order="t0")
waits = game[(game["type"] == EV["GameComputeWait"]) & (game["c"] != 0)]
fh = allev[allev["type"] == EV["FenceHeld"]]
fw_by = []  # (role, events)
gc_by = []
for t in tr["threads"]:
    e = t["ev"]
    fw_by.append((t["role"], e[e["type"] == EV["FenceWrite"]]))
    gc_by.append((t["role"], e[e["type"] == EV["GpuCompleted"]]))
cp = [t for t in tr["threads"] if t["role"] == "CP"]
cpev = np.sort(np.concatenate([t["ev"] for t in cp]), order="t0") if cp else None
b = np.sort(tr["batches"], order="submit_t0")
bs0 = b["submit_t0"].astype(np.int64)
g1 = conv(b["gpu_t1"])
ms = lambda x: x / hz * 1e3
seen_ms, write_ms, total_ms = [], [], []
seen_role, write_role = collections.Counter(), collections.Counter()
cp_doing = collections.Counter()
for w in waits:
    a = int(w["t0"])
    m = fh[(fh["a"] == w["a"]) & (fh["b"] >= w["b"]) & (fh["t0"].astype(np.int64) >= a - hz // 10)]
    if len(m) == 0:
        continue
    i = int(np.searchsorted(bs0, int(m["t0"][0])))
    if i >= len(b):
        continue
    end = int(g1[i])
    val = int(b["value"][i])
    best_w = None
    for role, e in fw_by:
        x = e[(e["a"] == w["a"]) & (e["b"] >= w["b"]) & (e["t0"].astype(np.int64) >= a - hz // 10)]
        if len(x) and (best_w is None or int(x["t0"][0]) < best_w[1]):
            best_w = (role, int(x["t0"][0]))
    if best_w is None:
        continue
    best_s = None
    for role, e in gc_by:
        x = e[(e["a"] >= val) & (e["t0"].astype(np.int64) >= end - hz // 20000)]
        if len(x) and (best_s is None or int(x["t0"][0]) < best_s[1]):
            best_s = (role, int(x["t0"][0]))
    tl = best_w[1]
    total_ms.append(ms(tl - end))
    write_role[best_w[0]] += 1
    if best_s is not None and best_s[1] <= tl:
        seen_ms.append(ms(best_s[1] - end))
        write_ms.append(ms(tl - best_s[1]))
        seen_role[best_s[0]] += 1
    if cpev is not None:
        sp = cpev[(cpev["t1"].astype(np.int64) > end) & (cpev["t0"].astype(np.int64) < tl)]
        for s in sp:
            o = min(int(s["t1"]), tl) - max(int(s["t0"]), end)
            if o > 0:
                cp_doing[NAMES.get(int(s["type"]), str(s["type"]))] += o
n = len(total_ms)
print(f"{info.get('method')}: {n} blocked waits with their label write")
for name, v in (("GPU end -> label written", total_ms), ("GPU end -> a poll saw it", seen_ms),
                ("seen -> label written", write_ms)):
    v = np.array(v) if v else np.zeros(1)
    print(f"  {name:28s} mean {v.mean():7.3f}  p50 {np.median(v):7.3f}  p90 {np.percentile(v, 90):7.3f}  "
          f"p99 {np.percentile(v, 99):7.3f}  (n {len(v)})")
print("  completion first seen by: " + ", ".join(f"{r} {c}" for r, c in seen_role.most_common()))
print("  label written by: " + ", ".join(f"{r} {c}" for r, c in write_role.most_common()))
print("  thread 1 (CP) spans inside [GPU end, label written], ms per wait:")
for k, v in cp_doing.most_common(12):
    print(f"    {k:16s} {ms(v) / max(n, 1):7.3f}")
