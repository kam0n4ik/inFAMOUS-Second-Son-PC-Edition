# SecondSonPC P4.12: one run's stall trace -> the frames' critical paths, the waits, the GPU's gaps, the slow frames.
#   py -I tools\p412\p412_analyze.py <run dir with p412_trace.bin, gpu_frames.csv> [--exe <SecondSonPC.exe that ran>]
#       [--json out.json]
# The trace (src/runtime/gpu/stall_trace.*, SSPC_P412_STALL_TRACE=deep): every wait/span of the backend's threads in a
# window (TSC), the GPU's batches (submission TSC, GPU start/end in device ticks), calibration pairs (device ticks <->
# QPC <-> TSC), stack samples of threads 1 (CP) and 2 (Record).
#
# The critical path of frame n is walked backwards from the moment thread 1 saw the frame's last batch complete (its
# EOP labels written: the game's next frame can start) to the same moment of frame n-1:
#   * on the GPU: the batch was executing; if it had been submitted while the GPU was still busy with the batch before,
#     the path stays on the GPU (the previous batch); else the GPU waited for that submission -> the submitting thread
#   * on a CPU thread inside a wait: the path goes to whoever ended the wait - thread 2 executing the record thread 1
#     drained for, thread 1 pushing the record thread 2 waited for, the GPU batch a GPU wait waited for; the time between
#     the release and the waiter running again is a wake-up latency (on the path); waits without a traced releaser
#     (a VideoOut label, a game flag, the queue lock, the presenter's frame) stay on the path as themselves
#   * on a CPU thread outside waits: busy - back to the end of its previous wait; the busy time is split by the
#     thread's stack samples that fall into the path's busy segments (statistical) and by its traced spans
# Waits that are not on the path cost nothing by themselves (the other thread's work behind them is on it instead).
import bisect
import collections
import csv
import json
import os
import re
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import performance_inspector as pi  # noqa: E402  (Symbols: llvm-symbolizer with the PDB, DLL exports)

ROLES = ["CP", "Record", "Present", "Game", "Other"]
EVENTS = ["PreRound", "Poll", "IdleEmpty", "IdleGpu", "IdleLabel", "IdleFlag", "Drain", "QueueFull", "JournalFull",
          "FlipMarker", "GpuCompleted", "RecordWait", "Record", "GpuSync", "RingWait", "BindWait", "QueueLock", "Submit",
          "PipelineCreate", "ShaderTranslate", "FlipAcquire", "CopyFallback", "VblankWait", "NotReady", "Present",
          "PresentFence", "PresentAcquire", "PresentSubmit", "GameSubmitLock", "GameIdleWait", "GameSubmitFlip",
          # P5.4 frame lifecycle (SSPC_P54_FRAME_TRACE=1)
          "GameSubmitFrame", "GameComputeWait", "GameSubmitDone", "GameDingDong", "FenceHeld", "FenceWrite",
          # P5.4 early frame submission (SSPC_P54_EARLY_SUBMIT=1)
          "GameEarlySubmit",
          # P5.4 async compute analysis: thread 1's slice of one queue in a round
          "CpQueueSlice"]
EV = {n: i for i, n in enumerate(EVENTS)}
DRAIN_REASONS = ["round end", "cache action", "fence", "dma report", "flip", "timeline write", "timeline copy",
                 "timeline gds", "memory wait", "wait met", "synchronize", "memory read", "memory write",
                 "frontend read", "stop"]
RECORD_KINDS = ["Draw", "Dispatch", "Journal", "Stop", "CacheAction", "Fence", "Dma", "WaitMet", "MemoryAcquired", "Copy"]
WAITS = {EV[n] for n in ("IdleEmpty", "IdleGpu", "IdleLabel", "IdleFlag", "Drain", "QueueFull", "JournalFull",
                         "RecordWait", "GpuSync", "RingWait", "BindWait", "QueueLock", "FlipAcquire")}
EVT_DT = np.dtype([("t0", "<u8"), ("t1", "<u8"), ("a", "<u8"), ("b", "<u4"), ("type", "<u2"), ("c", "<u2")])
BATCH_DT = np.dtype([("value", "<u8"), ("submit_t0", "<u8"), ("submit_t1", "<u8"), ("gpu_t0", "<u8"),
                     ("gpu_t1", "<u8"), ("frame", "<u4"), ("pad", "<u4")])
CAL_DT = np.dtype([("tsc_before", "<u8"), ("tsc_after", "<u8"), ("gpu", "<u8"), ("qpc", "<u8"), ("dev_ns", "<u8"),
                   ("qpc2", "<u8"), ("tsc2", "<u8")])
SAMPLE_DT = np.dtype([("tsc", "<u8"), ("tid", "<u4"), ("role", "u1"), ("n", "u1"), ("pad", "<u2"), ("pcs", "<u8", (8,))])

# what each segment of the path is and its class (A idle, B overhead, C required, D unknown; "work" = busy CPU/GPU
# work - neither a wait nor proven redundant: split further by the samples)
SEG_CLASS = {
    "game answer (EOP seen -> first submission)": "C",
    "thread 1 wake-up (submission -> processing)": "A",
    "EOP observation (GPU done -> thread 1 saw it)": "A",
    "thread 1 wake-up after a drain": "A",
    "thread 2 wake-up after a push": "A",
    "CPU wake-up after a GPU wait": "A",
    "GPU executing the frame's batches": "work",
    "GPU start after the submission": "B",
    "GPU idle with a batch submitted (bind wait / driver)": "D",
    "WAIT_REG_MEM on a VideoOut label (flip not shown yet)": "C",
    "WAIT_REG_MEM on a flag the game's CPU writes": "C",
    "thread 1 waits for the game (submissions)": "C",
    "Vulkan queue lock (another thread submits/presents)": "A",
    "flip waits for a presenter frame": "A",
    "submission pickup delayed (thread 1 in a GPU wait)": "A",
    "thread 1 idle (GPU wait ended without a completion)": "A",
    "thread 1 busy": "work",
    "thread 2 busy": "work",
    "untraced gap": "D",
}


def load_trace(path):
    data = open(path, "rb").read()
    hdr = struct.Struct("<8sIIdQQQdQQQQQII")
    (magic, ver, nthreads, tsc_hz, start_tsc, win0, win1, ts_period, qpc_freq, nbatches, ncal, nsamples, nmodules,
     sample_hz, records) = hdr.unpack_from(data, 0)
    if magic != b"SSPCP412" or ver != 1:
        raise SystemExit(f"{path}: not a P4.12 trace")
    off = hdr.size
    threads = []
    for _ in range(nthreads):
        role, tid = struct.unpack_from("<B3xI", data, off)
        name = data[off + 8:off + 72].split(b"\0", 1)[0].decode("utf-8", "replace")
        n, dropped = struct.unpack_from("<QQ", data, off + 72)
        off += 88
        ev = np.frombuffer(data, EVT_DT, count=n, offset=off).copy()
        off += n * EVT_DT.itemsize
        threads.append({"role": ROLES[role] if role < len(ROLES) else "?", "tid": tid, "name": name, "ev": ev,
                        "dropped": dropped})
    batches = np.frombuffer(data, BATCH_DT, count=nbatches, offset=off).copy()
    off += nbatches * BATCH_DT.itemsize
    cal = np.frombuffer(data, CAL_DT, count=ncal, offset=off).copy()
    off += ncal * CAL_DT.itemsize
    samples = np.frombuffer(data, SAMPLE_DT, count=nsamples, offset=off).copy()
    off += nsamples * SAMPLE_DT.itemsize
    modules = []
    for _ in range(nmodules):
        base, size = struct.unpack_from("<QQ", data, off)
        p = data[off + 16:off + 276].split(b"\0", 1)[0].decode("utf-8", "replace")
        off += 276
        modules.append({"base": f"{base:#x}", "size": f"{size:#x}", "name": os.path.basename(p), "path": p})
    return {"tsc_hz": tsc_hz, "start_tsc": start_tsc, "win0": win0, "win1": win1, "ts_period": ts_period,
            "qpc_freq": qpc_freq, "threads": threads, "batches": batches, "cal": cal, "samples": samples,
            "modules": modules, "sample_hz": sample_hz, "records": records}


def calibrate(tr, completions):
    """GPU ticks -> TSC: (GPU, QPC) pairs from VK_EXT_calibrated_timestamps and (QPC, TSC) pairs (least squares);
    without pairs, the lower envelope of thread 1's completion observations (an estimate)."""
    cal = tr["cal"]
    if len(cal) >= 4:
        q2 = cal["qpc2"].astype(np.float64)
        t2 = cal["tsc2"].astype(np.float64)
        q0 = q2[0]
        b1, a1 = np.polyfit(q2 - q0, t2, 1)  # tsc = a1 + b1 (qpc - q0)
        g = cal["gpu"].astype(np.float64)
        q = cal["qpc"].astype(np.float64)
        g0 = g[0]
        b2, a2 = np.polyfit(g - g0, q - q0, 1)  # qpc - q0 = a2 + b2 (gpu - g0)
        res_q = (q - q0) - (a2 + b2 * (g - g0))
        res_t = t2 - (a1 + b1 * (q2 - q0))
        # the converted QPC of each pair must lie between the TSC read before and after the call
        pred = a1 + b1 * (q - q0)
        inside = float(np.mean((pred >= cal["tsc_before"]) & (pred <= cal["tsc_after"])))

        def conv(gt):
            return a1 + b1 * (a2 + b2 * (np.asarray(gt, dtype=np.float64) - g0))
        info = {"method": "VK_EXT_calibrated_timestamps", "pairs": int(len(cal)),
                "gpu_ns_per_tick_measured": float(b2 * b1 / tr["tsc_hz"] * 1e9),
                "qpc_residual_us_max": float(np.max(np.abs(res_q)) / tr["qpc_freq"] * 1e6),
                "tsc_residual_us_max": float(np.max(np.abs(res_t)) / tr["tsc_hz"] * 1e6),
                "max_deviation_us": float(np.max(cal["dev_ns"]) / 1000.0),
                "pairs_inside_call_window": inside}
        return conv, info
    # fallback: offset = min over batches of (completion seen - GPU end), the scale from the timestamp period
    b = tr["batches"]
    scale = tr["ts_period"] * 1e-9 * tr["tsc_hz"]
    vals, ts = completions
    best = None
    for row in b:
        i = bisect.bisect_left(vals, int(row["value"]))
        if i < len(vals):
            d = ts[i] - row["gpu_t1"] * scale
            best = d if best is None or d < best else best
    if best is None:
        return None, {"method": "none"}
    return (lambda gt: best + np.asarray(gt, dtype=np.float64) * scale), {
        "method": "estimate: lower envelope of thread 1's completion observations (no calibration pairs)"}


class Thread:
    def __init__(self, t):
        self.role, self.tid, self.name = t["role"], t["tid"], t["name"]
        ev = t["ev"]
        self.ev = ev
        w = ev[np.isin(ev["type"], list(WAITS))]
        w = w[np.argsort(w["t1"], kind="stable")]
        self.w = w
        self.w_t1 = w["t1"].astype(np.int64)
        self.w_t0 = w["t0"].astype(np.int64)
        rec = ev[ev["type"] == EV["Record"]]
        self.rec = rec[np.argsort(rec["b"], kind="stable")]
        self.rec_b = self.rec["b"].astype(np.int64)
        recs_t = rec[np.argsort(rec["t1"], kind="stable")]
        self.rec_t1 = recs_t["t1"].astype(np.int64)
        self.rec_kind = recs_t["c"].astype(np.int64)
        self.rec_dur = recs_t["t1"].astype(np.int64) - recs_t["t0"].astype(np.int64)

    def wait_at(self, t):
        """The wait span containing t (t0 < t <= t1), else None; the waits do not overlap."""
        i = bisect.bisect_left(self.w_t1, t)
        if i < len(self.w_t1) and self.w_t0[i] < t <= self.w_t1[i]:
            return self.w[i]
        return None

    def wait_end_before(self, t):
        """The last wait that ended before t (index), else -1."""
        return bisect.bisect_left(self.w_t1, t) - 1

    def record_end(self, index):
        """When record `index` finished on thread 2 (its span's end), else None."""
        i = bisect.bisect_left(self.rec_b, index & 0xFFFFFFFF)
        if i < len(self.rec_b) and self.rec_b[i] == (index & 0xFFFFFFFF):
            return int(self.rec["t1"][i])
        return None


def read_frames_csv(path):
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            rows.append({k: (float(v) if v not in ("", None) else None) for k, v in r.items()})
    return rows


RULES = [  # (bucket, regex on the exe function names, from the leaf) - the first frame that matches decides
    ("Трансляция шейдеров", re.compile(r"^Shader::|^Sirit::|ShaderTranslator|shader_translator_host|TranslateProgram")),
    ("VirtualProtect / учёт страниц", re.compile(r"PageTracker::|VirtualProtect|NtProtectVirtualMemory")),
    ("Создание pipeline (наш код)", re.compile(r"PipelineFactory::")),
    ("Буферы: BufferCache (загрузка, учёт)", re.compile(r"BufferCache::")),
    ("Буферы: GuestRange/StreamRing/копии", re.compile(r"GuestRange|GuestBuffers::|StreamRing::|PrepareShadows|"
                                                       r"FinishShadows|CopySparse|TryWriteBacking|BackingRun")),
    ("Привязки (дескрипторы)", re.compile(r"BindStage|PushDescriptors")),
    ("Образы (ImageCache)", re.compile(r"ImageCache::|LayoutFromTSharp|ImageLayout|Detile")),
    ("Фронтенд шейдеров", re.compile(r"SrtProgram|ShaderFrontend::|ProgramCache::|PipelineStateCache|SpecializationKey|"
                                     r"ResolveResources|DecodeResources|TimelineReader|FrontReader|Gpu::Shader::|"
                                     r"RuntimeParams|FetchCache")),
    ("Динамика/барьеры draw/dispatch", re.compile(r"SetDynamicState|SplitMergeLevels|NeedsBarrier|NoteAccesses|"
                                                  r"GlobalBarrier|EndRendering|RecordDraw|RecordDispatch|"
                                                  r"Executor::Draw|Executor::Dispatch|Executor::Insp")),
    ("Сабмит/опрос/флип", re.compile(r"Executor::Flush|Executor::Poll|Executor::Begin|Timeline::|RecordWriteBack|"
                                     r"Executor::Fence|Executor::Flip|Presenter::")),
    ("Очередь P4.10: спин/ожидание", re.compile(r"ParallelCp::WaitHead|ParallelCp::Drain")),
    ("Очередь P4.10: журнал регистров", re.compile(r"ParallelCp::On(Write|Load|Reset)|JournalReserve|ApplyJournal|"
                                                   r"RegisterSink|RegisterFile::")),
    ("Очередь P4.10: подготовка записи (копии, учёт GPU-записей)",
     re.compile(r"ParallelCp::(Prepare|BeginRecord|PushRecord|Draw|Dispatch|NoteStageWrites|MayBeGpuWritten|"
                r"AddGpuWrite|RetireGpuWrites|RebuildGpuWriteBits)")),
    ("Очередь P4.10: исполнение записи (поток 2)", re.compile(r"ParallelCp::(Loop|Execute|RunCopy)")),
    ("Очередь P4.10: прочее", re.compile(r"ParallelCp::")),
    ("PM4 разбор", re.compile(r"CommandProcessor|Pm4|Cp::")),
    ("Инвалидации (записи CPU игры)", re.compile(r"DrainCpuWrites|::Invalidate")),
    ("Трасса P4.12", re.compile(r"StallTrace::")),
]
DRIVER = ("nvoglv64.dll", "nvgpucomp64.dll", "vulkan-1.dll", "nvldumdx.dll")
MEMCPY = re.compile(r"^(memmove|memcpy|memset|__memmove|__memcpy|__memset)")
WAITFN = re.compile(r"NtWaitFor|ZwWaitFor|NtDelayExecution|WaitOnAddress|RtlWaitOnAddress|SleepConditionVariable|"
                    r"WaitForSingleObject|NtWaitForAlertByThreadId|_Thrd_sleep|SwitchToThread|NtYieldExecution")


def classify(frames, sym):
    names = [sym.name(a) for a in frames]
    if not names:
        return "?", ""
    leaf_mod, leaf = names[0]
    exe_names = [n for m, n in names if m == "exe"]
    ctx = exe_names[0] if exe_names else ""
    if any(WAITFN.search(n) for _, n in names[:5]):
        return "Ожидание (системный вызов)", ctx
    bucket = None
    for m, n in names:
        if m != "exe" or MEMCPY.search(n):
            continue
        for b, rx in RULES:
            if rx.search(n):
                bucket = b
                break
        if bucket:
            break
    if leaf_mod in DRIVER:
        return f"Драйвер Vulkan ← {bucket or ctx[:60]}", ctx
    if MEMCPY.search(leaf):
        return f"memcpy ← {bucket or ctx[:60]}", ctx
    if leaf_mod not in ("exe", "guest") and bucket is None:
        return f"Система ({leaf_mod})", ctx
    return bucket or ("Прочее SecondSonPC: " + ctx[:60] if ctx else "?"), ctx


def analyze(run_dir, exe=None):
    tr = load_trace(os.path.join(run_dir, "p412_trace.bin"))
    hz = tr["tsc_hz"]
    ms = 1000.0 / hz
    th = {t["role"]: Thread(t) for t in tr["threads"] if t["role"] in ("CP", "Record", "Present")}
    games = [Thread(t) for t in tr["threads"] if t["role"] == "Game"]
    cp = th["CP"]
    rec = th.get("Record")
    parallel = rec is not None and len(rec.ev) > 0
    # completions seen by thread 1 (EOP written then), flip markers
    gc = cp.ev[cp.ev["type"] == EV["GpuCompleted"]]
    comp_vals = gc["a"].astype(np.int64)
    comp_ts = gc["t0"].astype(np.int64)
    order = np.argsort(comp_vals, kind="stable")
    comp_vals_s, comp_ts_s = comp_vals[order], comp_ts[order]
    fm = cp.ev[cp.ev["type"] == EV["FlipMarker"]]
    conv, cal_info = calibrate(tr, (list(comp_vals_s), list(comp_ts_s)))
    b = tr["batches"]
    b = b[np.argsort(b["value"], kind="stable")]
    gs = conv(b["gpu_t0"]) if conv else None
    ge = conv(b["gpu_t1"]) if conv else None
    bval = b["value"].astype(np.int64)
    # which thread submitted each batch (its Submit span: a = value)
    sub_thread = {}
    for role, t in th.items():
        s = t.ev[t.ev["type"] == EV["Submit"]]
        for v in s["a"]:
            sub_thread[int(v)] = role
    # the frame log (the game's first submission per frame)
    frows = read_frames_csv(os.path.join(run_dir, "gpu_frames.csv"))
    start = tr["start_tsc"]

    def tsc_of(ms_val):
        return None if ms_val is None else start + ms_val * hz / 1000.0
    by_n = {int(r["n"]): r for r in frows}
    # frames: flip marker n (a = its timeline value) -> the first completion >= value is its end
    frames = []
    for row in fm:
        n, val = int(row["b"]), int(row["a"])
        i = bisect.bisect_left(comp_vals_s, val)
        if i >= len(comp_vals_s):
            continue
        frames.append({"n": n, "value": val, "cp_flip": int(row["t0"]), "end": int(comp_ts_s[i])})
    frames.sort(key=lambda f: f["n"])
    for prev, f in zip(frames, frames[1:]):
        if f["n"] == prev["n"] + 1:
            f["start"] = prev["end"]
    frames = [f for f in frames if "start" in f and f["start"] >= tr["win0"] and f["end"] <= tr["win1"]]

    def batch_idx(value):
        i = bisect.bisect_left(bval, value)
        return i if i < len(bval) and bval[i] == value else None

    def batch_done_before(t, upto_value=None):
        """The latest batch whose calibrated end is <= t (and value <= upto_value)."""
        if ge is None:
            return None
        cand = np.nonzero(ge <= t)[0]
        if upto_value is not None:
            cand = cand[bval[cand] <= upto_value]
        return int(cand[np.argmax(ge[cand])]) if len(cand) else None

    eps = 2e-6 * hz  # 2 us: "at the same time" between two clocks
    busy_after = collections.defaultdict(float)  # the path's busy time by the wait it followed (ms, all frames)

    def walk(f):
        segs = []  # (t0, t1, label, where)
        lo, T = f["start"], float(f["end"])
        game_submit = tsc_of(by_n.get(f["n"], {}).get("game_submit_ms"))

        def add(a, bb, label, where):
            a, bb = max(a, lo), min(bb, f["end"])
            if bb > a:
                segs.append((a, bb, label, where))
        loc = ("gpu", batch_idx(f["value"]))
        if loc[1] is not None and ge is not None and ge[loc[1]] <= T:
            add(float(ge[loc[1]]), T, "EOP observation (GPU done -> thread 1 saw it)", "CP")
            T = float(ge[loc[1]])
        else:
            loc = ("CP", None)
        guard = 0
        while T > lo and guard < 200000:
            guard += 1
            if loc[0] == "gpu":
                i = loc[1]
                s = float(gs[i])
                sub1 = float(b["submit_t1"][i])
                prev_e = float(ge[i - 1]) if i > 0 else None
                if prev_e is not None and prev_e > sub1 + eps and prev_e > lo:
                    # submitted while the GPU was busy with the batch before: the GPU's own chain
                    busy0 = max(s, prev_e)
                    add(busy0, T, "GPU executing the frame's batches", "GPU")
                    if s > prev_e + eps:
                        add(prev_e, s, "GPU idle with a batch submitted (bind wait / driver)", "GPU")
                    T = min(T, prev_e)
                    loc = ("gpu", i - 1)
                    continue
                # the GPU waited for this submission
                start_exec = max(s, sub1)
                add(start_exec, T, "GPU executing the frame's batches", "GPU")
                if s > sub1:
                    add(sub1, s, "GPU start after the submission", "GPU")
                T = min(T, min(s, sub1))
                loc = (sub_thread.get(int(bval[i]), "Record" if parallel else "CP"), None)
                continue
            t = th[loc[0]]
            w = t.wait_at(T)
            if w is None:
                k = t.wait_end_before(T)
                busy0 = float(t.w_t1[k]) if k >= 0 else lo
                add(busy0, T, f"thread {'1' if loc[0] == 'CP' else '2'} busy", loc[0])
                # which wait the busy stretch followed (a drain's reason): the pipeline's refill after each kind
                if k >= 0 and min(T, f["end"]) > max(busy0, lo):
                    pw = t.w[k]
                    nm = EVENTS[int(pw["type"])]
                    if nm == "Drain":
                        c = int(pw["c"])
                        nm += ": " + (DRAIN_REASONS[c] if c < len(DRAIN_REASONS) else str(c))
                    busy_after[f"{'thread 1' if loc[0] == 'CP' else 'thread 2'} busy after {nm}"] += \
                        (min(T, f["end"]) - max(busy0, lo)) * ms
                T = busy0
                if k < 0 or T <= lo:
                    break
                continue
            typ = EVENTS[int(w["type"])]
            w0 = float(w["t0"])
            if typ in ("Drain", "QueueFull", "JournalFull") and parallel:
                rel = rec.record_end(int(w["a"]) - 1)
                rel = min(float(rel), T) if rel is not None and rel > w0 else None
                if rel is None:
                    add(w0, T, "untraced gap", "CP")
                    T = w0
                    continue
                add(rel, T, "thread 1 wake-up after a drain", "CP")
                T, loc = rel, ("Record", None)
            elif typ == "RecordWait":
                push = float(w["a"])
                rel = min(max(push, w0), T) if push > 0 else w0
                add(rel, T, "thread 2 wake-up after a push", "Record")
                T, loc = rel, ("CP", None)
            elif typ == "IdleGpu" and game_submit is not None and w0 < game_submit < T:
                # the game's submission arrived while thread 1 waited for a batch (WaitOldest: the submission does not
                # wake it - only a completion or the 1 ms timeout): the frame's start waited for that
                add(game_submit, T, "submission pickup delayed (thread 1 in a GPU wait)", "CP")
                add(max(w0, lo), game_submit, "game answer (EOP seen -> first submission)", "Game")
                T = lo
            elif typ in ("IdleGpu", "GpuSync", "RingWait"):
                upto = int(w["a"]) if typ in ("GpuSync", "RingWait") else None
                bi = batch_done_before(T, upto)
                if bi is None or ge[bi] <= w0 - eps:
                    add(w0, T, "thread 1 idle (GPU wait ended without a completion)" if typ == "IdleGpu"
                        else "untraced gap", loc[0])
                    T = w0
                    continue
                rel = float(ge[bi])
                add(rel, T, "EOP observation (GPU done -> thread 1 saw it)" if typ == "IdleGpu"
                    else "CPU wake-up after a GPU wait", loc[0])
                T, loc = rel, ("gpu", bi)
            elif typ == "IdleEmpty":
                a0 = max(w0, lo)
                if game_submit is not None and a0 < game_submit < T:
                    add(game_submit, T, "thread 1 wake-up (submission -> processing)", "CP")
                    add(a0, game_submit, "game answer (EOP seen -> first submission)", "Game")
                else:
                    add(a0, T, "thread 1 waits for the game (submissions)", "CP")
                T = w0
            elif typ == "IdleLabel":
                add(w0, T, "WAIT_REG_MEM on a VideoOut label (flip not shown yet)", "CP")
                T = w0
            elif typ == "IdleFlag":
                add(w0, T, "WAIT_REG_MEM on a flag the game's CPU writes", "CP")
                T = w0
            elif typ == "QueueLock":
                add(w0, T, "Vulkan queue lock (another thread submits/presents)", loc[0])
                T = w0
            elif typ == "FlipAcquire":
                add(w0, T, "flip waits for a presenter frame", loc[0])
                T = w0
            else:
                add(w0, T, "untraced gap", loc[0])
                T = w0
        return segs

    # the stack samples, symbolized and bucketed (per thread)
    samples = tr["samples"]
    buckets = {}
    if len(samples):
        sym = pi.Symbols(tr["modules"], exe_path=exe)
        addrs = set()
        for s in samples:
            for k in range(s["n"]):
                a = int(s["pcs"][k])
                addrs.add(a if k == 0 else a - 1)
        sym.resolve(sorted(addrs))
        for idx, s in enumerate(samples):
            fr = [int(s["pcs"][k]) if k == 0 else int(s["pcs"][k]) - 1 for k in range(s["n"])]
            buckets[idx] = classify(fr, sym)
    samp_t = samples["tsc"].astype(np.int64) if len(samples) else np.zeros(0, np.int64)
    samp_role = samples["role"] if len(samples) else np.zeros(0, np.uint8)

    # walk every frame
    agg = collections.defaultdict(float)
    per_frame = []
    busy_segs = {"CP": [], "Record": []}
    all_segs = []
    for f in frames:
        segs = walk(f)
        dur = (f["end"] - f["start"]) * ms
        covered = sum(s[1] - s[0] for s in segs) * ms
        comp = collections.defaultdict(float)
        for a, bb, label, where in segs:
            comp[label] += (bb - a) * ms
            if label in ("thread 1 busy", "thread 2 busy"):
                busy_segs["CP" if label.startswith("thread 1") else "Record"].append((a, bb))
        comp["untraced gap"] += max(0.0, dur - covered)
        for k, v in comp.items():
            agg[k] += v
        row = {"n": f["n"], "t_s": round((f["end"] - start) / hz, 3), "emu_ms": round(dur, 3),
               "path": {k: round(v, 3) for k, v in sorted(comp.items(), key=lambda kv: -kv[1]) if v > 0}}
        r = by_n.get(f["n"])
        if r is not None:
            row["present_ms"] = r.get("present_ms")
        per_frame.append(row)
        all_segs.append((f, segs))
    nf = max(1, len(per_frame))
    path_mean = {k: v / nf for k, v in sorted(agg.items(), key=lambda kv: -kv[1])}

    # the frame's phases (hypotheses about batching and the threads' pipelining - measured per frame):
    #   gpu_start_gap   frame start (EOP seen) -> the GPU starts the frame's first batch
    #   cpu_phase       the frame's first submission picked up by thread 1 -> the flip batch submitted
    #   t1_busy/t2_busy each thread's time outside its waits inside cpu_phase
    #   serial_loss     cpu_phase - max(t1_busy, t2_busy): what the two threads lose by waiting for each other
    #   backlog_at_flip GPU work (calibrated execution) still to run when the flip batch was submitted
    #   tail            flip batch submitted -> thread 1 saw the frame complete
    def busy_in(t, a, bb):
        if t is None or bb <= a:
            return 0.0
        i0 = bisect.bisect_left(t.w_t1, a)
        tot = 0.0
        for k in range(i0, len(t.w_t1)):
            w0, w1 = t.w_t0[k], t.w_t1[k]
            if w0 >= bb:
                break
            tot += max(0.0, min(w1, bb) - max(w0, a))
        return (bb - a) - tot
    phases = []
    for f in frames:
        lo_f, hi_f = f["start"], f["end"]
        idx = [i for i in range(len(bval)) if b["submit_t0"][i] >= lo_f and bval[i] <= f["value"]]
        fi = batch_idx(f["value"])
        if not idx or fi is None or ge is None:
            continue
        first = min(idx)
        sub_last = float(b["submit_t1"][fi])
        gsub = tsc_of(by_n.get(f["n"], {}).get("game_submit_ms"))
        cpu0 = max(lo_f, gsub) if gsub is not None else lo_f
        t1b = busy_in(cp, cpu0, sub_last)
        t2b = busy_in(rec, cpu0, sub_last) if parallel else 0.0
        backlog = 0.0
        covered = sub_last  # the union of the GPU's execution intervals after the flip batch's submission
        for i in range(len(bval)):
            if ge[i] > sub_last and bval[i] <= f["value"]:
                a0 = max(float(gs[i]), covered)
                if ge[i] > a0:
                    backlog += float(ge[i]) - a0
                covered = max(covered, float(ge[i]))
        phases.append({"gpu_start_gap": (float(gs[first]) - lo_f) * ms, "cpu_phase": (sub_last - cpu0) * ms,
                       "t1_busy": t1b * ms, "t2_busy": t2b * ms,
                       "serial_loss": ((sub_last - cpu0) - max(t1b, t2b)) * ms if parallel else 0.0,
                       "backlog_at_flip": backlog * ms, "tail": (hi_f - sub_last) * ms})
    phase_mean = {k: round(sum(p[k] for p in phases) / len(phases), 3) for k in phases[0]} if phases else {}

    # busy time on the path split by the samples that fall inside the path's busy segments
    busy_split = {}
    role_code = {"CP": 0, "Record": 1}
    for role, segs in busy_segs.items():
        if not segs or not len(samp_t):
            continue
        segs.sort()
        starts = np.array([s[0] for s in segs])
        ends = np.array([s[1] for s in segs])
        idxs = np.nonzero(samp_role == role_code[role])[0]
        counts = collections.Counter()
        all_counts = collections.Counter()
        for i in idxs:
            t = samp_t[i]
            all_counts[buckets[i][0]] += 1
            j = np.searchsorted(starts, t, side="right") - 1
            if j >= 0 and t <= ends[j]:
                counts[buckets[i][0]] += 1
        total = sum(counts.values())
        busy_ms = path_mean.get(f"thread {'1' if role == 'CP' else '2'} busy", 0.0)
        busy_split[role] = {"samples_on_path": total, "samples_all": sum(all_counts.values()),
                            "on_path_ms_per_frame": {k: round(busy_ms * v / total, 3) for k, v in counts.most_common()}
                            if total else {},
                            "all_share": {k: round(v / max(1, sum(all_counts.values())), 4)
                                          for k, v in all_counts.most_common()}}
    # thread 2's records on the path: kinds (the records whose end falls into the path's busy segments of thread 2)
    rec_on_path = {}
    if parallel and busy_segs["Record"]:
        segs = sorted(busy_segs["Record"])
        starts = np.array([s[0] for s in segs])
        ends = np.array([s[1] for s in segs])
        j = np.searchsorted(starts, rec.rec_t1, side="right") - 1
        ok = (j >= 0) & (rec.rec_t1 <= ends[np.clip(j, 0, len(ends) - 1)])
        for kind in np.unique(rec.rec_kind[ok]):
            sel = ok & (rec.rec_kind == kind)
            rec_on_path[RECORD_KINDS[int(kind)] if kind < len(RECORD_KINDS) else str(kind)] = {
                "per_frame": round(float(np.sum(sel)) / nf, 1),
                "ms_per_frame": round(float(np.sum(rec.rec_dur[sel])) * ms / nf, 3),
                "mean_us": round(float(np.mean(rec.rec_dur[sel])) * ms * 1000.0, 2) if np.any(sel) else 0.0}

    # every record thread 2 executed in the frames, by kind (on the path or not)
    rec_all = {}
    if parallel and frames:
        sel = (rec.rec_t1 >= frames[0]["start"]) & (rec.rec_t1 <= frames[-1]["end"])
        for kind in np.unique(rec.rec_kind[sel]):
            m = sel & (rec.rec_kind == kind)
            d = rec.rec_dur[m] * ms * 1000.0
            rec_all[RECORD_KINDS[int(kind)] if kind < len(RECORD_KINDS) else str(kind)] = {
                "per_frame": round(float(np.sum(m)) / len(frames), 1),
                "ms_per_frame": round(float(np.sum(d)) / 1000.0 / len(frames), 3),
                "mean_us": round(float(np.mean(d)), 2), "p95_us": round(float(np.percentile(d, 95)), 2),
                "p99_us": round(float(np.percentile(d, 99)), 2), "max_us": round(float(np.max(d)), 1)}
    # every wait in the frames: totals per frame and percentiles (whether it is on the path: the path's own figures)
    lo_all = frames[0]["start"] if frames else tr["win0"]
    hi_all = frames[-1]["end"] if frames else tr["win1"]
    waits = []
    for role, t in list(th.items()) + [("Game", g) for g in games]:
        ev = t.ev
        sel = ev[(ev["t1"] >= lo_all) & (ev["t0"] <= hi_all)]
        for typ in np.unique(sel["type"]):
            e = sel[sel["type"] == typ]
            d = (e["t1"].astype(np.int64) - e["t0"].astype(np.int64)) * ms
            name = EVENTS[int(typ)]
            extra = {}
            if name == "Drain":
                by = collections.Counter()
                for c, dd in zip(e["c"], d):
                    by[DRAIN_REASONS[int(c)] if c < len(DRAIN_REASONS) else str(int(c))] += dd
                extra["by_reason_ms_per_frame"] = {k: round(v / nf, 3) for k, v in by.most_common()}
            waits.append({"thread": role, "event": name, "count_per_frame": round(len(e) / nf, 2),
                          "total_ms_per_frame": round(float(np.sum(d)) / nf, 3),
                          "p50_us": round(float(np.percentile(d, 50)) * 1000, 2) if len(d) else 0,
                          "p95_us": round(float(np.percentile(d, 95)) * 1000, 2) if len(d) else 0,
                          "p99_us": round(float(np.percentile(d, 99)) * 1000, 2) if len(d) else 0,
                          "max_us": round(float(np.max(d)) * 1000, 1) if len(d) else 0, **extra})
    waits.sort(key=lambda w: -w["total_ms_per_frame"])

    # the GPU: busy/gaps in the frames (calibrated), gaps by whether the next batch had been submitted
    gpu = {}
    if ge is not None:
        m = (ge >= lo_all) & (gs <= hi_all)
        s_, e_ = gs[m], ge[m]
        sub1 = b["submit_t1"][m].astype(np.float64)
        busy = 0.0
        last = None
        gaps = []
        for i in range(len(s_)):
            a0 = s_[i] if last is None else max(s_[i], last)
            if e_[i] > a0:
                busy += e_[i] - a0
            if last is not None and s_[i] > last:
                gaps.append((last, s_[i], sub1[i]))
            last = e_[i] if last is None else max(last, e_[i])
        span = hi_all - lo_all
        gap_wait_submit = sum(min(g[1], g[2]) - g[0] for g in gaps if g[2] > g[0])
        gap_after_submit = sum(g[1] - max(g[0], g[2]) for g in gaps if g[1] > max(g[0], g[2]))
        gpu = {"busy_ms_per_frame": round(busy * ms / nf, 3), "frames_span_ms_per_frame": round(span * ms / nf, 3),
               "idle_ms_per_frame": round((span - busy) * ms / nf, 3), "gaps_per_frame": round(len(gaps) / nf, 2),
               "idle_waiting_for_submission_ms_per_frame": round(gap_wait_submit * ms / nf, 3),
               "idle_after_submission_ms_per_frame": round(gap_after_submit * ms / nf, 3),
               "batches_per_frame": round(float(np.sum(m)) / nf, 2)}
    emu = sorted(per_frame, key=lambda r: -r["emu_ms"])
    med = sorted(r["emu_ms"] for r in per_frame)[len(per_frame) // 2] if per_frame else 0.0
    # the slowest frames: what their path's busy time ran (the samples inside their busy segments; ~1 per ms) and
    # the frame log's figures (commands, compilation, the GPU)
    segs_by_n = {f["n"]: segs for f, segs in all_segs}
    for r in emu[:8]:
        cnt = collections.Counter()
        for a, bb, label, where in segs_by_n.get(r["n"], []):
            if label not in ("thread 1 busy", "thread 2 busy") or not len(samp_t):
                continue
            code = 0 if label.startswith("thread 1") else 1
            i0, i1 = np.searchsorted(samp_t, a), np.searchsorted(samp_t, bb)
            for i in range(i0, i1):
                if samp_role[i] == code:
                    cnt[("T1 " if code == 0 else "T2 ") + buckets[i][0]] += 1
        r["busy_samples"] = dict(cnt.most_common(8))
        fr = by_n.get(r["n"])
        if fr is not None:
            r["log"] = {k: fr.get(k) for k in ("cp_ms", "gpu_busy_ms", "draws", "dispatches", "shaders", "shader_ms",
                                               "pipelines", "pipeline_ms", "par_wait_ms", "par_busy_ms")}
    res = {"run": run_dir, "calibration": cal_info, "parallel": parallel, "frames": len(per_frame),
           "window_s": [round((tr["win0"] - start) / hz, 2), round((tr["win1"] - start) / hz, 2)],
           "samples": int(len(samples)),
           # how long the sampler held each thread suspended per frame (traces from before the field: 0)
           "sampler_suspend_ms_per_frame": {
               role: round(float(np.sum(samples["pad"][samples["role"] == code])) / 1000.0 / nf, 4)
               for role, code in (("CP", 0), ("Record", 1))} if len(samples) else {},
           "sample_hz_actual_per_thread": round(len(samples) / max(1e-9, (tr["win1"] - tr["win0"]) / hz) /
                                                (2 if parallel else 1), 1),
           "dropped": {t["role"]: t["dropped"] for t in tr["threads"]},
           "emu_ms_mean": round(float(np.mean([r["emu_ms"] for r in per_frame])), 3) if per_frame else None,
           "emu_ms_median": med,
           "path_ms_per_frame": {k: round(v, 3) for k, v in path_mean.items()},
           "path_busy_after_wait_ms_per_frame": {k: round(v / nf, 3)
                                                 for k, v in sorted(busy_after.items(), key=lambda kv: -kv[1])},
           "busy_split": busy_split, "records_on_path": rec_on_path, "waits": waits, "gpu": gpu,
           "phases": phase_mean, "records_all": rec_all,
           "slowest": emu[:8], "per_frame": per_frame}
    # a few frames' full timelines for the HTML (the median one and the slowest ones)
    pick = set(r["n"] for r in emu[:5])
    if per_frame:
        mid = sorted(per_frame, key=lambda r: r["emu_ms"])[len(per_frame) // 2]["n"]
        pick.add(mid)
        res["median_frame_n"] = mid
    timelines = []
    for f, segs in all_segs:
        if f["n"] not in pick:
            continue
        lo, hi = f["start"], f["end"]
        lanes = {}
        for role, t in th.items():
            ev = t.ev
            sel = ev[(ev["t1"] >= lo) & (ev["t0"] <= hi) & (ev["type"] != EV["Record"])]
            lanes[role] = [[round((max(int(e["t0"]), lo) - lo) * ms, 4), round((min(int(e["t1"]), hi) - lo) * ms, 4),
                            EVENTS[int(e["type"])]] for e in sel if int(e["t1"]) > int(e["t0"])]
        if ge is not None:
            m = (ge >= lo) & (gs <= hi)
            lanes["GPU"] = [[round((max(float(x), lo) - lo) * ms, 4), round((min(float(y), hi) - lo) * ms, 4), "batch"]
                            for x, y in zip(gs[m], ge[m])]
        timelines.append({"n": f["n"], "emu_ms": round((hi - lo) * ms, 3), "lanes": lanes,
                          "path": [[round((a - lo) * ms, 4), round((bb - lo) * ms, 4), label, where]
                                   for a, bb, label, where in sorted(segs)]})
    res["timelines"] = timelines
    return res


def main(argv):
    sys.stdout.reconfigure(encoding="utf-8")
    run = argv[1]
    exe, out = None, None
    i = 2
    while i < len(argv):
        if argv[i] == "--exe":
            exe = argv[i + 1]
            i += 2
        elif argv[i] == "--json":
            out = argv[i + 1]
            i += 2
        else:
            raise SystemExit(f"unknown argument {argv[i]}")
    res = analyze(run, exe)
    out = out or os.path.join(run, "p412_analysis.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(res, f, indent=1, ensure_ascii=False)
    print(f"{run}: {res['frames']} frames, emulation frame mean {res['emu_ms_mean']} ms (median {res['emu_ms_median']}),"
          f" calibration {res['calibration']}")
    print("critical path, ms/frame:")
    for k, v in res["path_ms_per_frame"].items():
        print(f"  {v:7.3f}  {k}")
    for role, s in res["busy_split"].items():
        print(f"busy on the path, {role} ({s['samples_on_path']} samples of {s['samples_all']}):")
        for k, v in list(s["on_path_ms_per_frame"].items())[:14]:
            print(f"  {v:7.3f}  {k}")
    print("GPU:", res["gpu"])
    print("phases (ms/frame):", res["phases"])
    print("records on the path:", res["records_on_path"])
    print("waits (ms/frame):")
    for w in res["waits"][:16]:
        print(f"  {w['thread']:8s} {w['event']:16s} {w['count_per_frame']:8.1f}/fr {w['total_ms_per_frame']:8.3f} ms "
              f"p50 {w['p50_us']:9.1f} p95 {w['p95_us']:9.1f} p99 {w['p99_us']:9.1f} max {w['max_us']:9.1f} us")


if __name__ == "__main__":
    main(sys.argv)
