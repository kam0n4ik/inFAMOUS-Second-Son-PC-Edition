"""R12 lights: one run's light census and GPU profile summary.

  py -I -X utf8 tools/r12/lightsum.py <run dir> [--from S] [--to S]

- census: the "R12 light census/10s" lines of SecondSonPC.log (SSPC_R12_LIGHT_CENSUS=1) inside the window (log time
  relative to the first line), averaged weighted by frames;
- gpu profile (SSPC_GPU_PROFILE=1, gpu_profile_full.txt): the reports' ms per frame by key, summed over the reports
  in the file after the first two (the loading screens), with the light-related keys named.
"""
import collections
import re
import sys
from pathlib import Path

LIGHT_KEYS = {
    "0xbb6be9001": "tiled lighting CS 0x2edafa400",
    "0x5": "batch end: write-back shader",
    "0x6": "batch end: barrier + write-back copies",
    "0x3": "before a command (barriers, uploads, render pass changes, early write-backs)",
    "0x7": "occlusion counts copy",
}


def census(run, t0, t1):
    log = Path(run) / "SecondSonPC.log"
    if not log.exists():
        return None
    first = None
    acc = collections.defaultdict(float)
    frames_total = 0.0
    kinds = collections.defaultdict(float)
    pat_t = re.compile(r"^\[(\d+):(\d+):(\d+)\.(\d+)\]")
    for line in log.open(encoding="utf-8", errors="replace"):
        m = pat_t.match(line)
        if not m:
            continue
        t = int(m[1]) * 3600 + int(m[2]) * 60 + int(m[3]) + int(m[4]) / 1000.0
        if first is None:
            first = t
        if "R12 light census/10s" not in line:
            continue
        rel = t - first
        if rel < t0 or rel > t1:
            continue
        fm = re.search(r"\((\d+) frames\)", line)
        fr = float(fm[1])
        frames_total += fr
        nums = {
            "tested": r"data lights ([\d.]+) tested",
            "kept": r"([\d.]+) kept",
            "shadowed": r"([\d.]+) with a shadow slot",
            "animated": r"([\d.]+) animated",
            "code": r"code point lights ([\d.]+) of",
            "flushed": r"flushed to the list ([\d.]+)",
            "peak": r"peak index (-?\d+)",
            "overcap": r"over capacity ([\d.]+)",
            "particle": r"particle lights ([\d.]+)",
            "atlas": r"spot atlas depth draws ([\d.]+)",
            "otherdepth": r"other depth-only draws ([\d.]+)",
            "occq": r"glow occlusion queries ([\d.]+)",
            "occd": r"queries [\d.]+ \(([\d.]+) draws\)",
        }
        for k, p in nums.items():
            mm = re.search(p, line)
            if mm:
                v = float(mm[1])
                acc[k] = max(acc[k], v) if k == "peak" else acc[k] + v * fr
        km = re.search(r"\(by kind([^)]*)\)", line)
        if km:
            for kk, vv in re.findall(r"(\w+):([\d.]+)", km[1]):
                kinds[kk] += float(vv) * fr
    if frames_total == 0:
        return None
    out = {k: (v if k == "peak" else v / frames_total) for k, v in acc.items()}
    out["kinds"] = {k: v / frames_total for k, v in kinds.items()}
    out["frames"] = frames_total
    return out


def gpuprof(run, top=40):
    f = Path(run) / "gpu_profile_full.txt"
    if not f.exists():
        return None
    reports = []
    cur = None
    for line in f.open(encoding="utf-8", errors="replace"):
        if line.startswith("report frames"):
            m = re.match(r"report frames (\d+) total_ms_per_frame ([\d.]+)", line)
            cur = {"frames": int(m[1]), "total": float(m[2]), "keys": {}}
            reports.append(cur)
            continue
        parts = line.rstrip("\n").split("\t")
        if cur is not None and len(parts) >= 4:
            cur["keys"][parts[0]] = (float(parts[1]), int(parts[2]), parts[3])
    body = [r for r in reports if r["total"] > 5.0][1:]  # (in-game reports; the first one is the warm-up)
    if not body:
        return None
    n = len(body)
    agg = collections.defaultdict(lambda: [0.0, 0, ""])
    for r in body:
        for k, (ms, cnt, label) in r["keys"].items():
            a = agg[k]
            a[0] += ms / n
            a[1] += cnt / n / r["frames"]
            a[2] = label
    total = sum(r["total"] for r in body) / n
    rows = sorted(agg.items(), key=lambda kv: -kv[1][0])
    return total, n, rows[:top], agg


def main():
    run = sys.argv[1]
    t0, t1 = 45.0, 1e9
    if "--from" in sys.argv:
        t0 = float(sys.argv[sys.argv.index("--from") + 1])
    if "--to" in sys.argv:
        t1 = float(sys.argv[sys.argv.index("--to") + 1])
    c = census(run, t0, t1)
    if c:
        print(f"census ({c['frames']:.0f} frames): data lights tested {c.get('tested', 0):.1f}, kept {c.get('kept', 0):.1f} "
              f"(kinds {', '.join(f'{k}:{v:.1f}' for k, v in sorted(c['kinds'].items()))}), shadow slot "
              f"{c.get('shadowed', 0):.1f}, animated {c.get('animated', 0):.1f}; code point {c.get('code', 0):.1f}; "
              f"flushed {c.get('flushed', 0):.1f} (peak {c.get('peak', 0):.0f}, over cap {c.get('overcap', 0):.1f}); "
              f"particle {c.get('particle', 0):.1f}; atlas draws {c.get('atlas', 0):.0f}, other depth-only "
              f"{c.get('otherdepth', 0):.0f}; glow queries {c.get('occq', 0):.1f} ({c.get('occd', 0):.1f} draws)")
    g = gpuprof(run)
    if g:
        total, n, rows, agg = g
        print(f"gpu profile: {n} reports, {total:.2f} ms of GPU commands per frame (serialized by the timestamps)")
        for k, (ms, cnt, label) in rows:
            name = LIGHT_KEYS.get(k, "")
            print(f"  {ms:7.3f} ms  {cnt:9.1f}/frame  {k:>14}  {label[:110]}{'  <' + name + '>' if name else ''}")
        for k, name in LIGHT_KEYS.items():
            if k in agg and all(k != r[0] for r in rows):
                ms, cnt, label = agg[k]
                print(f"  {ms:7.3f} ms  {cnt:9.1f}/frame  {k:>14}  {label[:110]}  <{name}>")


if __name__ == "__main__":
    main()
