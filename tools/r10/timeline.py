# SecondSonPC R10: one run's CPU/GPU timeline (docs/R10_CPU_GPU_SCHEDULER.md §2) from the stall trace
# (SSPC_P412_STALL_TRACE=deep,..., src/runtime/gpu/stall_trace.*) -> a Chrome/Perfetto trace and a per-frame summary.
#   py -I -X utf8 tools\r10\timeline.py <run dir with p412_trace.bin> [--records] [--from s --to s] [--no-json]
# Writes <run>\r10\timeline.json.gz (open in https://ui.perfetto.dev or chrome://tracing), timeline_frames.csv (a row
# per presented frame) and timeline_summary.txt.
#
# Clocks: CPU spans are TSC; GPU batches are device timestamps converted to the TSC with the calibration pairs of
# VK_EXT_calibrated_timestamps (p412_analyze.calibrate): the GPU intervals are the GPU's execution, not the CPU's submit.
# A frame here is the interval between two presents (PresentDone instants: vkQueuePresentKHR returned).
import argparse
import bisect
import collections
import csv
import gzip
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "p412"))
import p412_analyze as pa  # noqa: E402

EV = pa.EV
WAIT_NAMES = ["IdleEmpty", "IdleGpu", "IdleLabel", "IdleFlag", "Drain", "QueueFull", "JournalFull", "RecordWait",
              "GpuSync", "RingWait", "BindWait", "QueueLock", "FlipAcquire", "FrontWait", "EmitWait", "VblankWait",
              "PresentFence", "PresentAcquire", "GameComputeWait", "GameIdleWait", "GameSubmitLock"]
WAITS = {EV[n] for n in WAIT_NAMES}
# the threads whose time is covered by waits (outside a wait they work): busy = window - waits
COVERED = {"CP", "Record", "Frontend", "Emit"}
INSTANTS = {EV[n] for n in ("FlipMarker", "GpuCompleted", "NotReady", "GameDingDong", "FenceHeld", "FenceWrite",
                            "InputEvent", "PresentDone")}
DETAIL = {EV["Record"], EV["FrontPrep"]}  # one span a record: only with --records


def union_len(iv, a, b):
    """Total length of the union of sorted (start, end) intervals clipped to [a, b]."""
    tot, cur_s, cur_e = 0.0, None, None
    for s, e in iv:
        s, e = max(s, a), min(e, b)
        if e <= s:
            continue
        if cur_e is None or s > cur_e:
            if cur_e is not None:
                tot += cur_e - cur_s
            cur_s, cur_e = s, e
        else:
            cur_e = max(cur_e, e)
    if cur_e is not None:
        tot += cur_e - cur_s
    return tot


def merged(iv):
    out = []
    for s, e in sorted(iv):
        if out and s <= out[-1][1]:
            out[-1][1] = max(out[-1][1], e)
        else:
            out.append([s, e])
    return out


class Merged:
    """Merged (disjoint, sorted) intervals with a fast clipped length (bisect - the per-frame loops stay linear)."""

    def __init__(self, iv):
        m = merged(iv)
        self.s = np.array([x[0] for x in m], dtype=float)
        self.e = np.array([x[1] for x in m], dtype=float)
        self.m = m

    def length(self, a, b):
        i = int(np.searchsorted(self.e, a, side="right"))
        j = int(np.searchsorted(self.s, b, side="left"))
        if j <= i:
            return 0.0
        s = np.clip(self.s[i:j], a, b)
        e = np.clip(self.e[i:j], a, b)
        return float((e - s).sum())

    def overlap(self, other, a, b):
        i = int(np.searchsorted(self.e, a, side="right"))
        j = int(np.searchsorted(self.s, b, side="left"))
        k = int(np.searchsorted(other.e, a, side="right"))
        n = int(np.searchsorted(other.s, b, side="left"))
        return overlap_len(self.m[i:j], other.m[k:n], a, b)


def overlap_len(m1, m2, a, b):
    """Length of the intersection of two merged interval lists inside [a, b]."""
    i = j = 0
    tot = 0.0
    while i < len(m1) and j < len(m2):
        s = max(m1[i][0], m2[j][0], a)
        e = min(m1[i][1], m2[j][1], b)
        if e > s:
            tot += e - s
        if m1[i][1] < m2[j][1]:
            i += 1
        else:
            j += 1
    return tot


def pct(v, q):
    return float(np.percentile(v, q)) if len(v) else float("nan")


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--records", action="store_true", help="one span per record of threads 2 / frontend in the json")
    ap.add_argument("--from", dest="t_from", type=float, default=None, help="seconds of the frame log's time")
    ap.add_argument("--to", dest="t_to", type=float, default=None)
    ap.add_argument("--no-json", action="store_true")
    a = ap.parse_args(argv)
    tr = pa.load_trace(os.path.join(a.run, "p412_trace.bin"))
    hz = tr["tsc_hz"]
    us = 1e6 / hz
    ms = 1e3 / hz
    t_lo = tr["win0"] if a.t_from is None else tr["start_tsc"] + a.t_from * hz
    t_hi = tr["win1"] if a.t_to is None else tr["start_tsc"] + a.t_to * hz
    out_dir = os.path.join(a.run, "r10")
    os.makedirs(out_dir, exist_ok=True)

    threads = tr["threads"]
    cp = next((t for t in threads if t["role"] == "CP"), None)
    comp_vals, comp_ts = [], []
    if cp is not None:
        gc = cp["ev"][cp["ev"]["type"] == EV["GpuCompleted"]]
        order = np.argsort(gc["a"], kind="stable")
        comp_vals, comp_ts = list(gc["a"][order].astype(np.int64)), list(gc["t0"][order].astype(np.int64))
    conv, cal_info = pa.calibrate(tr, (comp_vals, comp_ts))
    b = tr["batches"]
    lanes = {0: [], 1: []}
    sub_iv = []
    if conv is not None and len(b):
        g0, g1 = conv(b["gpu_t0"]), conv(b["gpu_t1"])
        for i in range(len(b)):
            lane = int(b["pad"][i]) & 1  # (GpuBatch flags bit 0: the async compute lane)
            lanes[lane].append((float(g0[i]), float(g1[i]), int(b["value"][i]), float(b["submit_t1"][i])))
    m_lane = {k: Merged([(s, e) for s, e, _, _ in v]) for k, v in lanes.items()}
    m_all = Merged([(s, e) for v in lanes.values() for s, e, _, _ in v])
    sub_a = np.array([sub for v in lanes.values() for _, _, _, sub in v], dtype=float)
    end_a = np.array([e for v in lanes.values() for _, e, _, _ in v], dtype=float)

    # presents
    pres = None
    for t in threads:
        if t["role"] == "Present":
            pres = t
    present_t = []
    if pres is not None:
        pd = pres["ev"][pres["ev"]["type"] == EV["PresentDone"]]
        present_t = sorted(int(x) for x in pd["t0"])
        if not present_t:  # (a trace without the R10 instants: the Present spans' ends)
            ps = pres["ev"][pres["ev"]["type"] == EV["Present"]]
            present_t = sorted(int(x) for x in ps["t1"])
    present_t = [x for x in present_t if t_lo <= x <= t_hi]
    # how far the CP runs ahead of the screen: flip markers reached minus presents done, both counted inside the
    # window - relative (the frames already in flight when the window opened are not known): shifted to min 0 below
    flip_t = sorted(int(x) for x in cp["ev"][cp["ev"]["type"] == EV["FlipMarker"]]["t0"] if t_lo <= x <= t_hi)         if cp is not None else []

    def flips_ahead(at):
        return bisect.bisect_right(flip_t, at) - bisect.bisect_right(present_t, at) if flip_t else float("nan")

    # per thread: waits as sorted intervals per wait type
    tinfo = []
    for t in threads:
        ev = t["ev"]
        if not len(ev):
            continue
        w = ev[np.isin(ev["type"], list(WAITS))]
        tinfo.append({"t": t, "label": f'{t["role"]}:{t["name"] or t["tid"]}',
                      "waits": Merged(list(zip(w["t0"].astype(float), w["t1"].astype(float)))),
                      "wait_by": {}, "spans_by": {}})
        for ty in np.unique(ev["type"]):
            ty = int(ty)
            if ty in DETAIL or ty in INSTANTS:
                continue
            x = ev[ev["type"] == ty]
            iv = list(zip(x["t0"].astype(float), x["t1"].astype(float)))
            tinfo[-1]["wait_by" if ty in WAITS else "spans_by"][ty] = Merged(iv)

    # frames
    rows = []
    for f0, f1 in zip(present_t, present_t[1:]):
        r = {"t_s": (f0 - tr["start_tsc"]) / hz, "present_ms": (f1 - f0) * ms}
        for k in (0, 1):
            r[f"gpu_lane{k}_busy_ms"] = m_lane[k].length(f0, f1) * ms
        r["gpu_busy_ms"] = m_all.length(f0, f1) * ms
        r["gpu_idle_ms"] = r["present_ms"] - r["gpu_busy_ms"]
        r["gfx_compute_overlap_ms"] = m_lane[0].overlap(m_lane[1], f0, f1) * ms
        # batches in the GPU's queue (submitted, not finished) when this frame was presented
        r["gpu_queue_depth"] = int(((sub_a <= f1) & (end_a > f1)).sum())
        r["cp_flips_ahead"] = flips_ahead(f1)
        for ti in tinfo:
            role = ti["t"]["role"]
            if role in COVERED:
                wait = ti["waits"].length(f0, f1) * ms
                r[f"{ti['label']}_wait_ms"] = wait
                r[f"{ti['label']}_busy_ms"] = r["present_ms"] - wait
        rows.append(r)
    fa = [r["cp_flips_ahead"] for r in rows if r["cp_flips_ahead"] == r["cp_flips_ahead"]]
    for r in rows:
        r["cp_flips_ahead"] -= min(fa) if fa else 0
    with open(os.path.join(out_dir, "timeline_frames.csv"), "w", newline="") as fh:
        if rows:
            keys = list(dict.fromkeys(k for r in rows for k in r))
            wr = csv.DictWriter(fh, fieldnames=keys)
            wr.writeheader()
            for r in rows:
                wr.writerow({k: (f"{v:.4f}" if isinstance(v, float) else v) for k, v in r.items()})

    # summary
    L = []
    L.append(f"run {a.run}")
    L.append(f"window {(t_lo - tr['start_tsc']) / hz:.2f}..{(t_hi - tr['start_tsc']) / hz:.2f} s, frames {len(rows)}, "
             f"calibration {cal_info.get('method')} ({cal_info.get('pairs', 0)} pairs, "
             f"max deviation {cal_info.get('max_deviation_us', float('nan')):.1f} us)")
    if rows:
        pm = np.array([r["present_ms"] for r in rows])
        L.append(f"present interval ms: mean {pm.mean():.2f}, p50 {pct(pm, 50):.2f}, p95 {pct(pm, 95):.2f}, "
                 f"p99 {pct(pm, 99):.2f}, max {pm.max():.1f}; FPS {1000.0 / pm.mean():.2f}; >50 ms {int((pm > 50).sum())}, "
                 f">100 ms {int((pm > 100).sum())}")
        for key, name in (("gpu_busy_ms", "GPU busy (union of queues)"), ("gpu_idle_ms", "GPU idle"),
                          ("gpu_lane0_busy_ms", "GPU main queue busy"), ("gpu_lane1_busy_ms", "GPU async compute lane busy"),
                          ("gfx_compute_overlap_ms", "main/compute queues executing at once"),
                          ("gpu_queue_depth", "batches in the GPU's queue at present"),
                          ("cp_flips_ahead", "CP flips ahead of the presents (relative, min 0)")):
            v = np.array([r[key] for r in rows], dtype=float)
            v = v[~np.isnan(v)]
            if len(v):
                L.append(f"  {name}: mean {v.mean():.2f}, p50 {pct(v, 50):.2f}, p95 {pct(v, 95):.2f}")
            else:
                L.append(f"  {name}: n/a (the trace has no PresentDone / FlipMarker ids)")
        L.append("threads (mean ms per presented frame; busy = the frame minus the traced waits - an upper bound: untraced waits count as busy):")
        for ti in tinfo:
            role = ti["t"]["role"]
            n_f = len(rows)
            span_ms = (present_t[-1] - present_t[0]) * ms if len(present_t) > 1 else 1.0
            line = f"  {ti['label']:<40}"
            if role in COVERED:
                busy = np.array([r[f"{ti['label']}_busy_ms"] for r in rows])
                line += f" busy {busy.mean():6.2f} (p95 {pct(busy, 95):6.2f})"
            parts = []
            for key, fmt_ in (("wait_by", "{} {:.2f}"), ("spans_by", "[{} {:.2f}]")):
                tots = sorted(((m.length(present_t[0], present_t[-1]) * ms, ty) for ty, m in ti[key].items()), reverse=True)
                for tot, ty in tots:
                    if tot / n_f >= 0.05:
                        parts.append(fmt_.format(pa.EVENTS[ty], tot / n_f))
            L.append(line + ("  " + ", ".join(parts) if parts else "") + f"  ({span_ms / 1000:.1f} s)")
        L.append("  (names: waits; [spans] - work spans; Record/FrontPrep per-record spans are part of busy)")
    # GPU idle attribution: inside the GPU's idle time (no batch of either queue executing) what each covered thread
    # did - worked (not in a traced wait) or waited, and by which wait (the critical path's CPU side: a thread that works
    # while the GPU idles is what the GPU waits for)
    if rows and len(m_all.m):
        lo, hi = present_t[0], present_t[-1]
        idle = []
        cur = lo
        for s0, e0 in m_all.m:
            if e0 <= lo or s0 >= hi:
                continue
            if s0 > cur:
                idle.append((cur, min(s0, hi)))
            cur = max(cur, e0)
        if cur < hi:
            idle.append((cur, hi))
        idle_m = Merged(idle)
        idle_ms = sum(e0 - s0 for s0, e0 in idle) * ms
        n_f = len(rows)
        L.append(f"GPU idle {idle_ms / n_f:.2f} ms per frame ({len(idle)} gaps, {sum(1 for s0, e0 in idle if (e0 - s0) * ms >= 1.0)} "
                 f"of 1 ms and more); meanwhile (ms per frame):")
        for ti in tinfo:
            if ti["t"]["role"] not in COVERED and ti["t"]["role"] != "Game":
                continue
            waits = []
            for ty, m in ti["wait_by"].items():
                v = idle_m.overlap(m, lo, hi) * ms
                if v / n_f >= 0.05:
                    waits.append((v, ty))
            waited = idle_m.overlap(ti["waits"], lo, hi) * ms
            txt = ", ".join(f"{pa.EVENTS[ty]} {v / n_f:.2f}" for v, ty in sorted(waits, reverse=True))
            L.append(f"  {ti['label']:<40} working {(idle_ms - waited) / n_f:6.2f}, waiting {waited / n_f:6.2f}"
                     + (f" ({txt})" if txt else ""))
    # pipeline builds
    pb = collections.Counter()
    pbt = collections.defaultdict(float)
    for ti in tinfo:
        e = ti["t"]["ev"]
        x = e[(e["type"] == EV["PipeBuild"]) & (e["t0"] >= t_lo) & (e["t1"] <= t_hi)]
        for c, t0, t1 in zip(x["c"], x["t0"], x["t1"]):
            pb[int(c)] += 1
            pbt[int(c)] += (int(t1) - int(t0)) * ms
    for c, n in sorted(pb.items()):
        L.append(f"pipeline builds on workers ({['async graphics', 'compute prefetch', 'precompiler'][c] if c < 3 else c}): "
                 f"{n}, {pbt[c]:.0f} ms")
    for ti in tinfo:
        e = ti["t"]["ev"]
        x = e[(e["type"] == EV["PipelineCreate"]) & (e["t0"] >= t_lo) & (e["t1"] <= t_hi)]
        if len(x):
            d = (x["t1"].astype(np.int64) - x["t0"].astype(np.int64)) * ms
            L.append(f"pipeline creations/waits on {ti['label']}: {len(x)}, {d.sum():.1f} ms, max {d.max():.1f} ms")
    txt = "\n".join(L)
    open(os.path.join(out_dir, "timeline_summary.txt"), "w", encoding="utf-8").write(txt + "\n")
    print(txt)

    if a.no_json:
        return 0
    # Chrome trace (JSON object format): pid 1 CPU threads, pid 2 GPU queues; ts in us from the window's start
    T = []
    T.append({"ph": "M", "pid": 1, "name": "process_name", "args": {"name": "CPU"}})
    T.append({"ph": "M", "pid": 2, "name": "process_name", "args": {"name": "GPU (device timestamps, calibrated)"}})
    for tid, nm in ((1, "main queue"), (2, "async compute lane")):
        T.append({"ph": "M", "pid": 2, "tid": tid, "name": "thread_name", "args": {"name": nm}})
    for ti in tinfo:
        t = ti["t"]
        T.append({"ph": "M", "pid": 1, "tid": t["tid"], "name": "thread_name", "args": {"name": ti["label"]}})
        ev = t["ev"]
        sel = (ev["t1"] >= t_lo) & (ev["t0"] <= t_hi)
        if not a.records:
            sel &= ~np.isin(ev["type"], list(DETAIL))
        for e in ev[sel]:
            ty = int(e["type"])
            name = pa.EVENTS[ty] if ty < len(pa.EVENTS) else str(ty)
            ts = (float(e["t0"]) - t_lo) * us
            args = {"a": int(e["a"]), "b": int(e["b"]), "c": int(e["c"])}
            if ty in INSTANTS:
                T.append({"ph": "i", "s": "t", "pid": 1, "tid": t["tid"], "name": name, "ts": ts, "args": args})
            else:
                T.append({"ph": "X", "pid": 1, "tid": t["tid"], "name": name, "ts": ts,
                          "dur": max((float(e["t1"]) - float(e["t0"])) * us, 0.01), "cat": "wait" if ty in WAITS else "work",
                          "args": args})
    for lane, v in lanes.items():
        for s, e, val, sub in v:
            if e < t_lo or s > t_hi:
                continue
            T.append({"ph": "X", "pid": 2, "tid": lane + 1, "name": f"batch {val}", "ts": (s - t_lo) * us,
                      "dur": max((e - s) * us, 0.01), "args": {"value": val, "submit_to_start_us": (s - sub) * us}})
    for x in present_t:
        T.append({"ph": "i", "s": "g", "pid": 1, "tid": 0, "name": "present", "ts": (x - t_lo) * us})
    with gzip.open(os.path.join(out_dir, "timeline.json.gz"), "wt", encoding="utf-8") as fh:
        json.dump({"traceEvents": T, "displayTimeUnit": "ms"}, fh)
    print(f"-> {os.path.join(out_dir, 'timeline.json.gz')} ({len(T)} events)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
