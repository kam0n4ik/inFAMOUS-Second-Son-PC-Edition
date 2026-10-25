"""R11: tools/p54/cc_chain.py with p412_analyze from this worktree's tools/p412.
The chain of each game async compute wait that blocked (CC): doorbell -> wait start -> the processor reaches the
label's RELEASE_MEM -> its batch submitted -> GPU start/end -> the label written -> the game wakes. ms."""
import sys, numpy as np
import os; sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "p412"))
import p412_analyze as pa
tr = pa.load_trace(sys.argv[1]); hz = tr["tsc_hz"]; EV = pa.EV
conv, info = pa.calibrate(tr, ([], []))
allev = np.sort(np.concatenate([t["ev"] for t in tr["threads"]]), order="t0")
game = np.sort(np.concatenate([t["ev"] for t in tr["threads"] if t["role"] == "Game"]), order="t0")
waits = game[(game["type"] == EV["GameComputeWait"]) & (game["c"] != 0)]
dd = game[game["type"] == EV["GameDingDong"]]["t0"].astype(np.int64)
fh = allev[allev["type"] == EV["FenceHeld"]]
fw = allev[allev["type"] == EV["FenceWrite"]]
b = np.sort(tr["batches"], order="submit_t0")
bs0 = b["submit_t0"].astype(np.int64)
g0 = conv(b["gpu_t0"]); g1 = conv(b["gpu_t1"])
seg = {k: [] for k in ("doorbell->wait", "wait->cp reaches", "cp->submitted", "submitted->gpu start", "gpu exec",
                       "gpu end->label", "label->game wakes", "whole wait")}
for w in waits:
    a, e = int(w["t0"]), int(w["t1"])
    m = fh[(fh["a"] == w["a"]) & (fh["b"] >= w["b"]) & (fh["t0"].astype(np.int64) >= a - hz // 10)]
    l = fw[(fw["a"] == w["a"]) & (fw["b"] >= w["b"]) & (fw["t0"].astype(np.int64) >= a - hz // 10)]
    if len(m) == 0 or len(l) == 0:
        continue
    tc = int(m["t0"][0]); tl = int(l["t0"][0])
    i = int(np.searchsorted(bs0, tc))
    if i >= len(b):
        continue
    d = dd[dd <= a]
    td = int(d[-1]) if len(d) else a
    ms = lambda x: x / hz * 1e3
    seg["doorbell->wait"].append(ms(a - td))
    seg["wait->cp reaches"].append(ms(tc - a))
    seg["cp->submitted"].append(ms(int(b["submit_t1"][i]) - tc))
    seg["submitted->gpu start"].append(ms(g0[i] - int(b["submit_t1"][i])))
    seg["gpu exec"].append(ms(g1[i] - g0[i]))
    seg["gpu end->label"].append(ms(tl - g1[i]))
    seg["label->game wakes"].append(ms(e - tl))
    seg["whole wait"].append(ms(e - a))
print(info.get("method"), "waits", len(seg["whole wait"]), "of", len(waits))
for k, v in seg.items():
    v = np.array(v)
    print(f"  {k:22s} mean {v.mean():7.3f}  p50 {np.median(v):7.3f}  p90 {np.percentile(v, 90):7.3f}  p99 {np.percentile(v, 99):7.3f}")
