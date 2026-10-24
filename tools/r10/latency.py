# SecondSonPC R10: input-to-present latency of one run (SSPC_R10_LATENCY=1, src/runtime/gpu/r10/latency.h).
#   py -I -X utf8 tools\r10\latency.py <run dir with r10_latency.csv> [--min-move 300] [--json out.json]
# For each scripted camera turn (a mouse movement of at least --min-move counts in one event) the first presented frame
# after it whose texel grid changed clearly more than the frames before it: change > max(4 x the median change of the
# 8 presents before the input, the 95th percentile of them + 2, 3.0). Latency = that present's TSC - the input's TSC
# (SDL_PushEvent -> the game samples it -> simulates -> renders -> the frame reaches vkQueuePresentKHR). Turns closer
# than 0.8 s to the previous one are skipped (the picture still moving).
import argparse
import bisect
import json
import os
import statistics
import sys


def load(path):
    hz = None
    inputs, presents = [], []
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("# tsc_hz"):
                hz = float(line.split()[2])
                continue
            if line.startswith("type,"):
                continue
            p = line.rstrip("\n").split(",")
            if p[0] == "input":
                inputs.append((int(p[1]), int(p[2]), float(p[4] or 0), float(p[5] or 0)))
            elif p[0] == "present":
                presents.append((int(p[1]), int(p[2]), float(p[3])))
    presents.sort()
    return hz, inputs, presents


def analyze(run, min_move=300.0):
    hz, inputs, presents = load(os.path.join(run, "r10_latency.csv"))
    pt = [p[0] for p in presents]
    res = []
    last = None
    for tsc, kind, dx, dy in inputs:
        if kind != 3 or abs(dx) + abs(dy) < min_move:
            continue
        if last is not None and (tsc - last) / hz < 0.8:
            continue
        last = tsc
        i = bisect.bisect_right(pt, tsc)
        before = [presents[k][2] for k in range(max(0, i - 8), i) if presents[k][2] >= 0]
        if len(before) < 4:
            continue
        med = statistics.median(before)
        p95 = sorted(before)[int(0.95 * (len(before) - 1))]
        thr = max(4.0 * med, p95 + 2.0, 3.0)
        hit = None
        for k in range(i, min(len(presents), i + 30)):
            if presents[k][2] > thr:
                hit = k
                break
        if hit is None:
            res.append({"t": tsc / hz, "latency_ms": None})
            continue
        res.append({"t": tsc / hz, "latency_ms": (presents[hit][0] - tsc) * 1000.0 / hz,
                    "presents_after": hit - i + 1, "change": presents[hit][2], "threshold": thr})
    ok = [r["latency_ms"] for r in res if r["latency_ms"] is not None]
    out = {"turns": len(res), "detected": len(ok)}
    if ok:
        s = sorted(ok)
        out.update({"mean_ms": statistics.fmean(ok), "p50_ms": s[len(s) // 2], "p95_ms": s[int(0.95 * (len(s) - 1))],
                    "min_ms": s[0], "max_ms": s[-1],
                    "presents_after_mean": statistics.fmean(r["presents_after"] for r in res if r["latency_ms"])})
    if len(pt) > 1:
        out["present_interval_ms"] = (pt[-1] - pt[0]) * 1000.0 / hz / (len(pt) - 1)
    return out, res


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--min-move", type=float, default=300.0)
    ap.add_argument("--json")
    a = ap.parse_args(argv)
    out, res = analyze(a.run, a.min_move)
    for r in res:
        print(f"  turn at {r['t']:.2f}: " + (f"{r['latency_ms']:.1f} ms ({r['presents_after']} presents, change "
                                              f"{r['change']:.1f} > {r['threshold']:.1f})" if r["latency_ms"] else "not detected"))
    print(json.dumps(out, indent=1))
    if a.json:
        json.dump({"summary": out, "turns": res}, open(a.json, "w"), indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
