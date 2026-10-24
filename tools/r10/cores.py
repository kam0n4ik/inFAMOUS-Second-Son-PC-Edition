# SecondSonPC R10: how one run spreads over the PC's logical CPUs (docs/R10_CPU_GPU_SCHEDULER.md, "Распределение по
# ядрам"), from a kernel ETW trace's context switches (tools\etw_capture.ps1 -NoStacks; xperf -i trace.etl -o dump.txt
# -a dumper).
#   py -I -X utf8 tools\r10\cores.py <dir with dump.txt> [--process SecondSonPC.exe] [--top 30] [--smt-pairs adjacent]
# Writes <dir>\cores.txt and cores.json:
#   * per logical CPU: busy % of the window (any thread but Idle), of it the game process's %, other processes' %;
#   * per physical core (logical CPUs 2k and 2k+1 - the SMT siblings as Windows numbers them on Ryzen/Intel): the time
#     both siblings ran game threads at once (two game threads sharing one core);
#   * per game thread (its name from ThreadName/T-DCStart): CPU % of one logical CPU, on which logical CPUs (% of its
#     run time), switch-ins, the time it waited for a CPU after being readied (ReadyThread -> CSwitch in: CPU
#     contention; mean / p95 / sum), the time it ran while its SMT sibling ran another game thread;
#   * thread groups by name (the job system's workers etc.).
# CSwitch columns (xperf dumper): TimeStamp(us), New Process (PID), New TID, NPri, NQnt, TmSinceLast, WaitTime, Old
# Process (PID), Old TID, OPri, OQnt, OldState, WaitReason, Swapable, InSwitchTime, CPU, IdealProc ...
import argparse
import collections
import json
import os
import re
import sys

PID_RE = re.compile(r"\(\s*(\d+)\)\s*$")


def pid_of(s):
    m = PID_RE.search(s)
    return int(m.group(1)) if m else -1


def proc_name(s):
    i = s.rfind("(")
    return s[:i].strip() if i > 0 else s.strip()


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--process", default="SecondSonPC.exe")
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--log", help="SecondSonPC.log of the run: guest thread names (SSPC_R10_AFFINITY_LOG=1)")
    ap.add_argument("--trace", help="p412_trace.bin of the run: SecondSonPC's thread names")
    a = ap.parse_args(argv)
    dump = os.path.join(a.dir, "dump.txt")
    names = {}
    masks = {}
    if a.log and os.path.exists(a.log):
        rx = re.compile(r"R10 affinity: thread '(.*)' \(tid \d+, Windows (\d+)\) starts with priority (-?\d+), policy (\d+), "
                        r"CPU mask (.*)$")
        for line in open(a.log, encoding="utf-8", errors="replace"):
            m = rx.search(line)
            if m:
                names[int(m.group(2))] = "guest:" + m.group(1)
                masks[int(m.group(2))] = f"prio {m.group(3)}, mask {m.group(5).strip()}"
    if a.trace and os.path.exists(a.trace):
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "p412"))
        import p412_analyze as pa  # noqa: E402
        for t in pa.load_trace(a.trace)["threads"]:
            if t["name"] and int(t["tid"]) not in names:
                names[int(t["tid"])] = t["name"]
    game_pids = set()
    cpu_cur = {}  # cpu -> (pid, tid, since)
    busy = collections.Counter()  # cpu -> us (not idle)
    busy_game = collections.Counter()
    run = collections.defaultdict(collections.Counter)  # tid -> cpu -> us
    switch_ins = collections.Counter()
    ready_at = {}
    ready_w = collections.defaultdict(list)
    first = last = None
    # SMT co-run: per core the game thread on each sibling right now
    smt_both = collections.Counter()  # core -> us both siblings ran game threads
    smt_tid = collections.Counter()   # tid -> us it ran while its sibling ran another game thread
    with open(dump, encoding="utf-8", errors="replace") as f:
        for line in f:
            s = line.lstrip()
            if s.startswith("ThreadName,") or s.startswith("T-DCStart,") or s.startswith("T-Start,"):
                p = [x.strip() for x in s.split(",")]
                if len(p) < 5 or not p[1].isdigit():
                    continue
                if proc_name(p[2]).lower() == a.process.lower():
                    game_pids.add(pid_of(p[2]))
                    tid = int(p[3])
                    if s.startswith("ThreadName,") and tid not in names:
                        names[tid] = p[4]
                continue
            if s.startswith("P-DCStart,") or s.startswith("P-Start,"):
                p = [x.strip() for x in s.split(",")]
                if len(p) > 2 and proc_name(p[2]).lower() == a.process.lower():
                    game_pids.add(pid_of(p[2]))
                continue
            if s.startswith("ReadyThread,"):
                p = [x.strip() for x in s.split(",")]
                if len(p) < 6 or not p[1].isdigit():
                    continue
                if pid_of(p[4]) in game_pids:
                    ready_at[int(p[5])] = int(p[1])
                continue
            if not s.startswith("CSwitch,"):
                continue
            p = [x.strip() for x in s.split(",")]
            if len(p) < 17 or not p[1].isdigit():
                continue
            ts = int(p[1])
            first = ts if first is None else first
            last = ts
            new_pid, new_tid = pid_of(p[2]), int(p[3])
            cpu = int(p[16])
            prev = cpu_cur.get(cpu)
            if prev is not None:
                dt = ts - prev[2]
                if prev[0] != 0:
                    busy[cpu] += dt
                if prev[0] in game_pids:
                    busy_game[cpu] += dt
                    run[prev[1]][cpu] += dt
                    sib = cpu ^ 1
                    o = cpu_cur.get(sib)
                    if o is not None and o[0] in game_pids:
                        # (the overlap with the sibling's current game thread since both started)
                        ov = ts - max(prev[2], o[2])
                        if ov > 0:  # (each overlapping pair counted once: by the one that ends first)
                            smt_tid[prev[1]] += ov
                            smt_tid[o[1]] += ov
                            smt_both[cpu // 2] += ov
            if new_pid in game_pids:
                switch_ins[new_tid] += 1
                r = ready_at.pop(new_tid, None)
                if r is not None and ts >= r:
                    ready_w[new_tid].append(ts - r)
            cpu_cur[cpu] = (new_pid, new_tid, ts)
    if first is None:
        print("no CSwitch events")
        return 1
    win = (last - first) or 1
    ncpu = max(busy.keys() | busy_game.keys() | cpu_cur.keys()) + 1
    L = [f"window {win / 1e6:.1f} s, logical CPUs {ncpu}, game pids {sorted(game_pids)}", "",
         "logical CPU: busy % (game % / other %)"]
    out = {"window_s": win / 1e6, "cpus": {}, "threads": [], "cores": {}}
    for c in range(ncpu):
        b, g = busy[c] / win * 100, busy_game[c] / win * 100
        L.append(f"  CPU{c:<2} {b:5.1f}  ({g:5.1f} / {b - g:5.1f})")
        out["cpus"][c] = {"busy_pct": round(b, 1), "game_pct": round(g, 1)}
    L.append(f"  all: busy {sum(busy.values()) / win / ncpu * 100:.1f} % of {ncpu} CPUs, the game "
             f"{sum(busy_game.values()) / win * 100:.0f} % of one CPU ({sum(busy_game.values()) / win / ncpu * 100:.1f} % of all)")
    L.append("")
    L.append("physical core (logical 2k, 2k+1): game threads on both siblings at once, % of the window")
    for k in range(ncpu // 2):
        v = smt_both[k] / win * 100
        L.append(f"  core{k} (CPU{2 * k},{2 * k + 1}): {v:5.1f}")
        out["cores"][k] = round(v, 1)
    L.append("")
    L.append(f"game threads (top {a.top} by CPU): CPU % of one logical CPU | where (% of its run time) | switch-ins/s | "
             "waited for a CPU after readied: mean/p95 us, ms/s | ran beside a game thread on its SMT sibling, % of its run")
    tot = {t: sum(c.values()) for t, c in run.items()}
    for tid in sorted(tot, key=lambda t: -tot[t])[:a.top]:
        c = run[tid]
        where = sorted(c.items(), key=lambda kv: -kv[1])
        wtxt = " ".join(f"{cpu}:{v / tot[tid] * 100:.0f}" for cpu, v in where if v / tot[tid] >= 0.03)
        rw = sorted(ready_w.get(tid, []))
        rw_mean = sum(rw) / len(rw) if rw else 0
        rw_p95 = rw[int(0.95 * (len(rw) - 1))] if rw else 0
        name = names.get(tid, "?")
        L.append(f"  {tid:>6} {name[:34]:<34} {tot[tid] / win * 100:5.1f} | {wtxt:<40} | {switch_ins[tid] / (win / 1e6):6.0f} | "
                 f"{rw_mean:6.0f}/{rw_p95:6.0f}, {sum(rw) / (win / 1e3):5.1f} | {smt_tid[tid] / max(tot[tid], 1) * 100:4.0f}")
        out["threads"].append({"tid": tid, "name": name, "guest_request": masks.get(tid, ""), "cpu_pct": round(tot[tid] / win * 100, 2),
                               "by_cpu_pct": {cpu: round(v / tot[tid] * 100, 1) for cpu, v in where},
                               "switch_ins_s": round(switch_ins[tid] / (win / 1e6), 1),
                               "ready_wait_mean_us": round(rw_mean, 1), "ready_wait_p95_us": rw_p95,
                               "ready_wait_ms_per_s": round(sum(rw) / (win / 1e3), 2),
                               "smt_shared_pct": round(smt_tid[tid] / max(tot[tid], 1) * 100, 1)})
    # groups by name without digits
    grp = collections.defaultdict(lambda: [0, 0.0])
    for tid, v in tot.items():
        g = re.sub(r"\d+", "#", names.get(tid, "?"))
        grp[g][0] += 1
        grp[g][1] += v / win * 100
    L.append("")
    L.append("thread groups (name with digits as #): threads, CPU % of one logical CPU together")
    for g, (n, v) in sorted(grp.items(), key=lambda kv: -kv[1][1])[:25]:
        L.append(f"  {g[:50]:<50} {n:4d} {v:7.1f}")
    out["groups"] = {g: {"threads": n, "cpu_pct": round(v, 1)} for g, (n, v) in grp.items()}
    txt = "\n".join(L)
    open(os.path.join(a.dir, "cores.txt"), "w", encoding="utf-8").write(txt + "\n")
    json.dump(out, open(os.path.join(a.dir, "cores.json"), "w"), indent=1)
    print(txt)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
