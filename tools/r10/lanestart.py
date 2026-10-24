# R10 pacing: when does a lane (AQ) batch start on the GPU - relative to its submission and to the main queue's batches
# (is the lane held until a main batch boundary, or until the main queue drains?)
#   python -I -X utf8 tools/r10/lanestart.py <run dir>
import os, sys
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
lane = (b["pad"] & 1) == 1
gs = conv(b["gpu_t0"]); ge = conv(b["gpu_t1"])
mv = np.nonzero(~lane)[0]
m_gs, m_ge = gs[mv], ge[mv]
order = np.argsort(m_ge); m_ge_s = m_ge[order]; m_gs_s = m_gs[order]
ms = lambda x: x / hz * 1e3
rows = []
for i in np.nonzero(lane)[0]:
    v = int(b["value"][i]); s = sub.get(v)
    if s is None: continue
    t_sub = float(s["t1"]); c = int(s["c"])
    # main busy at the submission? (a main batch started before and ending after)
    busy = np.any((m_gs <= t_sub) & (m_ge > t_sub))
    # main batch ends between submission and lane start
    k = np.searchsorted(m_ge_s, gs[i])
    prev_end = m_ge_s[k - 1] if k > 0 else np.nan
    # the main queue's last end at or after lane start (drain): last main batch submitted before the lane submit
    pend = [ge[j] for j in mv if float(sub[int(b["value"][j])]["t1"]) < t_sub] if False else []
    rows.append((ms(gs[i] - t_sub), ms(gs[i] - prev_end), busy, c & 2, ms(ge[i] - gs[i])))
r = np.array(rows, dtype=float)
print("lane batches", len(r), "calib", info.get("method"))
for name, m in (("main busy at submit", r[:, 2] == 1), ("main idle at submit", r[:, 2] == 0)):
    m2 = m & (r[:, 3] == 0)
    if m2.sum():
        print(f"  [{name}, no wait for main] n {int(m2.sum())}: submit->start p50 {np.median(r[m2,0]):.2f} p90 {np.percentile(r[m2,0],90):.2f} ms;"
              f" start - previous main batch end: p50 {np.median(r[m2,1]):.3f} p10 {np.percentile(r[m2,1],10):.3f} p90 {np.percentile(r[m2,1],90):.3f} ms; dur p50 {np.median(r[m2,4]):.2f}")
x = r[(r[:, 2] == 1) & (r[:, 3] == 0), 1]
print("  share of lane starts within 0.05 ms after a main batch end (busy at submit, no wait):", f"{np.mean(np.abs(x) < 0.05):.2f}" if len(x) else "-")
# overlap: lane execution intervals intersecting main execution
# the lane's own queue: a lane batch starting right after the previous lane batch ended (queued behind it)?
li = np.nonzero(lane)[0]
li = li[np.argsort(b["value"][li])]
prev_le = None
d_prev, d_sub, idle_both = [], [], []
for i in li:
    v = int(b["value"][i]); s = sub.get(v)
    if s is None: continue
    t_sub = float(s["t1"])
    if prev_le is not None:
        d_prev.append(ms(gs[i] - prev_le))
        d_sub.append(ms(gs[i] - t_sub))
        mbusy = np.any((m_gs <= t_sub) & (m_ge > t_sub))
        idle_both.append((not mbusy) and prev_le < t_sub and (int(s["c"]) & 2) == 0)
    prev_le = ge[i]
d_prev = np.array(d_prev); d_sub = np.array(d_sub); idle_both = np.array(idle_both)
print(f"  lane start - previous lane end: p50 {np.median(d_prev):.3f} p10 {np.percentile(d_prev,10):.3f}")
if idle_both.sum():
    print(f"  both queues idle at submit, no wait for main: n {int(idle_both.sum())}, submit->start p50 {np.median(d_sub[idle_both]):.2f}"
          f" p10 {np.percentile(d_sub[idle_both],10):.2f} p90 {np.percentile(d_sub[idle_both],90):.2f} ms")
# main batches: the same for comparison (submit -> start when the GPU is idle)
mi = mv[np.argsort(b["value"][mv])]
dd = []
for j, i in enumerate(mi):
    s = sub.get(int(b["value"][i]))
    if s is None or j == 0: continue
    t_sub = float(s["t1"])
    if ge[mi[j - 1]] < t_sub and not np.any((gs[li] <= t_sub) & (ge[li] > t_sub)):
        dd.append(ms(gs[i] - t_sub))
dd = np.array(dd)
print(f"  main batches submitted to an idle GPU: n {len(dd)}, submit->start p50 {np.median(dd):.2f} p10 {np.percentile(dd,10):.2f} p90 {np.percentile(dd,90):.2f} ms")
