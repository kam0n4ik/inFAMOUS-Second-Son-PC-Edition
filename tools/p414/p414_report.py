# SecondSonPC P4.14: the shader cache / precompiler matrix's tables (tools\tests\p414_matrix.ps1).
#   py -I -X utf8 tools\p414\p414_report.py logs\p414\<Tag> [--out report.md]
# For every <config>\<scene>_1 folder: frame times over the whole run (presentation to presentation, from the first
# presented frame; the first visit's stalls are in the loading and the first seconds, not in the steady window) and in
# the steady window 62-88 s (tools\frame_stats.py's definitions: average FPS = (frames - 1) / span, 1% low = 1000 / mean
# of the slowest 1% frame times, p95/p99 nearest rank); stalls >= 50/100/1000 ms; translations and pipelines made on
# demand (gpu_native.txt); the start (process start to the first presented frame, the precompiler's share); memory;
# errors; the cache's size.
import csv
import math
import os
import re
import sys
from datetime import datetime

SCENES = {"user1022": "деревня", "beach": "пляж", "early": "гавань"}
CONFIGS = ["A", "R", "B", "C", "CW", "D", "E", "F", "G", "H"]


def pct(v, q):
    if not v:
        return float("nan")
    s = sorted(v)
    return s[min(len(s) - 1, max(0, math.ceil(q * len(s)) - 1))]


def mean(v):
    return sum(v) / len(v) if v else float("nan")


def frames(path, lo=None, hi=None):
    pres = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            p = r.get("present_ms")
            if p in ("", None):
                continue
            p = float(p)
            if p <= 0:
                continue
            if lo is not None and not (lo <= p <= hi):
                continue
            pres.append(p)
    ft = [b - a for a, b in zip(pres, pres[1:])]
    out = {"frames": len(pres)}
    if len(ft) >= 2:
        worst = sorted(ft, reverse=True)[: max(1, len(ft) // 100)]
        out.update({
            "avg_fps": (len(pres) - 1) * 1000.0 / (pres[-1] - pres[0]),
            "low1_fps": 1000.0 / mean(worst),
            "p95": pct(ft, 0.95), "p99": pct(ft, 0.99), "max": max(ft),
            "s50": sum(1 for d in ft if d >= 50), "s100": sum(1 for d in ft if d >= 100),
            "s1000": sum(1 for d in ft if d >= 1000),
            "first_present_s": pres[0] / 1000.0,
        })
    return out


def grab(pattern, text, cast=float, default=None, group=1):
    m = re.search(pattern, text)
    return cast(m.group(group)) if m else default


def log_time(line):
    m = re.match(r"\[(\d\d:\d\d:\d\d\.\d+)\]", line)
    return datetime.strptime(m.group(1), "%H:%M:%S.%f") if m else None


def run(folder):
    r = {}
    fcsv = os.path.join(folder, "gpu_frames.csv")
    if not os.path.exists(fcsv):
        return None
    r["all"] = frames(fcsv)
    r["win"] = frames(fcsv, 62000.0, 88000.0)
    native_path = os.path.join(folder, "gpu_native.txt")
    native = open(native_path, encoding="utf-8", errors="replace").read() if os.path.exists(native_path) else ""
    r["pipe_ms"] = grab(r"pipeline creation \(driver\): ([\d.]+) ms", native, default=0.0)
    r["pipe_n"] = grab(r"pipelines on demand: created (\d+)", native, int, None)
    if r["pipe_n"] is None:
        g = grab(r"pipelines: graphics (\d+)", native, int, 0)
        c = grab(r"pipelines: graphics \d+, compute (\d+)", native, int, 0)
        r["pipe_n"] = g + c
    r["pipe_max"] = grab(r"slowest ([\d.]+) ms", native, default=0.0)
    r["tr_n"] = grab(r"shader translations \(GCN -> SPIR-V\): (\d+)", native, int, 0)
    r["tr_ms"] = grab(r"shader translations \(GCN -> SPIR-V\): \d+, ([\d.]+) ms", native, default=0.0)
    r["disk_read"] = grab(r"this run: modules read (\d+)", native, int, 0)
    r["cache_mib"] = grab(r"persistent shader cache \(P4.14\): .* - ([\d.]+) MiB", native, default=0.0)
    r["vk_mib"] = grab(r"saved \d+ times, last ([\d.]+) MiB", native, default=0.0)
    r["hits"] = grab(r"driver cache feedback \d+: hits (\d+)", native, int, 0)
    r["misses"] = grab(r"driver cache feedback \d+: hits \d+, misses (\d+)", native, int, 0)
    r["pre_total"] = grab(r"start-up precompiler: (\d+) pipelines", native, int, 0)
    r["pre_built"] = grab(r"start-up precompiler: \d+ pipelines in the manifest, built (\d+)", native, int, 0)
    r["pre_taken"] = grab(r", taken (\d+), never taken", native, int, 0)
    r["pre_never"] = grab(r"never taken (\d+)", native, int, 0)
    r["pre_wait"] = grab(r"taken precompiled \d+ \(waited for (\d+)", native, int, 0)
    log_path = os.path.join(folder, "SecondSonPC.log")
    log = open(log_path, encoding="utf-8", errors="replace").read() if os.path.exists(log_path) else ""
    lines = log.splitlines()
    t_first = next((log_time(l) for l in lines if log_time(l)), None)
    t_backend = next((log_time(l) for l in lines if "native backend started on" in l), None)
    r["pre_ms"] = grab(r"shader precompiler: \d+ of \d+ pipelines ready in (\d+) ms", log, default=0.0)
    r["backend_s"] = (t_backend - t_first).total_seconds() if t_first and t_backend else float("nan")
    r["start_s"] = r["backend_s"] + r["all"].get("first_present_s", float("nan"))
    pm = re.findall(r"commit sample: process private (\d+) MiB", log)
    r["private_max"] = max(int(x) for x in pm) if pm else None
    r["errors"] = len(re.findall(r"\[Error\]", log))
    val = os.path.join(folder, "vk_validation.log")
    r["validation"] = sum(1 for _ in open(val, encoding="utf-8", errors="replace")) if os.path.exists(val) else None
    return r


def f1(v):
    return "–" if v is None or (isinstance(v, float) and math.isnan(v)) else f"{v:.1f}"


def main(argv):
    base = argv[1]
    out = argv[argv.index("--out") + 1] if "--out" in argv else os.path.join(base, "report.md")
    rows = []
    for scene in SCENES:
        for cfg in CONFIGS:
            folder = os.path.join(base, cfg, f"{scene}_1")
            if os.path.isdir(folder):
                r = run(folder)
                if r:
                    rows.append((scene, cfg, r))
    t = []
    t.append("## Весь прогон (от первого показанного кадра)\n")
    t.append("| сцена | конф. | кадров | ср. FPS | 1% low | p95 мс | p99 мс | макс мс | ≥50 мс | ≥100 мс | ≥1000 мс |")
    t.append("|---|---|---|---|---|---|---|---|---|---|---|")
    for scene, cfg, r in rows:
        a = r["all"]
        t.append(f"| {SCENES[scene]} | {cfg} | {a.get('frames')} | {f1(a.get('avg_fps'))} | {f1(a.get('low1_fps'))} | "
                 f"{f1(a.get('p95'))} | {f1(a.get('p99'))} | {f1(a.get('max'))} | {a.get('s50')} | {a.get('s100')} | "
                 f"{a.get('s1000')} |")
    t.append("\n## Окно 62–88 с (установившаяся сцена)\n")
    t.append("| сцена | конф. | ср. FPS | 1% low | p95 мс | p99 мс | ≥50 мс | ≥100 мс |")
    t.append("|---|---|---|---|---|---|---|---|")
    for scene, cfg, r in rows:
        w = r["win"]
        t.append(f"| {SCENES[scene]} | {cfg} | {f1(w.get('avg_fps'))} | {f1(w.get('low1_fps'))} | {f1(w.get('p95'))} | "
                 f"{f1(w.get('p99'))} | {w.get('s50')} | {w.get('s100')} |")
    t.append("\n## Компиляция на ходу, кэш, запуск, память\n")
    t.append("| сцена | конф. | трансляций (мс) | из кэша | конвейеров на ходу (мс, макс) | кэш драйвера попад./промах "
             "| прекомпил. собрано/взято/не нужно | прекомпил. мс | до 1-го кадра с | кэш МиБ + vulkan.bin | RAM макс МиБ "
             "| [Error] | валидация |")
    t.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for scene, cfg, r in rows:
        t.append(f"| {SCENES[scene]} | {cfg} | {r['tr_n']} ({f1(r['tr_ms'])}) | {r['disk_read']} | {r['pipe_n']} "
                 f"({f1(r['pipe_ms'])}, {f1(r['pipe_max'])}) | {r['hits']}/{r['misses']} | "
                 f"{r['pre_built']}/{r['pre_taken']}/{r['pre_never']} | {f1(r['pre_ms'])} | {f1(r['start_s'])} | "
                 f"{f1(r['cache_mib'])} + {f1(r['vk_mib'])} | {r['private_max']} | {r['errors']} | "
                 f"{'–' if r['validation'] is None else r['validation']} |")
    text = "\n".join(t) + "\n"
    open(out, "w", encoding="utf-8").write(text)
    print(text)


if __name__ == "__main__":
    main(sys.argv)
