"""R10 sysrt: which imports of the game are still not SecondSonPC's own (docs/R10_SYSRT.md).

    python -I tools/r10/sysrt_deps.py --run base=<run dir> [--run native=<run dir> ...]
                                      [--root <worktree>] [--md FILE] [--csv FILE]

Re-run after any merge: nothing here is a hand-kept list of owners.
  * every import of eboot.bin / libc.prx / libSceFios2.prx and its kind: src/loader/generated/import_table.inc;
  * who serves each one in a run: the loader's "binding table" block in <run dir>/SecondSonPC.log (it names every
    NATIVE / OBSERVED / COMMON_STUB / UNRESOLVED row; LLE rows come from the table; every other row is BRIDGE);
  * how often the game called it: <run dir>/calls.json (the import counter; BRIDGE rows are listed there only when
    called, so a missing BRIDGE row is 0 calls). Without calls.json the calls are "?".
The last --run is the "after" state. Rows that are NATIVE/OBSERVED-free in every run (or LLE) are counted, not listed.

Classes (column "class"):
  own      SecondSonPC's in the last run;
  ajm      libSceAjm: agent ajm's scope (R10), not sysrt's;
  called   BRIDGE or COMMON_STUB and called in a run: needs an owner or a recorded reason (NOTES);
  idle     BRIDGE, imported but called in no run given;
  stub     COMMON_STUB (nobody implements it; a generic stub returns 0; the loader logs every call);
  object   UNRESOLVED data object (the slot stays 0).
NOTES adds the reason/ABI/side effects for rows whose class alone does not explain them.
"""
from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from pathlib import Path

ROW = re.compile(r'\{"(?P<nid>[^"]*)", "(?P<lib>[^"]*)", "(?P<mod>[^"]*)", "(?P<name>[^"]*)", Kind::(?P<kind>\w+), '
                 r'"(?P<sub>[^"]*)", (?P<imp>[^,]+), Phase0::(?P<p0>\w+)\}')
SUMMARY = re.compile(r"binding table: (\d+) imports = NATIVE (\d+) \+ OBSERVED (\d+) \+ BRIDGE (\d+) \+ "
                     r"COMMON_STUB (\d+) \+ UNRESOLVED (\d+) \(objects\) \| LLE (\d+)")
LISTED = re.compile(r"\[Loader\] \(tid \d+\)   (NATIVE|OBSERVED|COMMON_STUB|UNRESOLVED)\s+(\S+)\s+([^/\s]+)/(\S+)")
IMPORTERS = {"kEboot": "eboot", "kLibc": "libc", "kFios2": "Fios2"}

# name -> (ABI, side effects, priority, note). Only what the class does not say; [E]/[A]/[H] as in R10_RULES.md.
NOTES = {
    "sceCommonDialogInitialize": ("SysV, () -> s32", "module state: initialized flag read by MsgDialog/SaveData-/"
                                  "ErrorDialog", "P1", "sysrt-dialog; eboot 0x15d7d9 ignores the result [A]"),
    "sceNetInit": ("SysV, () -> s32", "libSceNet state", "P1",
                   "sysrt-net; eboot 0x5ed272 compares with 0 and 0x804101c8 [A]"),
    "sceNetPoolCreate": ("SysV, (const char*, s32 size, s32 flags) -> s32 id", "pool table 3000..3030",
                         "P1", "sysrt-net; eboot 0x23699f: size 0x4000, flags 0; id -> sceSslInit/sceHttpInit [A]"),
    "sceNetInetNtop": ("SysV, (s32 af, const void*, char*, u32) -> const char*", "net errno", "P1",
                       "sysrt-net; eboot 0x5fd253: AF_INET, buffer 0x20 [A]"),
    "fflush": ("SysV, (FILE*) -> int", "libc stdout buffer", "P1",
               "sysrt-libc; Fios2 0x38650 after vprintf, FILE = _Stdout slot [A]"),
    "vprintf": ("SysV, (const char*, va_list) -> int; guest va_list = SysV struct", "stdout text", "P1",
                "sysrt-libc; Fios2 0x38650 log printer [A]"),
    "sceLibcHeapGetTraceInfo": ("SysV, (HeapTraceInfo* 32 B) -> void", "heap trace state of libc.prx", "P1",
                                "sysrt-libc; libc.prx 0x5b8dc/0x5bd79 [A]"),
    "_Stdout": ("data object (FILE)", "-", "P1", "sysrt-libc exports the object: Fios2's fflush(_Stdout) gets a "
                "real FILE* instead of NULL [A]"),
    "Need_sceLibcInternal": ("data object (link marker)", "-", "P3", "link marker of libSceLibcInternal; its "
                             "slot (Fios2 0x60068) is read by no instruction (data_refs.py) [A]"),
}
# Reasons for whole groups of the remainder, by library or name prefix: (side effects, note).
GROUP_NOTES = [
    ("sceSaveDataDialog", "system UI (save list) over the game", "the save-list system dialog: Settings > Save opens it "
     "[E]; drawn by the bridge's UI layer - an own one needs a system dialog UI on SecondSonPC's presenter (blocked: "
     "not a runtime function, separate task); CommonDialog state shared through the sysrt-dialog adapter"),
    ("sceMsgDialog", "system UI", "system message dialog; CommonDialog state shared through the sysrt-dialog adapter"),
    ("sceErrorDialog", "system UI", "system error dialog; CommonDialog state shared through the sysrt-dialog adapter"),
    ("sceNetEpoll", "host sockets", "offline: NetCtl (NATIVE) reports no network, the game opens no socket [E]; pools "
     "and errno shared through the sysrt-net adapter"),
    ("sceNetResolver", "host DNS", "offline, never started [E]; the bridge's resolvers take SecondSonPC's pool ids "
     "(sysrt-net adapter)"),
    ("sceNet", "host sockets", "offline: NetCtl (NATIVE) reports no network, the game opens no socket [E]; errno "
     "shared through the sysrt-net adapter"),
    ("sceKernelConvert", "-", "time zone conversion, never called [E]"),
    ("sceKernelGetPrtAperture", "GPU memory", "partially resident textures: GPU memory (Phase 4)"),
    ("msync", "memory", "memory API, never called [E]"),
    ("sceKernelMlock", "memory", "memory API, never called [E]"),
]
STUB_NOTE = ("returns 0, logged", "imported by {imp}, never called in any run [E] (the loader logs each COMMON_STUB "
             "call); exit/crash paths and Fios2's unused helpers")


def note_for(name: str, kind: str, cls: str, importers: str) -> tuple[str, str, str, str]:
    if name in NOTES:
        return NOTES[name]
    abi = "SysV" if kind == "Function" else "data object"
    prio = "P1" if cls == "called" else "P3"
    if cls == "stub":
        return abi, STUB_NOTE[0], prio, STUB_NOTE[1].format(imp=importers)
    if cls == "ajm":
        return abi, "", prio, "libSceAjm: R10 agent ajm"
    for prefix, effects, note in GROUP_NOTES:
        if name.startswith(prefix):
            return abi, effects, prio, note
    return abi, "", prio, ""


def parse_table(root: Path) -> list[dict]:
    rows = []
    for line in (root / "src" / "loader" / "generated" / "import_table.inc").read_text(encoding="utf-8").splitlines():
        m = ROW.search(line)
        if m:
            d = m.groupdict()
            d["importers"] = "+".join(IMPORTERS.get(p.strip(), p.strip()) for p in d["imp"].split("|"))
            rows.append(d)
    if not rows:
        sys.exit("import_table.inc: no rows parsed")
    return rows


def parse_run(path: Path, rows: list[dict]) -> dict:
    log = (path / "SecondSonPC.log").read_text(encoding="utf-8", errors="replace").splitlines()
    summary, listed, inside = None, {}, False
    for line in log:
        if summary is None:
            m = SUMMARY.search(line)
            if m:
                summary = [int(x) for x in m.groups()]
                inside = True
            continue
        m = LISTED.search(line)
        if inside and m:
            listed.setdefault((m.group(2), m.group(3), m.group(4)), []).append(m.group(1))
        elif inside:
            break  # the block ends at the first line that is not one of its rows
    if summary is None:
        sys.exit(f"{path}: no 'binding table' line in SecondSonPC.log (Loader Info log level?)")
    calls = None
    if (path / "calls.json").exists():
        data = json.loads((path / "calls.json").read_text(encoding="utf-8"))
        calls = {(e["nid"], e["library"], e["module"]): (e["owner"], int(e["calls"])) for e in data["entries"]}
    owners = {}
    for r in rows:
        key = (r["nid"], r["lib"], r["mod"])
        if r["p0"] == "Lle":
            owners[key] = "LLE"
            continue
        names = listed.get((r["name"], r["lib"], r["mod"]))
        owners[key] = names[0] if names and len(set(names)) == 1 else ("?" if names else "BRIDGE")
        if calls is not None and key in calls:
            owners[key] = calls[key][0]  # the counter has the NID: exact even when names repeat ("?")
    got = [sum(1 for o in owners.values() if o == x) for x in ("NATIVE", "OBSERVED", "BRIDGE", "COMMON_STUB",
                                                               "UNRESOLVED")]
    if got != summary[1:6]:
        print(f"WARNING {path}: owners {got} != log summary {summary[1:6]} (NATIVE OBSERVED BRIDGE STUB UNRES)",
              file=sys.stderr)
    return {"summary": summary, "owners": owners, "calls": calls}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    ap.add_argument("--run", action="append", required=True, help="label=<run dir with SecondSonPC.log [calls.json]>")
    ap.add_argument("--md", type=Path)
    ap.add_argument("--csv", type=Path)
    a = ap.parse_args()

    rows = parse_table(a.root)
    runs = []
    for spec in a.run:
        label, _, d = spec.partition("=")
        runs.append((label, Path(d), parse_run(Path(d), rows)))
    last = runs[-1][2]

    out_rows = []
    for r in rows:
        key = (r["nid"], r["lib"], r["mod"])
        owners = [run["owners"][key] for _, _, run in runs]
        if owners[-1] == "LLE" or all(o in ("NATIVE", "OBSERVED") for o in owners):
            continue
        calls = []
        for _, _, run in runs:
            if run["calls"] is None:
                calls.append(None)
            else:
                calls.append(run["calls"].get(key, ("", 0))[1])
        called = any(c for c in calls if c)
        o = owners[-1]
        if o in ("NATIVE", "OBSERVED"):
            cls = "own"
        elif r["lib"] == "libSceAjm":
            cls = "ajm"
        elif o == "COMMON_STUB":
            cls = "called" if called else "stub"
        elif o == "UNRESOLVED":
            cls = "object"
        else:
            cls = "called" if called else "idle"
        abi, effects, prio, note = note_for(r["name"], r["kind"], cls, r["importers"])
        out_rows.append({"nid": r["nid"], "name": r["name"], "library": r["lib"], "module": r["mod"],
                         "kind": r["kind"], "importers": r["importers"], "abi": abi, "owners": owners,
                         "calls": calls, "class": cls, "effects": effects, "priority": prio, "note": note,
                         "subsystem": r["sub"]})
    order = {"called": 0, "own": 1, "ajm": 2, "object": 3, "stub": 4, "idle": 5}
    out_rows.sort(key=lambda x: (order[x["class"]], x["library"], x["name"]))

    labels = [label for label, _, _ in runs]
    fmt = lambda c: "?" if c is None else str(c)
    md = ["| " + " | ".join(["run", "imports", "NATIVE", "OBSERVED", "BRIDGE", "COMMON_STUB", "UNRESOLVED", "LLE"])
          + " |", "|---|" + "---:|" * 7]
    for label, d, run in runs:
        md.append(f"| {label} | " + " | ".join(str(x) for x in run["summary"]) + " |")
    md += ["", "| NID | function | library/module | importers | ABI | " + " | ".join(f"{l} owner/calls" for l in labels)
           + " | class | priority | side effects | note |", "|---|---|---|---|---|" + "---|" * len(labels) + "---|---|---|---|"]
    for x in out_rows:
        per_run = [f"{o} / {fmt(c)}" for o, c in zip(x["owners"], x["calls"])]
        md.append(f"| `{x['nid']}` | `{x['name']}` | {x['library']}/{x['module']} | {x['importers']} | {x['abi']} | "
                  + " | ".join(per_run) + f" | {x['class']} | {x['priority']} | {x['effects']} | {x['note']} |")
    counts = {}
    for x in out_rows:
        counts[x["class"]] = counts.get(x["class"], 0) + 1
    md += ["", "classes (last run): " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items(), key=lambda kv: order[kv[0]]))]
    text = "\n".join(md) + "\n"
    if a.md:
        a.md.write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    if a.csv:
        with open(a.csv, "w", newline="", encoding="utf-8") as fh:
            w = csv.writer(fh)
            w.writerow(["nid", "name", "library", "module", "kind", "importers", "subsystem", "abi"]
                       + [f"{l}_owner" for l in labels] + [f"{l}_calls" for l in labels]
                       + ["class", "priority", "effects", "note"])
            for x in out_rows:
                w.writerow([x["nid"], x["name"], x["library"], x["module"], x["kind"], x["importers"], x["subsystem"],
                            x["abi"]] + x["owners"] + [fmt(c) for c in x["calls"]]
                           + [x["class"], x["priority"], x["effects"], x["note"]])
    print("classes (last run): " + ", ".join(f"{k} {v}" for k, v in counts.items()), file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
