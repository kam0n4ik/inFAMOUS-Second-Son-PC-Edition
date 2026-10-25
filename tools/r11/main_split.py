"""R11: Game:Main's frame split into the game's own logic, SecondSonPC's code it calls and its waits (docs/R11_MAIN_THREAD.md).

  py -I -X utf8 tools/r11/main_split.py <run dir> [--phase name=S-S ...] [--marks] [--top 25]
        [--funcs E:/research/r9/ghidra/functions.tsv] [--exe <SecondSonPC.exe>] [--out <file>]

Input: a run with SSPC_R9_GPROF=2 (r9_gprof.bin + gpu_frames.csv). Every running sample of Game:Main (weighted by the
cycles the thread ran in its tick, as tools/r9/gprof.py) goes to one class:
  wait/<kind>   the game's delay(us) 0x53b2b0 (rdtsc spin) or anything below it, by its caller: cw_emitters (the async
                compute wait 0x1695e0 from EndFrame 0x3edef0), cw_light (0x1695e0 from 0x3ee470), eop (SubmitFrame
                0x1f1e30), spin_<caller> (any other caller of the delay)
  jobwait       inside WaitBatch 0x243c50 (the main thread waits for its job batch; it runs jobs meanwhile)
  ours/<fn>     outside the eboot (SecondSonPC.exe, ntdll/kernel32/driver), not under a wait: by SecondSonPC function
                (the innermost SecondSonPC frame; system code by its SecondSonPC caller)
  game          inside the eboot, not a wait: by function (exclusive) - the game's own logic
  blocked       ran in the tick, then sampled in a wait system call (where its cycles went is not seen)
Per phase: ms per presented frame and share of Game:Main's CPU, the frame time, the wall time the thread was off the
CPU (frame - CPU), the top functions of each class, and the eboot callers of SecondSonPC's code (which import).
"""
import collections
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "r9"))
import gprof  # noqa: E402

DELAY, CWAIT, ENDFRAME, MAINLOOP, SUBMIT, WAITBATCH = 0x53b2b0, 0x1695e0, 0x3edef0, 0x29c8d0, 0x1f1e30, 0x243c50
# SecondSonPC's replacements of the compute wait (frame_trace.cpp) and R11's sleep on its label (label_wait.cpp)
WAIT_HOST = ("ComputeWaitHook", "ComputeResetBarrier", "LabelWait::Sleep")


def classify(g, funcs, flags, addrs, host_names):
    """(class, detail) of one running Game:Main sample."""
    rip, caller, hosts, chain = g.split(flags, addrs)
    lo = g.guest_lo
    in_eboot = lo <= rip < g.guest_hi
    ebo = [funcs.of(r - lo) for r in chain]
    leaf = funcs.of(rip - lo) if in_eboot else None
    cal = funcs.of(caller - lo) if (caller and not in_eboot) else None
    # the delay: leaf in it, or SecondSonPC/system code called from it (sceKernelUsleep above 500 us)
    if leaf == DELAY or cal == DELAY:
        up = ebo[0] if ebo else None
        if up == CWAIT:
            up2 = ebo[1] if len(ebo) > 1 else None
            if up2 == ENDFRAME:
                return "wait", "cw_emitters"
            if up2 == MAINLOOP:
                return "wait", "cw_light"
            return "wait", f"cw_from_{up2:#x}" if up2 is not None else "cw_?"
        if up == SUBMIT:
            return "wait", "eop"
        if up == WAITBATCH:
            return "jobwait", "delay"
        return "wait", f"spin_{up:#x}" if up is not None else "spin_?"
    hn = [host_names.get(h, "") for h in ((rip,) if g.host_lo <= rip < g.host_hi else ()) + tuple(hosts)]
    if any(any(w in n for w in WAIT_HOST) for n in hn):
        # the replaced compute wait: by the eboot frames above it (0x1695e0 itself is not on the chain then)
        up = cal
        if up == ENDFRAME:
            return "wait", "cw_emitters"
        if up == MAINLOOP:
            return "wait", "cw_light"
        return "wait", f"cw_hook_{up:#x}" if up is not None else "cw_hook"
    if gprof.blocked_at(g, rip):
        return "blocked", g.export_of(rip)
    if WAITBATCH in ebo or leaf == WAITBATCH or cal == WAITBATCH:
        return "jobwait", f"{leaf:#x}" if leaf is not None else "host"
    if in_eboot:
        return "game", leaf
    if g.host_lo <= rip < g.host_hi:
        return "ours", host_names.get(rip, f"exe+{rip - g.host_lo:#x}")
    sysname = g.export_of(rip)
    via = host_names.get(hosts[0], "?") if hosts else "(no SecondSonPC frame)"
    return "ours", f"{via} -> {sysname}"


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    run = argv[1]
    opts = {"funcs": r"E:\research\r9\ghidra\functions.tsv", "exe": None, "top": "25", "out": None}
    phases, use_marks = [], False
    i = 2
    while i < len(argv):
        a = argv[i]
        if a == "--phase":
            name, span = argv[i + 1].split("=", 1)
            s0, s1 = span.split("-", 1)
            phases.append((name, float(s0) * 1000.0, float(s1) * 1000.0))
            i += 2
        elif a == "--marks":
            use_marks = True
            i += 1
        elif a.startswith("--") and a[2:] in opts:
            opts[a[2:]] = argv[i + 1]
            i += 2
        else:
            raise SystemExit(f"unknown argument {a}")
    g = gprof.Gprof(os.path.join(run, "r9_gprof.bin"))
    if not g.samples:
        raise SystemExit("no code samples: the run needs SSPC_R9_GPROF=2")
    frames = gprof.read_frames(os.path.join(run, "gpu_frames.csv"))
    funcs = gprof.Functions(opts["funcs"])
    exe = opts["exe"] or os.path.join(run, "..", "SecondSonPC.exe")
    if not os.path.exists(exe):
        exe = None
    rows_all = gprof.frame_rows(g, frames, 0.0, 1e12)
    if use_marks:
        phases += gprof.phases_from_marks(g, 0.0, rows_all[-1]["present_ms"])
    if not phases:
        phases = [("window", rows_all[0]["present_ms"] - rows_all[0]["ft_ms"], rows_all[-1]["present_ms"])]
    main_idx = {i for i, gr in g.group.items() if gr == "game_main"}
    # SecondSonPC addresses of Game:Main's samples, symbolized once
    hosts = set()
    for (tick, idx, flags, cyc, addrs) in g.samples:
        if idx in main_idx and not flags & 2:
            rip, caller, hs, chain = g.split(flags, addrs)
            if g.host_lo <= rip < g.host_hi:
                hosts.add(rip)
            hosts |= {h for h in hs if g.host_lo <= h < g.host_hi}
    names = gprof.symbolize_host(exe, hosts, g.host_lo) if exe else {}
    top = int(opts["top"])
    out = []
    for name, p0, p1 in phases:
        pr = [r for r in rows_all if p0 < r["present_ms"] <= p1]
        if len(pr) < 2:
            continue
        n = len(pr)
        frame_ms = sum(r["ft_ms"] for r in pr) / n
        ta, tb = pr[0]["_ticks"][0], pr[-1]["_ticks"][1]
        cpu = sum(cyc for ran in g.tick_ran[ta:tb] for idx, cyc in ran if idx in main_idx)
        cls = collections.Counter()
        det = collections.defaultdict(collections.Counter)
        ours_callers = collections.Counter()  # eboot function calling into SecondSonPC (the import's caller)
        sampled = 0
        for (tick, idx, flags, cyc, addrs) in g.samples_in(ta, tb):
            if idx not in main_idx or flags & 2:
                continue
            sampled += cyc
            c, d = classify(g, funcs, flags, addrs, names)
            cls[c] += cyc
            det[c][d] += cyc
            if c == "ours":
                rip, caller, hs, chain = g.split(flags, addrs)
                e = funcs.of(caller - g.guest_lo) if caller else None
                ours_callers[e] += cyc
        k = g.cyc_to_ms / n
        scale = cpu / sampled if sampled else 1.0  # (samples cover the ticks that ran >= the threshold)
        st = gprof.ft_stats([r["ft_ms"] for r in pr])
        out.append(f"== {name}: {p0 / 1000:.1f}..{p1 / 1000:.1f} s, {n} frames, FPS {st['avg_fps']:.2f}, 1% low "
                   f"{st['low1_fps']:.2f}, p99 {st['p99']:.1f} ms")
        out.append(f"  frame {frame_ms:.2f} ms; Game:Main CPU {cpu * k:.2f} ms ({100.0 * cpu / max(frame_ms / k, 1):.1f}% "
                   f"of the frame), off the CPU {frame_ms - cpu * k:.2f} ms; sampled {sampled * k:.2f} ms "
                   f"(class ms below scaled by {scale:.3f} to the CPU)")
        waits = sum(v for d, v in det["wait"].items())
        for c in ("game", "ours", "wait", "jobwait", "blocked"):
            out.append(f"  {c:8s} {cls[c] * k * scale:7.2f} ms/frame  {100.0 * cls[c] / max(sampled, 1):5.1f}% of CPU")
        out.append("  wait by kind: " + ", ".join(f"{d} {v * k * scale:.2f}" for d, v in det["wait"].most_common(10)))
        out.append(f"  (sum of waits {waits * k * scale:.2f} ms/frame)")
        out.append("  game (eboot, exclusive), top:")
        for e, v in det["game"].most_common(top):
            nm = funcs.name(e) if e is not None else "?"
            ent = f"{e:#x}" if e is not None else "?"
            out.append(f"    {ent:>9} {nm:<34} {v * k * scale:7.3f}")
        out.append("  ours (SecondSonPC / system code on Game:Main), top:")
        for d, v in det["ours"].most_common(top):
            out.append(f"    {d[:100]:<100} {v * k * scale:7.3f}")
        out.append("  ours by the eboot function that called it:")
        for e, v in ours_callers.most_common(15):
            ent = f"{e:#x}" if e is not None else "?"
            out.append(f"    {ent:>9} {funcs.name(e) if e is not None else '?':<34} {v * k * scale:7.3f}")
        if det["jobwait"]:
            out.append("  jobwait (inside WaitBatch 0x243c50) leaves: " + ", ".join(
                f"{d} {v * k * scale:.3f}" for d, v in det["jobwait"].most_common(8)))
        if det["blocked"]:
            out.append("  blocked: " + ", ".join(f"{d} {v * k * scale:.3f}" for d, v in det["blocked"].most_common(6)))
        out.append("")
    text = "\n".join(out)
    if opts["out"]:
        with open(opts["out"], "w", encoding="utf-8") as f:
            f.write(text + "\n")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
