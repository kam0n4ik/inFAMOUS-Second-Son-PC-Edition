# R10 pacing: summary of a tools\r10\ab.ps1 series - per scene and configuration, the median [min..max] over the runs
# of: average FPS, 1 % / 0.1 % low (mean of the worst 1 % / 0.1 % present intervals, as tools\frame_stats.py), P50 / P95
# / P99 present interval, the worst one, intervals over 33.3 / 50 / 100 ms (per minute), lag1 (correlation of
# neighbouring present intervals: < 0 alternation), the share of intervals < 22 ms, GPU busy and CP busy per frame
# (the frame log), the game's frame period vs the present intervals (mismatch), device lost / [Critical] / [Error].
# Present intervals: the frame log's present_ms (vkQueuePresentKHR of the free-pacing presenter); the display (DWM at
# 165 Hz, windowed Mailbox) shows each at the next refresh - within 6.1 ms.
#   python -I -X utf8 tools/r10/ab_sum.py <series dir> [--csv out.csv] [--md]
import csv
import json
import math
import os
import re
import statistics as st
import sys

WINDOWS = {"user": (40, 100), "calm": (45, 118), "fight": (45, 140), "city": (45, 140), "neon": (42, 133), "beach": (62, 88),
           "harbor": (62, 88), "village": (62, 88)}


def pct(v, q):
    s = sorted(v)
    return s[min(len(s) - 1, max(0, math.ceil(q * len(s)) - 1))]


def lag1(x):
    m = st.mean(x)
    v = sum((a - m) ** 2 for a in x)
    return sum((a - m) * (b - m) for a, b in zip(x, x[1:])) / v if v else 0.0


def run_stats(d, scene):
    lo, hi = WINDOWS[scene]
    rows = list(csv.DictReader(open(os.path.join(d, "gpu_frames.csv"), newline="")))
    rs = [r for r in rows if r["present_ms"] and lo * 1000 <= float(r["present_ms"]) <= hi * 1000]
    pr = [float(r["present_ms"]) for r in rs]
    iv = [b - a for a, b in zip(pr, pr[1:])]
    if len(iv) < 50:
        return None
    s = sorted(iv, reverse=True)
    n = len(iv)
    minutes = (pr[-1] - pr[0]) / 60000.0
    game = [float(r["game_submit_ms"]) for r in rs if float(r["game_submit_ms"]) > 0]
    giv = [b - a for a, b in zip(game, game[1:])]
    mis = [abs(g - p) for g, p in zip(giv, iv)]
    out = {
        "fps": (len(pr) - 1) * 1000.0 / (pr[-1] - pr[0]),
        "low1": 1000.0 / st.mean(s[: max(1, n // 100)]),
        "low01": 1000.0 / st.mean(s[: max(1, n // 1000)]),
        "p50": pct(iv, 0.5), "p95": pct(iv, 0.95), "p99": pct(iv, 0.99), "max": max(iv),
        "gt33": sum(1 for x in iv if x > 33.33) / minutes, "gt50": sum(1 for x in iv if x > 50) / minutes,
        "gt100": sum(1 for x in iv if x > 100) / minutes,
        "lag1": lag1(iv), "lt22": 100.0 * sum(1 for x in iv if x < 22) / n,
        "gpu": st.mean(float(r["gpu_busy_ms"]) for r in rs), "cp": st.mean(float(r["cp_ms"]) for r in rs),
        "mism": st.mean(mis) if mis else float("nan"),
    }
    lost = crit = err = 0
    for name in ("SecondSonPC.log", "gpu_native.txt"):
        p = os.path.join(d, name)
        if os.path.exists(p):
            t = open(p, errors="replace").read()
            lost += len(re.findall(r"DEVICE_LOST|device lost|VkResult -4", t))
            crit += t.count("[Critical]")
            err += t.count("[Error]")
    lost += 1 if os.path.exists(os.path.join(d, "gpu_device_lost.txt")) else 0
    out.update({"lost": lost, "crit": crit, "err": err})
    return out


KEYS = [("fps", "FPS", 2), ("low1", "1% low", 1), ("low01", "0.1% low", 1), ("p50", "P50", 1), ("p95", "P95", 1),
        ("p99", "P99", 1), ("max", "max", 0), ("gt33", ">33/min", 0), ("gt50", ">50/min", 1), ("gt100", ">100/min", 1),
        ("lag1", "lag1", 2), ("lt22", "<22ms%", 0), ("gpu", "GPU", 1), ("cp", "CP", 1), ("mism", "mism", 1)]


def main(argv):
    root = argv[1]
    md = "--md" in argv
    out_csv = argv[argv.index("--csv") + 1] if "--csv" in argv else None
    groups = {}
    for name in sorted(os.listdir(root)):
        d = os.path.join(root, name)
        if not os.path.isdir(d) or "_dirty" in name or "_aborted" in name:
            continue
        m = re.match(r"([a-z]+)_(.+)_(\d+)$", name)
        if not m or m.group(1) not in WINDOWS or not os.path.exists(os.path.join(d, "gpu_frames.csv")):
            continue
        s = run_stats(d, m.group(1))
        if s is None:
            continue
        groups.setdefault((m.group(1), m.group(2)), []).append((name, s))
    rows_csv = []
    for (scene, cfg), runs in sorted(groups.items()):
        for name, s in runs:
            rows_csv.append({"scene": scene, "config": cfg, "run": name, **{k: round(v, 3) for k, v in s.items()}})
    if out_csv:
        with open(out_csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows_csv[0].keys()))
            w.writeheader()
            w.writerows(rows_csv)
    scene_prev = None
    for (scene, cfg), runs in sorted(groups.items(), key=lambda kv: (kv[0][0], kv[0][1])):
        if scene != scene_prev:
            hdr = " | ".join(lbl for _, lbl, _ in KEYS)
            print(f"\n### {scene}\n\n| config | n | {hdr} | lost/crit/err |" if md else f"\n== {scene}")
            if md:
                print("|---|---|" + "---|" * len(KEYS) + "---|")
            scene_prev = scene
        cells = []
        for k, _, p in KEYS:
            v = [s[k] for _, s in runs if not math.isnan(s[k])]
            if not v:
                cells.append("-")
                continue
            med = st.median(v)
            cells.append(f"{med:.{p}f} [{min(v):.{p}f}..{max(v):.{p}f}]" if len(v) > 1 else f"{med:.{p}f}")
        fails = f"{sum(s['lost'] for _, s in runs)}/{sum(s['crit'] for _, s in runs)}/{sum(s['err'] for _, s in runs)}"
        if md:
            print(f"| {cfg} | {len(runs)} | " + " | ".join(cells) + f" | {fails} |")
        else:
            print(f"  {cfg:14s} n {len(runs)}  " + "  ".join(f"{lbl} {c}" for (_, lbl, _), c in zip(KEYS, cells)) + f"  fail {fails}")


if __name__ == "__main__":
    main(sys.argv)
