# R10 pacing: every batch (both queues) and the game's/CP's key events around one time (ms of the frame log)
#   python -I -X utf8 tools/r10/around.py <run> <t0 ms> <t1 ms>
import os, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "p412"))
import p412_analyze as pa
EV = pa.EV
tr = pa.load_trace(os.path.join(sys.argv[1], "p412_trace.bin"))
hz = tr["tsc_hz"]; T0 = tr["start_tsc"]
ms = lambda t: (float(t) - T0) / hz * 1e3
lo, hi = float(sys.argv[2]), float(sys.argv[3])
allev = np.concatenate([t["ev"] for t in tr["threads"]])
gc = allev[allev["type"] == EV["GpuCompleted"]]
o = np.argsort(gc["a"])
conv, info = pa.calibrate(tr, (list(gc["a"][o].astype(np.int64)), list(gc["t0"][o].astype(np.int64))))
aqs = {int(x["a"]): x for x in allev[allev["type"] == EV["AqSubmit"]]}
ev = []
for x in tr["batches"]:
    s = aqs.get(int(x["value"]))
    if s is None: continue
    ts = ms(s["t1"]); gs = ms(conv(x["gpu_t0"])); ge = ms(conv(x["gpu_t1"]))
    if hi < ts or ge < lo: continue
    c = int(s["c"])
    ev.append((ts, f"submit {'LANE' if x['pad'] & 1 else 'main'} v{int(x['value'])} q{c >> 8:#x} fl{c & 0xff:#x} waits v{int(s['b'])} -> GPU {gs:9.2f}..{ge:9.2f} ({ge - gs:5.2f})"))
names = {"GameComputeWait": "game wait", "GameSubmitFrame": "SubmitFrame", "FlipMarker": "FLIP MARKER", "Present": "present",
         "GameEarlySubmit": "early submit"}
for k, v in names.items():
    for x in allev[allev["type"] == EV[k]]:
        t = ms(x["t0"])
        if lo <= t <= hi:
            ev.append((t, f"{v} a={int(x['a']):#x} b={int(x['b'])} c={int(x['c'])} ..{ms(x['t1']):9.2f}"))
for t, s in sorted(ev):
    print(f"{t:9.2f} {s}")
