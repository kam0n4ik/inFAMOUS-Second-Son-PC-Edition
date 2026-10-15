r"""SecondSonPC P5.4: the frame lifecycle of one run - who holds the start of the next frame.

usage: py -I -X utf8 tools\p54\p54_lifecycle.py <run dir> [--json out.json]

The run dir holds p412_trace.bin (SSPC_P412_STALL_TRACE=deep,... with SSPC_P54_FRAME_TRACE=1: the engine's SubmitFrame
and async compute waits, sceGnmSubmitDone / DingDong, the command processor's fence holds and writes) and gpu_frames.csv
(frame_log.*). Per game frame k (one SubmitFrame of the engine's main loop 0x29c8d0):

  frame begin      SubmitFrame(k-1) returned (the main loop's next pass: update, compute submissions, recording)
  compute waits    0x1695e0 in EndFrame (through 0x3edef0 / 0x3ee470): the label of the particles' async compute queue;
                   the wake-up point is the command processor's write of that label (FenceWrite, queue id), which
                   follows the completion of a GPU batch; the label was held from FenceHeld (the processor reached the
                   RELEASE_MEM) to that batch
  SubmitFrame(k)   step 4 sceGnmSubmitDone, step 5 the wait for the previous frame's EOP label (its wake-up: the write of
                   the frame label with fence(k-1)), step 6 sceGnmSubmitAndFlip (the command processor gets frame k),
                   step 7 the wait while more than one flip is pending (until the previous flip is shown)
  CP / Vulkan      the flip marker of frame k on thread 1 (gpu_frames.csv cp_flip), the batches recorded for it (thread
                   2, GPU start/end converted to the TSC), the frame's EOP (gpu_done), the present
The blocker of frame k+1's submission is the last of: the game's own readiness (its compute waits, its CPU work) and
the EOP of frame k.
"""
import bisect
import collections
import json
import os
import statistics as st
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "p412"))
import p412_analyze as pa  # noqa: E402  (load_trace, calibrate, read_frames_csv, EV)

EV = pa.EV
WAIT_T1 = {EV[n] for n in ("IdleEmpty", "IdleGpu", "IdleLabel", "IdleFlag", "Drain", "QueueFull", "JournalFull",
                           "GpuSync", "RingWait", "BindWait", "QueueLock", "FlipAcquire")}
WAIT_T2 = {EV[n] for n in ("RecordWait", "GpuSync", "RingWait", "BindWait", "QueueLock", "FlipAcquire")}


def pct(v, q):
    if not v:
        return float("nan")
    s = sorted(v)
    return s[min(len(s) - 1, max(0, int(round(q * (len(s) - 1)))))]


def stat(v):
    v = [x for x in v if x is not None and x == x]
    if not v:
        return {"n": 0}
    return {"n": len(v), "mean": st.fmean(v), "p50": pct(v, 0.5), "p95": pct(v, 0.95), "max": max(v)}


def merged(iv):
    out = []
    for a, b in sorted(iv):
        if b <= a:
            continue
        if out and a <= out[-1][1]:
            out[-1] = (out[-1][0], max(out[-1][1], b))
        else:
            out.append((a, b))
    return out


def union_len(iv, lo, hi):
    """Length of the union of intervals clipped to [lo, hi]."""
    return sum(b - a for a, b in merged([(max(a, lo), min(b, hi)) for a, b in iv]))


def intersect(a_iv, b_iv):
    """Pairwise intersection of two sorted lists of disjoint intervals."""
    out, i, j = [], 0, 0
    while i < len(a_iv) and j < len(b_iv):
        lo, hi = max(a_iv[i][0], b_iv[j][0]), min(a_iv[i][1], b_iv[j][1])
        if hi > lo:
            out.append((lo, hi))
        if a_iv[i][1] < b_iv[j][1]:
            i += 1
        else:
            j += 1
    return out


def busy_from_waits(waits, lo, hi):
    """[lo, hi] minus the waits: the thread's busy intervals."""
    out, cur = [], lo
    for a, b in merged([(max(a, lo), min(b, hi)) for a, b in waits]):
        if a > cur:
            out.append((cur, a))
        cur = max(cur, b)
    if hi > cur:
        out.append((cur, hi))
    return out


def main(argv):
    run = argv[1]
    out_json = argv[argv.index("--json") + 1] if "--json" in argv else None
    tr = pa.load_trace(os.path.join(run, "p412_trace.bin"))
    hz = tr["tsc_hz"]
    start = tr["start_tsc"]

    def ms(t):  # TSC -> ms of the frame log's time
        return (float(t) - start) * 1000.0 / hz

    def tsc(m):
        return start + m / 1000.0 * hz

    th = {r: [t for t in tr["threads"] if t["role"] == r] for r in pa.ROLES}
    cp = max(th["CP"], key=lambda t: len(t["ev"]))
    rec = max(th["Record"], key=lambda t: len(t["ev"])) if th["Record"] else None
    by = collections.defaultdict(list)
    for t in tr["threads"]:
        for row in t["ev"]:
            by[int(row["type"])].append((int(row["t0"]), int(row["t1"]), int(row["a"]), int(row["b"]),
                                         int(row["type"]), int(row["c"]), t["tid"], t["role"]))
    for k in by:
        by[k].sort()

    submit_frames = by[EV["GameSubmitFrame"]]
    if not submit_frames:
        raise SystemExit("no GameSubmitFrame events: run with SSPC_P54_FRAME_TRACE=1 and SSPC_P412_STALL_TRACE=deep")
    main_tid = collections.Counter(x[6] for x in submit_frames).most_common(1)[0][0]
    submit_frames = [x for x in submit_frames if x[6] == main_tid]
    flips = [x for x in by[EV["GameSubmitFlip"]] if x[6] == main_tid]
    dones = [x for x in by[EV["GameSubmitDone"]] if x[6] == main_tid]
    earlies = [x for x in by[EV["GameEarlySubmit"]] if x[6] == main_tid]  # SSPC_P54_EARLY_SUBMIT=1: inside SubmitDone
    cwaits = by[EV["GameComputeWait"]]
    dings = by[EV["GameDingDong"]]
    held = by[EV["FenceHeld"]]
    writes = by[EV["FenceWrite"]]
    completed = sorted((x[0], x[2]) for x in by[EV["GpuCompleted"]] if x[6] == cp["tid"])
    comp_t = [c[0] for c in completed]

    # GPU batches in TSC
    vals = np.array([c[1] for c in completed], dtype=np.int64)
    order = np.argsort(vals, kind="stable")
    conv, cal_info = pa.calibrate(tr, (vals[order].tolist(), np.array(comp_t, dtype=np.float64)[order].tolist()))
    batches = []
    for row in tr["batches"]:
        g0 = float(conv(row["gpu_t0"])) if conv else None
        g1 = float(conv(row["gpu_t1"])) if conv else None
        batches.append({"value": int(row["value"]), "s0": int(row["submit_t0"]), "s1": int(row["submit_t1"]),
                        "g0": g0, "g1": g1, "frame": int(row["frame"])})
    batches.sort(key=lambda x: x["value"])
    bvals = [x["value"] for x in batches]
    gpu_iv = merged([(x["g0"], x["g1"]) for x in batches if x["g0"] is not None])

    wr_by_addr = collections.defaultdict(list)
    for x in writes:
        wr_by_addr[x[2]].append(x)
    held_by_key = {}
    for x in held:
        held_by_key.setdefault((x[2], x[3]), []).append(x)

    def wake(addr, target, t_from, t_to):
        """The first write of `addr` with a value >= target (wrapping u32) at or before t_to (+2 ms), after t_from - 0.5 s."""
        for x in wr_by_addr.get(addr, []):
            if x[0] < t_from - int(hz * 0.5):
                continue
            if x[0] > t_to + int(hz * 0.002):
                break
            if ((x[3] - target) & 0xFFFFFFFF) < 0x80000000:
                return x
        return None

    def batch_before(t):
        """The batch whose completion thread 1 saw last before t."""
        i = bisect.bisect_right(comp_t, t) - 1
        if i < 0:
            return None
        v = completed[i][1]
        j = bisect.bisect_left(bvals, v)
        return batches[j] if j < len(batches) and bvals[j] == v else None

    frames_csv = pa.read_frames_csv(os.path.join(run, "gpu_frames.csv"))
    flip_enter = sorted((r["flip_enter_ms"], i) for i, r in enumerate(frames_csv) if r.get("flip_enter_ms") is not None)
    fe_t = [x[0] for x in flip_enter]
    win_lo, win_hi = ms(tr["win0"]), ms(tr["win1"])

    def csv_row(t_ms, tol=0.2):
        i = bisect.bisect_left(fe_t, t_ms - tol)
        if i < len(fe_t) and abs(fe_t[i] - t_ms) <= tol:
            return frames_csv[flip_enter[i][1]]
        return None

    cp_w = [(int(r["t0"]), int(r["t1"])) for r in cp["ev"] if int(r["type"]) in WAIT_T1]
    rec_w = [(int(r["t0"]), int(r["t1"])) for r in rec["ev"] if int(r["type"]) in WAIT_T2] if rec else []

    rows = []
    for k in range(1, len(submit_frames)):
        sf, prev = submit_frames[k], submit_frames[k - 1]
        begin = prev[1]
        r = {"k": k, "frame_begin_ms": ms(begin), "submit_frame_enter_ms": ms(sf[0]), "submit_frame_return_ms": ms(sf[1]),
             "fence": sf[3], "fence_prev": prev[3], "game_cpu_to_submitframe_ms": (sf[0] - begin) * 1000 / hz}
        cw = [x for x in cwaits if begin <= x[0] < sf[0] and x[6] == main_tid]
        r["compute_waits"] = len(cw)
        r["compute_wait_ms"] = sum((x[1] - x[0]) for x in cw) * 1000 / hz
        r["compute_wait_detail"] = []
        for x in cw:
            d = {"t0_ms": ms(x[0]), "ms": (x[1] - x[0]) * 1000 / hz, "label": f"{x[2]:#x}", "target": x[3],
                 "waited": x[5]}
            w = wake(x[2], x[3], x[0], x[1]) if x[5] else None
            if w:
                d["wake_write_ms"] = ms(w[0])
                d["wake_queue"] = w[5]
                hs = [h for h in held_by_key.get((x[2], x[3]), []) if h[0] <= w[0]]
                if hs:
                    d["held_ms"] = ms(hs[-1][0])  # the processor reached the RELEASE_MEM
                bb = batch_before(w[0] + 1)
                if bb:
                    d["batch"] = bb["value"]
                    d["batch_submit_ms"] = ms(bb["s1"])
                    if bb["g1"] is not None:
                        d["batch_gpu_start_ms"] = ms(bb["g0"])
                        d["batch_gpu_end_ms"] = ms(bb["g1"])
                dd = [y for y in dings if y[0] <= x[0]]
                if dd:
                    d["last_dingdong_ms"] = ms(dd[-1][0])
                    d["dingdong_vqid"] = dd[-1][2]
            r["compute_wait_detail"].append(d)
        r["dingdongs"] = sum(1 for y in dings if begin <= y[0] < sf[0])
        fl = [x for x in flips if sf[0] <= x[0] <= sf[1]]
        dn = [x for x in dones if sf[0] <= x[0] <= sf[1]]
        r["submit_dones"] = len(dn)
        ea = [x for x in earlies if sf[0] <= x[0] <= sf[1]]
        r["early"] = bool(ea)
        if fl:
            f = fl[0]
            r["submit_and_flip_enter_ms"], r["submit_and_flip_return_ms"] = ms(f[0]), ms(f[1])
            # the command processor gets frame k here (the flip row of gpu_frames.csv starts here too)
            r["handoff_ms"] = ms(ea[0][0]) if ea else ms(f[0])
            before = [x for x in dn if x[1] <= f[0]]
            step5_from = before[-1][1] if before else sf[0]
            r["step5_wait_ms"] = (f[0] - step5_from) * 1000 / hz
            r["step7_wait_ms"] = (sf[1] - f[1]) * 1000 / hz
            w5 = wake(sf[2], prev[3], step5_from, f[0])
            if w5:
                r["eop_prev_write_ms"] = ms(w5[0])
                r["eop_prev_queue"] = w5[5]
                r["eop_prev_to_flip_ms"] = (f[0] - w5[0]) * 1000 / hz
            row = csv_row(r["handoff_ms"])
            if row:
                r["n"] = int(row["n"])
                for key in ("cp_flip_ms", "gpu_done_ms", "present_ms", "game_flip_ms"):
                    r[key] = row.get(key)
        rows.append(r)

    by_n = {r["n"]: r for r in rows if "n" in r}
    for r in rows:
        n = r.get("n")
        nxt = by_n.get(n + 1) if n is not None else None
        if nxt is None or "submit_and_flip_enter_ms" not in nxt or r.get("cp_flip_ms") is None or \
                r.get("gpu_done_ms") is None:
            continue
        r["cp_ms"] = r["cp_flip_ms"] - r["game_flip_ms"]
        r["gpu_tail_ms"] = r["gpu_done_ms"] - r["cp_flip_ms"]
        r["next_ready_minus_eop_ms"] = nxt["submit_frame_enter_ms"] - r["gpu_done_ms"]
        r["next_submit_minus_eop_ms"] = nxt["submit_and_flip_enter_ms"] - r["gpu_done_ms"]
        r["period_ms"] = nxt["submit_and_flip_enter_ms"] - r["submit_and_flip_enter_ms"]
        # the command processor starts frame k+1 at max(its hand-off, cp_flip(k)): < 0 = handed on while k was recorded
        r["next_handoff_minus_cp_flip_ms"] = nxt["handoff_ms"] - r["cp_flip_ms"]
        prev_row = by_n.get(n - 1)
        if prev_row is not None and prev_row.get("cp_flip_ms") is not None:
            r["cp_own_ms"] = r["cp_flip_ms"] - max(r["handoff_ms"], prev_row["cp_flip_ms"])
        last_cw_end = max((d["t0_ms"] + d["ms"] for d in nxt["compute_wait_detail"]), default=None)
        if nxt["early"]:
            if nxt["handoff_ms"] <= r["cp_flip_ms"] + 0.15:
                r["blocker"] = "early: the CP still on frame k (k+1 handed on before cp_flip(k))"
            elif nxt["compute_wait_ms"] > 0.5 and last_cw_end is not None and \
                    nxt["submit_frame_enter_ms"] - last_cw_end < 1.0:
                r["blocker"] = "early: the game late - async compute label (0x1695e0)"
            elif r.get("step5_wait_ms", 0) > 0.15:
                r["blocker"] = "early: the game late - it began k+1 after EOP(k-1) (SubmitFrame(k) step 5)"
            else:
                r["blocker"] = "early: the game late - its CPU work"
        elif nxt.get("step5_wait_ms", 0) > 0.15:
            r["blocker"] = "EOP(k): engine SubmitFrame step 5 (guest frame label)"
        elif nxt["compute_wait_ms"] > 0.5 and last_cw_end is not None and nxt["submit_frame_enter_ms"] - last_cw_end < 1.0:
            r["blocker"] = "async compute label: engine EndFrame 0x1695e0"
        elif nxt.get("step7_wait_ms", 0) > 1.0:
            r["blocker"] = "flip pending (vblank): engine SubmitFrame step 7"
        else:
            r["blocker"] = "the game's CPU work"
        lo, hi = tsc(r["submit_and_flip_enter_ms"]), tsc(nxt["submit_and_flip_enter_ms"])
        fb = [x for x in batches if x["frame"] == n and x["g0"] is not None]
        if fb:
            r["batches"] = len(fb)
            r["gpu_first_start_after_submit_ms"] = ms(min(x["g0"] for x in fb)) - r["handoff_ms"]
        gpu_busy = merged([(max(a, lo), min(b, hi)) for a, b in gpu_iv if b > lo and a < hi])
        cp_busy = busy_from_waits(cp_w, lo, hi)
        rec_busy = busy_from_waits(rec_w, lo, hi) if rec else []
        per = (hi - lo) * 1000 / hz
        cpu_any = merged(cp_busy + rec_busy)
        r["gpu_busy_ms"] = union_len(gpu_busy, lo, hi) * 1000 / hz
        r["cp_busy_ms"] = union_len(cp_busy, lo, hi) * 1000 / hz
        r["rec_busy_ms"] = union_len(rec_busy, lo, hi) * 1000 / hz
        r["cpu_gpu_overlap_ms"] = union_len(intersect(cpu_any, gpu_busy), lo, hi) * 1000 / hz
        r["all_idle_ms"] = per - union_len(cpu_any + gpu_busy, lo, hi) * 1000 / hz
        cpf, gd = tsc(r["cp_flip_ms"]), tsc(r["gpu_done_ms"])
        r["gpu_idle_until_cp_flip_ms"] = ((cpf - lo) - union_len(gpu_busy, lo, cpf)) * 1000 / hz
        r["gpu_busy_in_tail_ms"] = union_len(gpu_busy, cpf, gd) * 1000 / hz
        r["cp_busy_in_tail_ms"] = union_len(cp_busy, cpf, gd) * 1000 / hz

    done = [r for r in rows if "blocker" in r]
    summ = {"run": run, "window_ms": [win_lo, win_hi], "frames": len(done), "calibration": cal_info,
            "threads": {"main": main_tid, "cp": cp["tid"], "record": rec["tid"] if rec else None,
                        "dingdong": sorted({y[6] for y in dings}), "fence_write": sorted({x[6] for x in writes})},
            "blockers": dict(collections.Counter(r["blocker"] for r in done)),
            "early_frames": sum(1 for r in done if r["early"])}
    keys = ["period_ms", "game_cpu_to_submitframe_ms", "compute_waits", "compute_wait_ms", "dingdongs", "submit_dones",
            "step5_wait_ms", "step7_wait_ms", "eop_prev_to_flip_ms", "cp_ms", "gpu_tail_ms", "next_ready_minus_eop_ms",
            "next_submit_minus_eop_ms", "next_handoff_minus_cp_flip_ms", "cp_own_ms", "batches",
            "gpu_first_start_after_submit_ms", "gpu_busy_ms", "cp_busy_ms",
            "rec_busy_ms", "cpu_gpu_overlap_ms", "all_idle_ms", "gpu_idle_until_cp_flip_ms", "gpu_busy_in_tail_ms",
            "cp_busy_in_tail_ms"]
    summ["per_frame"] = {k: stat([r.get(k) for r in done]) for k in keys}
    cw = [d for r in done for d in r["compute_wait_detail"] if d.get("waited") and "wake_write_ms" in d]
    chain = {
        "game wait (0x1695e0)": [d["ms"] for d in cw],
        "last DingDong -> CP holds the RELEASE_MEM": [d["held_ms"] - d["last_dingdong_ms"] for d in cw
                                                      if "held_ms" in d and "last_dingdong_ms" in d],
        "held -> its batch submitted": [d["batch_submit_ms"] - d["held_ms"] for d in cw
                                        if "held_ms" in d and "batch_submit_ms" in d],
        "batch submitted -> GPU start": [d["batch_gpu_start_ms"] - d["batch_submit_ms"] for d in cw
                                         if "batch_gpu_start_ms" in d],
        "GPU start -> GPU end": [d["batch_gpu_end_ms"] - d["batch_gpu_start_ms"] for d in cw if "batch_gpu_end_ms" in d],
        "GPU end -> label written": [d["wake_write_ms"] - d["batch_gpu_end_ms"] for d in cw if "batch_gpu_end_ms" in d],
        "label written -> game wakes": [d["t0_ms"] + d["ms"] - d["wake_write_ms"] for d in cw],
    }
    summ["compute_wait_chain"] = {k: stat(v) for k, v in chain.items()}
    summ["compute_wake_queues"] = dict(collections.Counter(d.get("wake_queue") for d in cw))
    summ["eop_wake_queues"] = dict(collections.Counter(r.get("eop_prev_queue") for r in done))
    infl = []
    for r in frames_csv:
        t = r.get("game_flip_ms")
        if t is None or not (win_lo <= t <= win_hi):
            continue
        infl.append(sum(1 for q in frames_csv if q.get("game_flip_ms") and q["game_flip_ms"] <= t and
                        (q.get("gpu_done_ms") is None or q["gpu_done_ms"] > t)))
    summ["frames_in_flight_at_submit"] = dict(collections.Counter(infl))

    lines = [f"# P5.4 frame lifecycle: {run}", "",
             f"window {win_lo / 1000:.1f}..{win_hi / 1000:.1f} s, frames {len(done)}; GPU clock: {cal_info.get('method')}",
             f"threads: main {main_tid}, CP {cp['tid']}, record {summ['threads']['record']}, DingDong "
             f"{summ['threads']['dingdong']}, fence writes {summ['threads']['fence_write']}; frames handed on early "
             f"(SSPC_P54_EARLY_SUBMIT): {summ['early_frames']}", "",
             "## What held the next frame's submission", ""]
    for k, v in sorted(summ["blockers"].items(), key=lambda kv: -kv[1]):
        lines.append(f"- {k}: {v} frames ({100.0 * v / max(len(done), 1):.1f} %)")
    lines += ["", "## Per frame (ms; the frame = one submission of the game to the next)", "",
              "| metric | mean | p50 | p95 | max |", "|---|---|---|---|---|"]
    for k in keys:
        s = summ["per_frame"][k]
        if s["n"]:
            lines.append(f"| {k} | {s['mean']:.2f} | {s['p50']:.2f} | {s['p95']:.2f} | {s['max']:.2f} |")
    lines += ["", "## The async compute wait (0x1695e0): from the game's DingDong to its wake-up (ms)", "",
              "| step | n | mean | p50 | p95 |", "|---|---|---|---|---|"]
    for k, s in summ["compute_wait_chain"].items():
        if s["n"]:
            lines.append(f"| {k} | {s['n']} | {s['mean']:.2f} | {s['p50']:.2f} | {s['p95']:.2f} |")
    lines += ["", f"compute labels written by queue: {summ['compute_wake_queues']}; the previous EOP written by queue: "
                  f"{summ['eop_wake_queues']}",
              f"frames in flight when the game submits: {summ['frames_in_flight_at_submit']}"]
    text = "\n".join(lines) + "\n"
    open(os.path.join(run, "p54_lifecycle.md"), "w", encoding="utf-8").write(text)
    with open(os.path.join(run, "p54_frames.json"), "w", encoding="utf-8") as f:
        json.dump(rows, f, indent=0, default=float)
    if out_json:
        json.dump(summ, open(out_json, "w", encoding="utf-8"), indent=1, default=float)
    print(text)


if __name__ == "__main__":
    main(sys.argv)
