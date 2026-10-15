import csv,json,math,re,statistics
from pathlib import Path
from datetime import datetime,timedelta
base=Path(r"E:\SecondSonPC_AI\p415a")
def metric(rows,low=None,high=None):
    pres=[float(r["present_ms"]) for r in rows if r.get("present_ms") and float(r["present_ms"])>0]
    if low is not None: pres=[p for p in pres if low<=p<=high]
    dt=[b-a for a,b in zip(pres,pres[1:]) if b>a]
    if not dt:return {}
    ss=sorted(dt)
    tail=ss[-max(1,len(ss)//100):]
    return dict(frames=len(pres),fps=1000/statistics.mean(dt),low1=1000/statistics.mean(tail),
      p95=ss[min(len(ss)-1,math.ceil(.95*len(ss))-1)],p99=ss[min(len(ss)-1,math.ceil(.99*len(ss))-1)],
      max_ms=max(dt),ge50=sum(x>=50 for x in dt),ge100=sum(x>=100 for x in dt),ge1000=sum(x>=1000 for x in dt))
def first(pattern,text):
    m=re.search(pattern,text)
    return float(m[1]) if m else None
out=[]
for folder in sorted((base/"runs").glob("ab1-*")):
    meta=json.loads((folder/"run.json").read_text(encoding="utf-8-sig"))
    if meta.get("exit_code")!=0 or meta.get("forced") or meta.get("elapsed_s",0)<90:continue
    f=folder/"logs"/"gpu_frames.csv"
    if not f.exists():continue
    rows=list(csv.DictReader(f.open()))
    log=(folder/"logs"/"SecondSonPC.log").read_text(encoding="utf-8",errors="replace")
    native=(folder/"logs"/"gpu_native.txt").read_text(encoding="utf-8",errors="replace")
    def peak(pattern):
        vals=[float(x) for x in re.findall(pattern,log)]
        return max(vals) if vals else None
    steady=[r for r in rows if 62000<=float(r["present_ms"] or 0)<=88000]
    sums={k:sum(float(r.get(k) or 0) for r in steady)/len(steady) if steady else None
      for k in ["cp_ms","record_ms","gpu_busy_ms","shader_ms","pipeline_ms"]}
    starts=[re.match(r"\[(\d\d:\d\d:\d\d\.\d+)\]",l) for l in log.splitlines() if "native backend started on" in l]
    origin=datetime.fromisoformat(meta["start_utc"])
    startup=None
    if starts and starts[0]:
        t=datetime.strptime(starts[0][1],"%H:%M:%S.%f")
        actual=(origin+timedelta(hours=3)).replace(hour=t.hour,minute=t.minute,second=t.second,microsecond=t.microsecond)-timedelta(hours=3)
        pres=[float(r["present_ms"]) for r in rows if float(r.get("present_ms") or 0)>0]
        startup=(actual-origin).total_seconds()+pres[0]/1000 if pres else None
    out.append(dict(run=folder.name,scene=meta["scene"],variant=meta["label"].removeprefix("ab1-"),repeat=meta["repeat"],
      cache_state="cold application cache" if meta["repeat"]==1 else "warm application cache",
      all=metric(rows),steady=metric(rows,62000,88000),stage_mean_ms=sums,startup_s=startup,
      shutdown_s=meta["shutdown_s"],private_mib=meta["private_max"]/2**20,working_mib=meta["working_max"]/2**20,
      vram_peak_mib=peak(r"VRAM sample: local usage (\d+) MiB"),system_commit_peak_mib=peak(r"system committed (\d+)"),
      shader_count=first(r"shader translations \(GCN -> SPIR-V\): (\d+)",native),
      shader_ms=first(r"shader translations \(GCN -> SPIR-V\): \d+, ([\d.]+) ms",native),
      pipeline_ms=first(r"pipeline creation \(driver\): ([\d.]+) ms",native),
      pipeline_count=first(r"pipelines on demand: created (\d+)",native),
      precompile_wall_ms=first(r"built \d+ \([\d.]+ ms of the workers' time, ([\d.]+) ms wall",native),
      app_errors=len(re.findall(r"\[Error\]",log)),validation="not enabled in timing runs",
      binary_sha256=meta["binary_sha256"],source_sha256=meta["source_manifest_sha256"]))
(base/"ab-results.json").write_text(json.dumps(out,ensure_ascii=False,indent=2),encoding="utf-8")
lines=["# P4.15A A/B results","",
"90 s per run; same savedata snapshots and scripted inputs. Whole run includes loading/compilation. Steady window: 62–88 s from backend frame origin. Repeat 1 uses an empty application cache; repeats 2–3 are warm. Each run uses a new driver-cache directory. These states are reported separately, not pooled. Validation is tested separately.",
"",
"|Scene|Build|Repeat|FPS all|FPS steady|1% low steady|p95 ms|p99 ms|>=50/100/1000 ms all|max ms all|",
"|---|---|---:|---:|---:|---:|---:|---:|---|---:|"]
for x in out:
    a=x["all"];s=x["steady"]
    lines.append(f'|{x["scene"]}|{x["variant"]}|{x["repeat"]}|{a.get("fps",0):.2f}|{s.get("fps",0):.2f}|{s.get("low1",0):.2f}|{s.get("p95",0):.2f}|{s.get("p99",0):.2f}|{a.get("ge50")}/{a.get("ge100")}/{a.get("ge1000")}|{a.get("max_ms",0):.1f}|')
lines+=["","Full CPU/GPU, shader/pipeline, RAM/VRAM, startup/shutdown and binary identities: ab-results.json.",
"CPU stage and GPU busy times overlap; do not add them together. No FPS conclusion is drawn from an incomplete pair or one cold run.",""]
for scene in ["beach","early","user1022"]:
    vals={}
    for v in ["baseline","candidate1"]:
        group=[x["steady"].get("fps") for x in out if x["scene"]==scene and x["variant"]==v and x["repeat"]>=2]
        if group:vals[v]=group
    if all(len(vals.get(v,[]))>=2 for v in ["baseline","candidate1"]):
        b=vals["baseline"];c=vals["candidate1"]
        lines.append(f"{scene}: warm baseline {min(b):.2f}–{max(b):.2f} FPS; candidate {min(c):.2f}–{max(c):.2f} FPS; mean delta {(statistics.mean(c)/statistics.mean(b)-1)*100:+.2f}%. Two warm repetitions are a limited sample.")
(base/"ab-results.md").write_text("\n".join(lines)+"\n",encoding="utf-8")
print(f"Completed runs: {len(out)}")
for x in out:print(x["scene"],x["variant"],x["repeat"],round(x["steady"].get("fps",0),2))

