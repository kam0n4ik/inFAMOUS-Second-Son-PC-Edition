"""P5.3.1: the command processor's cost by category, from one profiling run per scene.

    py -I -X utf8 tools\\tests\\p531_cp_profile.py <series dir> [<series dir> ...]

A run of tools\\tests\\p46_runs.ps1 with
    SSPC_GPU_TIMINGS=sample:16;SSPC_DIAG_SAMPLE=1;SSPC_DIAG_SAMPLE_GAME=GpuRecord;SSPC_DIAG_SAMPLE_AFTER=60
leaves per scene: gpu_cpu_profile.txt (thread 1 "GpuNative": the command processor and, in the parallel processor, the
shader frontend; 1 ms samples with unwinding), gpu_cpu_profile_game.txt (thread 2 "GpuRecord": the Vulkan recording;
leaf only), gpu_frames.csv and stats.json. Thread 1 is sampled with call stacks: its categories are inclusive shares
of disjoint subtrees; thread 2 only by leaf: its categories are sums of exclusive shares. Milliseconds per frame =
share x the mean frame time of the window (both threads run for the whole frame). Reads only.
"""
import json
import os
import re
import sys


def profile(path):
    """{function: (exclusive %, inclusive %)} from the sampler's 'exclusive' section."""
    out = {}
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    on = False
    for line in lines:
        if line.startswith("exclusive"):
            on = True
            continue
        if on:
            m = re.match(r"\s+([\d.]+)%\s+([\d.]+)% incl\s+(.*)$", line)
            if not m:
                if line and not line.startswith(" "):
                    break
                continue
            out[m.group(3).strip()] = (float(m.group(1)), float(m.group(2)))
    return out


def incl(p, *names):
    return sum(p.get(n, (0.0, 0.0))[1] for n in names)


def excl(p, pred):
    return sum(v[0] for k, v in p.items() if pred(k))


NS = "SecondSon::Gpu::"
COPY = NS + "Native::`anonymous namespace'::CopyStage"
LOOP = NS + "Native::`anonymous namespace'::Backend::Loop"
# Thread 1: disjoint inclusive subtrees (the parallel processor's frontend runs under CommandProcessor::Draw/Dispatch).
T1 = [
    ("5. resources: shader frontend (program/fetch lookup, SRT walk, descriptor decode, key, pipeline id)",
     [NS + "Shader::ShaderFrontend::PrepareDraw", NS + "Shader::ShaderFrontend::PrepareDispatch"]),
    ("5. resources: copying the preparation into the record (CopyStage)", [COPY]),
    ("5. resources: GPU-write notes for the frontend's reader",
     [NS + "Native::ParallelCp::NoteStageWrites"]),
    ("8. sync: thread 1 waiting (records' queue head, timeline)",
     [NS + "Native::ParallelCp::WaitHead", NS + "Vk::Timeline::Wait"]),
    ("9. presentation on thread 1", ["DrvPresentBuffers"]),
]
T1_SUB = [n for _, ns in T1 for n in ns if n != "DrvPresentBuffers"]
CP_PARTS = [NS + "Cp::RegisterFile::Write", NS + "Cp::CommandProcessor::SetRegs", NS + "Pm4::Decode",
            NS + "Cp::CommandProcessor::Next", NS + "Native::ParallelCp::OnWrite",
            NS + "Native::ParallelCp::JournalReserve", NS + "Cp::CommandProcessor::BuildDraw",
            NS + "Native::ParallelCp::Dma", NS + "Native::ParallelCp::PushRecord",
            NS + "Cp::CommandProcessor::UpdateReadPointer", "SecondSon::Engine::Render::IsMarker"]
FRONT_PARTS = [NS + "Shader::ResolveResources", NS + "Shader::SrtProgram::Run",
               NS + "Native::ParallelCp::FrontReader::Read", NS + "Shader::DecodeResources",
               NS + "Shader::ProgramCache::Get", NS + "Shader::FetchCache::GetEntry",
               NS + "Shader::BuildSpecializationKey", NS + "Shader::Program::FindModule",
               NS + "Shader::PipelineStateCache::GetGraphics", NS + "Shader::BuildGraphicsRuntime"]

# Thread 2: leaf (exclusive) shares by function name.
T2 = [
    ("7. Vulkan driver (vkCmd*, the driver part of push descriptors; no symbols: vkGetInstanceProcAddr)",
     lambda k: k in ("vkGetInstanceProcAddr", "vk_icdNegotiateLoaderICDInterfaceVersion",
                     "vkEnumerateInstanceLayerProperties", "[nvoglv64.dll]")),
    ("8. sync: thread 2 idle (spin in Loop, blocked)",
     lambda k: k in (NS + "Native::ParallelCp::Loop", "NtWaitForAlertByThreadId", "NtWaitForMultipleObjects")),
    ("5. resources: buffers (GuestRange, buffer cache, page tracker, stream ring)",
     lambda k: any(s in k for s in ("GuestRange", "BufferCache::", "PageTracker::", "GuestBuffers::", "StreamRing::",
                                    "NoteAccesses", "PageMask"))),
    ("5. resources: images/samplers (image cache, texture memo)",
     lambda k: any(s in k for s in ("ImageCache::", "TexMemo", "BuildSamplerParams"))),
    ("4. descriptors (BindStage, PushDescriptors, layout signature)",
     lambda k: any(s in k for s in ("BindStage", "PushDescriptors", "LayoutSignature"))),
    ("6. barriers (range checks)", lambda k: "NeedsBarrier" in k),
    ("3. pipelines (factory lookup)", lambda k: "PipelineFactory" in k),
    ("7. recording (RecordDraw/RecordDispatch, dynamic state)",
     lambda k: any(s in k for s in ("RecordDraw", "RecordDispatch", "SetDynamicState", "Executor::Dma",
                                    "Vk::`anonymous namespace'::F"))),
    ("1+2. register replica (journal)", lambda k: "ApplyJournal" in k or "RegisterFile::" in k),
    ("9. submission", lambda k: any(s in k for s in ("SubmitCommand", "DrvPresentBuffers"))),
]


def main():
    for series in sys.argv[1:]:
        for scene in sorted(os.listdir(series)):
            d = os.path.join(series, scene)
            if not os.path.isdir(d) or not os.path.exists(os.path.join(d, "stats.json")):
                continue
            st = json.load(open(os.path.join(d, "stats.json"), encoding="utf-8"))
            frame_ms = st["ft_mean_ms"]
            p1 = profile(os.path.join(d, "gpu_cpu_profile.txt"))
            p2 = profile(os.path.join(d, "gpu_cpu_profile_game.txt"))
            print(f"=== {os.path.basename(series)}/{scene}: avg {st['avg_fps']:.1f} FPS, 1% low {st['low1_fps']:.1f}, "
                  f"p95 {st['ft_p95_ms']:.1f} ms, p99 {st['ft_p99_ms']:.1f} ms, frame {frame_ms:.2f} ms; per frame: "
                  f"draws {st['mean_draws']:.0f}, dispatches {st['mean_dispatches']:.0f}, thread 1 busy {st['mean_cp_ms']:.2f} "
                  f"ms, thread 2 busy {st['mean_par_busy_ms']:.2f} / idle {st['mean_par_idle_ms']:.2f}, thread 1 waits "
                  f"{st['mean_par_wait_ms']:.2f} ({st['mean_par_drains']:.0f} drains), GPU {st['mean_gpu_busy_ms']:.2f}")
            if not p1:
                print("  (no thread 1 profile)")
                continue
            print("  thread 1 (GpuNative: command processor + shader frontend), inclusive")
            for name, fns in T1:
                share = incl(p1, *fns)
                print(f"    {share:5.1f} %  {share * frame_ms / 100:5.2f} ms  {name}")
            pm4 = max(incl(p1, NS + "Cp::CommandProcessor::Step") - incl(p1, *T1_SUB), 0.0)
            print(f"    {pm4:5.1f} %  {pm4 * frame_ms / 100:5.2f} ms  1+2. PM4 read/decode, registers, journal, state "
                  f"(CommandProcessor::Step without the subtrees above)")
            print("           of it: " + ", ".join(f"{n.split('::')[-1]} {p1.get(n, (0, 0))[1]:.1f}%" for n in CP_PARTS))
            print("           frontend: " + ", ".join(f"{n.split('::')[-1]} {p1.get(n, (0, 0))[1]:.1f}%" for n in FRONT_PARTS))
            print(f"           (Backend::Loop {incl(p1, LOOP):.1f} % of the samples)")
            if not p2:
                print("  (no thread 2 profile)")
                continue
            print("  thread 2 (GpuRecord: Vulkan recording), leaf samples")
            used = 0.0
            for name, pred in T2:
                share = excl(p2, pred)
                used += share
                print(f"    {share:5.1f} %  {share * frame_ms / 100:5.2f} ms  {name}")
            rest = max(100.0 - used, 0.0)
            print(f"    {rest:5.1f} %  {rest * frame_ms / 100:5.2f} ms  10. other")
            print()


if __name__ == "__main__":
    main()
