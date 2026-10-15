r"""SecondSonPC port optimizations: the summary of a port_ab.ps1 series.

usage: py -I -X utf8 port_ab_summary.py <series dir>

<series dir>\<config>_r<n>\<scene>_1\win.json (tools\frame_stats.py over the scene's window: village 25..50 s, beach
62..88 s), cpu_mem.csv (the process's CPU time and memory once a second) and SecondSonPC.log (VRAM samples). Per scene
and configuration: mean, standard deviation and range over the repeats; every other configuration against the first
one (the baseline): the difference of the means and Welch's t. |t| < 2 is not called a measured change.

The times overlap (frame_log.h): cp is the GPU thread's busy time, record its part spent recording into Vulkan (single
thread); with the parallel processor the recording runs on its own thread (par_busy), par_wait is the processor's wait
for that thread's queue. gpu_busy comes from the GPU's timestamps of finished batches. None of them add up to the frame.
Writes summary.md and summary.json next to the series.
"""
import csv
import json
import math
import os
import re
import statistics
import sys
from collections import defaultdict

FIELDS = [
    ("avg_fps", "Average FPS"),
    ("low1_fps", "1% Low FPS"),
    ("ft_p50_ms", "P50 frametime, ms"),
    ("ft_p95_ms", "P95 frametime, ms"),
    ("ft_p99_ms", "P99 frametime, ms"),
    ("mean_cp_ms", "CP busy (decode+native ops), ms"),
    ("mean_par_busy_ms", "Vulkan recording thread busy, ms"),
    ("mean_record_ms", "record (single-thread part), ms"),
    ("mean_par_wait_ms", "CP waits for recording queue, ms"),
    ("mean_empty_ms", "CP idle (no work), ms"),
    ("cp_after_game_ms", "game submit -> CP done, ms"),
    ("mean_gpu_busy_ms", "GPU busy (timestamps), ms"),
    ("gpu_tail_ms", "GPU tail after CP, ms"),
    ("mean_draws", "draws / frame"),
    ("mean_dispatches", "dispatches / frame"),
    ("mean_par_drains", "recording queue drains / frame"),
    ("gpu_load_pct", "GPU load (busy / frame), %"),
    ("cpu_pct", "process CPU, % of one core"),
    ("ws_max_mib", "RAM working set max, MiB"),
    ("vram_mib", "VRAM (last sample), MiB"),
    ("v1_pct", "frames of 1 vblank, %"),
]
WINDOWS = {"user1022": (25, 50), "beach": (62, 88), "early": (62, 88)}


def cpu_mem(path, t0, t1):
    rows = []
    try:
        with open(path, newline="") as f:
            for r in csv.DictReader(f):
                rows.append((float(r["t_s"]), float(r["cpu_s"]), float(r["working_set_mib"])))
    except (OSError, KeyError, ValueError):
        return None, None
    # the sampler's clock starts a few seconds before the game's (process start): the window is approximate
    inside = [r for r in rows if t0 <= r[0] <= t1]
    cpu = None
    if len(inside) >= 2 and inside[-1][0] > inside[0][0]:
        cpu = 100.0 * (inside[-1][1] - inside[0][1]) / (inside[-1][0] - inside[0][0])
    ws = max((r[2] for r in rows), default=None)
    return cpu, ws


def vram(log):
    last = None
    try:
        with open(log, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = re.search(r"VRAM sample: local usage ([0-9]+) MiB", line)
                if m:
                    last = float(m.group(1))
    except OSError:
        pass
    return last


def welch(a, b):
    if len(a) < 2 or len(b) < 2:
        return None
    va, vb = statistics.variance(a), statistics.variance(b)
    se = math.sqrt(va / len(a) + vb / len(b))
    if se == 0:
        return None
    return (statistics.mean(b) - statistics.mean(a)) / se


def main(series):
    data = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))  # scene -> config -> field -> values
    order = []
    for d in sorted(os.listdir(series)):
        m = re.fullmatch(r"(.+)_r(\d+)", d)
        if not m or not os.path.isdir(os.path.join(series, d)):
            continue
        cfg = m.group(1)
        if cfg not in order:
            order.append(cfg)
        for sd in os.listdir(os.path.join(series, d)):
            sm = re.fullmatch(r"(.+)_1", sd)
            run = os.path.join(series, d, sd)
            if not sm or not os.path.isfile(os.path.join(run, "win.json")):
                continue
            scene = sm.group(1)
            with open(os.path.join(run, "win.json"), encoding="utf-8") as f:
                s = json.load(f)
            bins = s.get("ft_bins") or {}
            total = sum(bins.values()) or 0
            if total:
                s["v1_pct"] = 100.0 * bins.get("<=1v", 0) / total
            if s.get("mean_gpu_busy_ms") and s.get("ft_mean_ms"):
                s["gpu_load_pct"] = 100.0 * s["mean_gpu_busy_ms"] / s["ft_mean_ms"]
            t0, t1 = WINDOWS.get(scene, (62, 88))
            s["cpu_pct"], s["ws_max_mib"] = cpu_mem(os.path.join(run, "cpu_mem.csv"), t0, t1)
            s["vram_mib"] = vram(os.path.join(run, "SecondSonPC.log"))
            for k, _ in FIELDS:
                v = s.get(k)
                if isinstance(v, (int, float)) and not (isinstance(v, float) and math.isnan(v)):
                    data[scene][cfg][k].append(float(v))
    # the baseline: the first configuration declared in series.txt
    declared = []
    try:
        with open(os.path.join(series, "series.txt"), encoding="utf-8-sig", errors="replace") as f:
            declared = [m.group(1) for m in re.finditer(r"^\s+(\S+) = ", f.read(), re.M)]
    except OSError:
        pass
    order = [c for c in declared if c in order] + [c for c in order if c not in declared]
    base = order[0] if order else None
    out = {"series": series, "base": base, "scenes": {}}
    lines = [f"# Port A/B: {series}", "",
             f"baseline: `{base}`; values: median [min..max] (n), mean ± sd; Δ: the medians' difference vs the baseline "
             f"(%), Welch t of the means (|t| < 2: not a measured change)", ""]
    for scene in sorted(data):
        w = WINDOWS.get(scene, (62, 88))
        lines += [f"## {scene} (window {w[0]}..{w[1]} s)", ""]
        cfgs = [c for c in order if c in data[scene]]
        lines.append("| metric | " + " | ".join(cfgs) + " |")
        lines.append("|---|" + "---|" * len(cfgs))
        out["scenes"][scene] = {}
        for k, label in FIELDS:
            cells = []
            for c in cfgs:
                v = data[scene][c].get(k, [])
                if not v:
                    cells.append("—")
                    continue
                mu = statistics.mean(v)
                med = statistics.median(v)
                sd = statistics.stdev(v) if len(v) > 1 else 0.0
                cell = f"{med:.2f} [{min(v):.2f}..{max(v):.2f}] ({len(v)}), {mu:.2f} ± {sd:.2f}"
                entry = {"median": med, "mean": mu, "sd": sd, "n": len(v), "values": v}
                if c != base and data[scene][base].get(k):
                    b = data[scene][base][k]
                    bmed = statistics.median(b)
                    d = med - bmed
                    t = welch(b, v)
                    pct = 100.0 * d / bmed if bmed else float("nan")
                    cell += f"<br>Δ {d:+.2f} ({pct:+.1f} %)" + (f", t {t:+.1f}" if t is not None else "")
                    entry.update({"delta": d, "delta_pct": pct, "t": t})
                out["scenes"][scene].setdefault(c, {})[k] = entry
                cells.append(cell)
            lines.append(f"| {label} | " + " | ".join(cells) + " |")
        lines.append("")
    with open(os.path.join(series, "summary.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    with open(os.path.join(series, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1)
    print("\n".join(lines))


if __name__ == "__main__":
    main(sys.argv[1])
