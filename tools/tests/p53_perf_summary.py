r"""SecondSonPC P5.3D.3: the summary of a p53_perf_ab.ps1 series.

usage: py -I -X utf8 p53_perf_summary.py <series dir>

<series dir>\<config>_r<n>\<scene>_1\stats.json (tools\frame_stats.py over 62..88 s) and cpu_mem.csv (the process's
CPU time and memory once a second). Per scene and configuration: the mean and the spread (min..max) over the repeats,
and the difference of each configuration's mean from base (or par_base for the parallel ones) with the base's own
spread next to it - a difference inside the spread is not a measured change.

Exclusive vs overlapping times (frame_log.h): cp is the GPU thread's whole busy time per frame; record is part of it
(recording draws and dispatches into Vulkan), callbacks too; cp_excl = cp - record - callbacks is the processor's own
work (packets, register file, native operations). gpu_busy comes from the GPU's timestamps of completed batches and
overlaps all of them. shader is the shader frontend's translation time (part of record when a draw waits for it).
With the parallel processor, record runs on the second thread: par_busy is its time, not part of cp.
"""
import csv
import json
import os
import re
import statistics
import sys
from collections import defaultdict

FIELDS = [
    ("avg_fps", "avg FPS"),
    ("low1_fps", "1% low FPS"),
    ("ft_mean_ms", "frame mean ms"),
    ("ft_p95_ms", "frame p95 ms"),
    ("ft_p99_ms", "frame p99 ms"),
    ("mean_cp_ms", "cp ms (busy, incl. record)"),
    ("cp_excl_ms", "cp excl. ms (packets, regs, native ops)"),
    ("mean_record_ms", "record ms (of cp)"),
    ("mean_par_busy_ms", "par. recording thread ms"),
    ("mean_gpu_busy_ms", "GPU busy ms (timestamps)"),
    # P5.3: the frame is a whole number of vblanks (the flip waits for one); what is not rounded to them
    ("cp_after_game_ms", "cp after the game's submit ms"),
    ("gpu_tail_ms", "GPU tail after cp ms"),
    ("v1_pct", "frames of 1 vblank %"),
    ("mean_draws", "draws / frame (processor)"),
    # P5.3 marker transport (SSPC_P53_NATIVE_DRAW_STATS log lines inside the window)
    ("t_markers", "PM4 markers / frame"),
    ("t_tokens", "run tokens / frame"),
    ("t_described", "NOP descriptions / frame"),
    ("t_native_ms", "cp native-op ms / frame (sampled)"),
    ("cpu_pct", "process CPU % of one core (62..88 s)"),
    ("ws_mib", "working set MiB (max)"),
    ("vram_mib", "VRAM MiB (log)"),
]


def cpu_mem(path, t0=62.0, t1=88.0):
    if not os.path.exists(path):
        return None, None
    text = open(path, encoding="ascii").read().splitlines()
    rows = list(csv.DictReader(text))
    if rows and "pid" in rows[0] and len(text[1].split(",")) == 7:
        # written under a decimal-comma locale: t_s and cpu_s split in two ("12,3") - put them back together
        rows = []
        for line in text[1:]:
            t = line.split(",")
            if len(t) == 7:
                rows.append({"t_s": f"{t[0]}.{t[1]}", "pid": t[2], "cpu_s": f"{t[3]}.{t[4]}", "working_set_mib": t[5],
                             "private_mib": t[6]})
    if len(rows) < 2:
        return None, None
    # the sampler's clock starts before the game: align on the first row with the process (the run's ~0 s); the
    # game's process only (the pid of the most rows: the sampler picks the one with the most private memory)
    if "pid" in rows[0]:
        pids = [r["pid"] for r in rows]
        main_pid = max(set(pids), key=pids.count)
        rows = [r for r in rows if r["pid"] == main_pid]
    base = float(rows[0]["t_s"])
    pts = [(float(r["t_s"]) - base, float(r["cpu_s"]), float(r["working_set_mib"])) for r in rows]
    win = [p for p in pts if t0 <= p[0] <= t1]
    if len(win) < 2:
        return None, max(p[2] for p in pts)
    cpu = (win[-1][1] - win[0][1]) / (win[-1][0] - win[0][0]) * 100.0
    return cpu, max(p[2] for p in pts)


def vram(run_dir):
    log = os.path.join(run_dir, "SecondSonPC.log")
    if not os.path.exists(log):
        return None
    last = None
    with open(log, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.search(r"VRAM[^0-9]*(\d+) MiB", line)
            if m:
                last = int(m.group(1))
    return last


TRANSPORT = re.compile(r"^\[(\d+):(\d+):(\d+)\.\d+\].*native transport per frame .*: processor markers ([\d.]+), "
                       r"run tokens ([\d.]+), descriptions in draw NOPs ([\d.]+), native-operation calls [\d.]+, "
                       r"time ([\d.]+) ms")


def transport(run_dir, t0=62.0, t1=98.0):
    """The means of the transport lines whose 10 s interval lies in the frame window (log time from its first line)."""
    log = os.path.join(run_dir, "SecondSonPC.log")
    if not os.path.exists(log):
        return {}
    first = None
    rows = []
    with open(log, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r"^\[(\d+):(\d+):(\d+)\.\d+\]", line)
            if not m:
                continue
            t = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))
            if first is None:
                first = t
            m = TRANSPORT.match(line)
            if m and t0 + 8 <= (t - first) % 86400 <= t1:
                rows.append([float(m.group(i)) for i in range(4, 8)])
    if not rows:
        return {}
    keys = ["t_markers", "t_tokens", "t_described", "t_native_ms"]
    return {k: statistics.fmean(r[i] for r in rows) for i, k in enumerate(keys)}


def main():
    root = sys.argv[1]
    data = defaultdict(lambda: defaultdict(list))  # (scene, config) -> field -> [values]
    for entry in sorted(os.listdir(root)):
        m = re.match(r"(.+)_r(\d+)$", entry)
        if not m or not os.path.isdir(os.path.join(root, entry)):
            continue
        cfg = m.group(1)
        for scene_dir in os.listdir(os.path.join(root, entry)):
            run = os.path.join(root, entry, scene_dir)
            st = os.path.join(run, "stats.json")
            if not os.path.exists(st):
                continue
            s = json.load(open(st, encoding="utf-8"))
            scene = scene_dir.rsplit("_", 1)[0]
            d = data[(scene, cfg)]
            for k, _ in FIELDS:
                if k in s:
                    d[k].append(s[k])
            d["cp_excl_ms"].append(s.get("mean_cp_ms", 0) - s.get("mean_record_ms", 0) - s.get("mean_callbacks_ms", 0))
            bins = s.get("ft_bins") or {}
            if bins and sum(bins.values()) > 0:
                d["v1_pct"].append(100.0 * bins.get("<=1v", 0) / sum(bins.values()))
            for k, v in transport(run).items():
                d[k].append(v)
            c, ws = cpu_mem(os.path.join(run, "cpu_mem.csv"))
            if c is not None:
                d["cpu_pct"].append(c)
            if ws is not None:
                d["ws_mib"].append(ws)
            v = vram(run)
            if v is not None:
                d["vram_mib"].append(v)
    lines = [f"# P5.3D.3 performance series: {root}", ""]
    out = {}
    for scene in sorted({k[0] for k in data}):
        cfgs = [c for c in ["base", "apply9", "apply19", "apply19s", "par_base", "par_apply19s", "par_ft"]
                if (scene, c) in data]
        cfgs += sorted(c for (s, c) in data if s == scene and c not in cfgs)
        lines += [f"## {scene}", "",
                  "| metric | " + " | ".join(f"{c} (n={len(data[(scene, c)]['avg_fps'])})" for c in cfgs) + " |",
                  "|---|" + "---|" * len(cfgs)]
        for k, label in FIELDS:
            cells = []
            for c in cfgs:
                vals = data[(scene, c)].get(k, [])
                if not vals:
                    cells.append("-")
                    continue
                mean = statistics.fmean(vals)
                ref = "par_base" if c.startswith("par_") else "base"
                diff = ""
                if c != ref and data[(scene, ref)].get(k):
                    rv = data[(scene, ref)][k]
                    d = mean - statistics.fmean(rv)
                    spread = (max(rv) - min(rv)) / 2 if len(rv) > 1 else float("nan")
                    diff = f" ({d:+.2f}; base ±{spread:.2f})"
                cells.append(f"{mean:.2f} [{min(vals):.2f}..{max(vals):.2f}]{diff}")
                out.setdefault(scene, {}).setdefault(c, {})[k] = {"mean": mean, "min": min(vals), "max": max(vals),
                                                                   "n": len(vals)}
            lines.append(f"| {label} | " + " | ".join(cells) + " |")
        lines.append("")
    open(os.path.join(root, "summary.md"), "w", encoding="utf-8").write("\n".join(lines) + "\n")
    json.dump(out, open(os.path.join(root, "summary.json"), "w", encoding="utf-8"), indent=1)
    print("\n".join(lines))


if __name__ == "__main__":
    main()
