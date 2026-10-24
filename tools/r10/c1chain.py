# R10 pacing (docs/R10_PACING.md): the chain of the game's async compute C1(N+1) - the work its wait A (0x3edef0 ->
# 0x1695e0) blocks on - step by step, per game frame, against the graphics frame G(N) it overlaps and the present
# intervals. Needs a deep P4.12 trace with the P5.4 frame trace (SSPC_P54_FRAME_TRACE=1,
# SSPC_P412_STALL_TRACE=deep,window=a:b) of an R10 build (AqSubmit events, the lane bit of the GPU batches).
#   py -I -X utf8 tools/r10/c1chain.py <run dir or p412_trace.bin> [--rows N] [--csv out.csv]
# Steps of one wait A (label L >= value V), all on the TSC (GPU times through the calibration pairs):
#   dd   the game's DingDong of its compute queue (the last before the wait)       -> cp   thread 1 parsed the RELEASE_MEM
#        (FenceHeld L/V)                                                             -> sub  the batch holding the fence
#        submitted by thread 2 (the first AqSubmit with that queue after FenceHeld) -> gs/ge its GPU start/end
#                                                                                    -> fw   the fence written (FenceWrite)
#                                                                                    -> we   the game's wait ended
# and G(N): the graphics frame before (FlipMarker processed before the wait), its flip batch's GPU end.
import bisect
import csv
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "p412"))
import p412_analyze as pa  # noqa: E402

EV = pa.EV


def main(argv):
    path = argv[1]
    if os.path.isdir(path):
        path = os.path.join(path, "p412_trace.bin")
    rows_n = 40
    out_csv = None
    i = 2
    while i < len(argv):
        if argv[i] == "--rows":
            rows_n = int(argv[i + 1]); i += 2
        elif argv[i] == "--csv":
            out_csv = argv[i + 1]; i += 2
        else:
            raise SystemExit(f"unknown argument {argv[i]}")
    tr = pa.load_trace(path)
    hz = tr["tsc_hz"]
    t00 = tr["start_tsc"]
    ms = lambda t: (float(t) - t00) / hz * 1e3
    d = lambda a, b: (float(b) - float(a)) / hz * 1e3
    allev = np.concatenate([t["ev"] for t in tr["threads"]])
    allev = allev[np.argsort(allev["t0"], kind="stable")]
    sel = lambda name: allev[allev["type"] == EV[name]]
    game = np.concatenate([t["ev"] for t in tr["threads"] if t["role"] == "Game"])
    game = game[np.argsort(game["t0"], kind="stable")]
    cw = game[(game["type"] == EV["GameComputeWait"]) & (game["c"] != 2)]
    dd = game[game["type"] == EV["GameDingDong"]]
    sf = game[game["type"] == EV["GameSubmitFrame"]]
    fh, fw, fm, gc = sel("FenceHeld"), sel("FenceWrite"), sel("FlipMarker"), sel("GpuCompleted")
    aqs = sel("AqSubmit")
    pres = sel("Present")
    # GPU batches: value -> (GPU start, end) on the TSC, lane
    comp_vals = gc["a"].astype(np.int64)
    comp_ts = gc["t0"].astype(np.int64)
    o = np.argsort(comp_vals, kind="stable")
    conv, info = pa.calibrate(tr, (list(comp_vals[o]), list(comp_ts[o])))
    if conv is None:
        raise SystemExit("no GPU calibration")
    b = tr["batches"]
    gpu = {int(x["value"]): (float(conv(x["gpu_t0"])), float(conv(x["gpu_t1"])), int(x["pad"]) & 1) for x in b}
    print(f"trace {path}: calibration {info.get('method')}; batches {len(b)} (lane {sum(1 for v in gpu.values() if v[2])}),"
          f" AqSubmit {len(aqs)}, waits {len(cw)} (blocked {int((cw['c'] == 1).sum())}), flips {len(fm)}, presents {len(pres)}")
    if len(aqs) == 0:
        print("  (no AqSubmit events: not an R10 build)")
    aq_t1 = aqs["t1"].astype(np.int64)
    # the GPU's busy union per main/lane to see what the lane batch ran beside
    fm_t = fm["t0"].astype(np.int64)
    pr_t = pres["t0"].astype(np.int64)
    # the frame log beside the trace: game frames (game_submit_ms) and their presents
    fl_rows = list(csv.DictReader(open(os.path.join(os.path.dirname(path), "gpu_frames.csv"), newline="")))
    gsub = [float(r["game_submit_ms"]) for r in fl_rows]
    pres_ms = [float(r["present_ms"]) for r in fl_rows]
    rows = []
    sf_t1 = sf["t1"].astype(np.int64)
    cw_t0 = cw["t0"].astype(np.int64)
    for w in cw:
        lab, val = int(w["a"]), int(w["b"]) & 0xffffffff
        t_w0, t_w1 = int(w["t0"]), int(w["t1"])
        h = fh[(fh["a"] == lab) & ((fh["b"] & 0xffffffff) == val)]
        x = fw[(fw["a"] == lab) & ((fw["b"] & 0xffffffff) == val)]
        if not len(x):
            continue
        q = int(x[0]["c"])
        t_fw = int(x[0]["t0"])
        t_fh = int(h[0]["t0"]) if len(h) else 0
        k = np.searchsorted(dd["t0"], t_w0) - 1
        t_dd = int(dd[k]["t0"]) if k >= 0 else 0
        # the batch with the fence: the first submission with this queue at or after FenceHeld
        sub = None
        if t_fh:
            j = np.searchsorted(aq_t1, t_fh)
            while j < len(aqs):
                c = int(aqs[j]["c"])
                if (c >> 8) & (1 << q if q < 8 else 0x80):
                    sub = aqs[j]
                    break
                j += 1
        # frame N: the game frame whose SubmitFrame came before the wait (the frame log's game_submit_ms - the trace's
        # time base is the frame log's), its flip marker (b = the frame's index) and its flip batch's GPU end
        n_frame = bisect.bisect_left(gsub, ms(t_w0)) - 1
        gN_end = np.nan
        kf = -1
        if n_frame >= 0:
            m = np.nonzero(fm["b"] == n_frame)[0]
            if len(m):
                kf = int(m[0])
                g = gpu.get(int(fm[kf]["a"]))
                gN_end = g[1] if g else np.nan
        nf = np.searchsorted(fm_t, t_fh) if t_fh else 0
        # A or B: the waits since the last SubmitFrame's return (wait B is deferred under REA-3)
        ks = np.searchsorted(sf_t1, t_w0) - 1
        since = int(((cw_t0 >= (sf_t1[ks] if ks >= 0 else 0)) & (cw_t0 < t_w0)).sum())
        r = {"t": ms(t_w0), "kind": since, "blocked": int(w["c"]), "queue": q, "wait": d(t_w0, t_w1),
             "dd_cp": d(t_dd, t_fh) if t_dd and t_fh else np.nan,
             "fh_after_flip": d(int(fm_t[kf]), t_fh) if kf >= 0 and t_fh else np.nan, "frame": n_frame}
        if sub is not None:
            v = int(sub["a"])
            c = int(sub["c"])
            r.update({"value": v, "lane": c & 1, "flags": c & 0xff, "waitv": int(sub["b"]),
                      "cp_sub": d(t_fh, int(sub["t1"])), "open_sub": d(int(sub["t0"]), int(sub["t1"]))})
            g = gpu.get(v)
            if g:
                r.update({"sub_gs": d(int(sub["t1"]), g[0]), "gpu": d(g[0], g[1]), "ge_fw": d(g[1], t_fw),
                          "ge_vs_gN": (g[1] - gN_end) / hz * 1e3 if not np.isnan(gN_end) else np.nan})
            if c & 2:
                wm = gpu.get(int(sub["b"]))
                if wm:
                    r["waited_main_end_vs_sub"] = d(int(sub["t1"]), wm[1])
        r["fw_we"] = d(t_fw, t_w1)
        # the present interval of frame N (the frame log: present_ms of N minus N-1)
        if 0 < n_frame < len(pres_ms):
            r["present_dt"] = pres_ms[n_frame] - pres_ms[n_frame - 1]
        rows.append(r)
    if not rows:
        raise SystemExit("no waits matched")
    keys = ["t", "kind", "blocked", "wait", "dd_cp", "fh_after_flip", "cp_sub", "sub_gs", "gpu", "ge_fw", "fw_we", "ge_vs_gN",
            "lane", "flags", "waited_main_end_vs_sub", "present_dt"]
    print("  t ms   kind(0=A) | blk | waitA | dd->cp | cp - flipmarker(N) | cp->sub | sub->gs | gpu | ge->fw | fw->we | C1 end - G(N) end |"
          " lane fl | main waited ends +ms | present dt")
    fmt = lambda v, w=6, p=1: (f"{v:{w}.{p}f}" if isinstance(v, float) and not np.isnan(v) else
                               f"{v:{w}d}" if isinstance(v, int) else " " * (w - 1) + "-")
    for r in rows[:rows_n]:
        print(" ".join(fmt(r.get(k, np.nan)) for k in keys))
    allrows = rows
    rows = [r for r in rows if r["kind"] == 0]  # wait A only below
    print(f"wait A rows {len(rows)} of {len(allrows)}")
    a = {k: np.array([r.get(k, np.nan) for r in rows], dtype=float) for k in keys}
    print("medians / p90 (ms):")
    for k in ["wait", "dd_cp", "cp_sub", "sub_gs", "gpu", "ge_fw", "fw_we", "ge_vs_gN", "fh_after_flip"]:
        v = a[k][~np.isnan(a[k])]
        if len(v):
            print(f"  {k:14s} n {len(v):4d} median {np.median(v):7.2f} p90 {np.percentile(v, 90):7.2f} mean {v.mean():7.2f}")
    lane = a["lane"]
    fl = np.nan_to_num(a["flags"], nan=0)
    n_ok = int((~np.isnan(lane)).sum())
    print(f"  C1 batches found {n_ok}: on the lane {int((lane == 1).sum())}, waited for a main batch "
          f"{int(((fl.astype(np.int64) & 2) != 0).sum())} (range {int(((fl.astype(np.int64) & 4) != 0).sum())}, "
          f"all {int(((fl.astype(np.int64) & 8) != 0).sum())}, shared {int(((fl.astype(np.int64) & 16) != 0).sum())}, "
          f"images {int(((fl.astype(np.int64) & 32) != 0).sum())})")
    late = a["ge_vs_gN"]
    v = late[~np.isnan(late)]
    print(f"  C1 GPU end after G(N)'s GPU end: {int((v > 0).sum())} of {len(v)}")
    # split by the present interval: which steps differ between short and long frames
    pdt = a["present_dt"]
    for name, m in (("present < 22 ms", pdt < 22), ("present 22-36", (pdt >= 22) & (pdt <= 36)), ("present > 36 ms", pdt > 36)):
        if m.sum() == 0:
            continue
        s = " ".join(f"{k} {np.nanmedian(a[k][m]):6.2f}" for k in ["wait", "dd_cp", "cp_sub", "sub_gs", "gpu", "ge_fw",
                                                                   "ge_vs_gN"])
        print(f"  [{name:15s}] n {int(m.sum()):4d}: {s}")
    if out_csv:
        with open(out_csv, "w", newline="") as f:
            wr = csv.DictWriter(f, fieldnames=keys + ["queue", "value", "waitv", "open_sub"], extrasaction="ignore")
            wr.writeheader()
            for r in allrows:
                wr.writerow({k: r.get(k, "") for k in wr.fieldnames})


if __name__ == "__main__":
    main(sys.argv)
