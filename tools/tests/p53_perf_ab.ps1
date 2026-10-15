# SecondSonPC P5.3D.3: interleaved A/B performance series of the native render path.
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\tests\p53_perf_ab.ps1 -Tag <name>
#       [-Scenes beach,early] [-Repeats 3] [-Seconds 90] [-Configs base,apply9,apply19,apply19s,par_base,par_apply19s]
# Every repeat runs every scene with every configuration, in turn (base, then the native variants, then again), so
# slow drift of the machine falls on all of them alike. Measurement runs: no Vulkan validation, the native path
# without its checks (SSPC_P53_NATIVE_VERIFY=0), no periodic native counters (SSPC_P53_NATIVE_DRAW_STATS=0). The
# process's CPU time and working set are sampled every second (cpu_mem.csv next to the run). Each run's frame window
# (62..88 s) is in stats.json (tools\frame_stats.py); tools\tests\p53_perf_summary.py sums the series up.
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$Scenes = "beach,early",
    [int]$Repeats = 3,
    [int]$Seconds = 90,
    [string]$Configs = "base,apply9,apply19,apply19s,par_base,par_apply19s",
    [string]$Exe = "" # a copy of SecondSonPC.exe for the whole series (the build may change while it runs)
)
$ErrorActionPreference = "Stop"
$root = "E:\SecondSonPC"
$quiet = "SSPC_P53_NATIVE_VERIFY=0;SSPC_P53_NATIVE_DRAW_STATS=0"
$defs = @{
    # (the single-thread processor named explicitly: the parallel one is the default since P5.3D)
    base         = "SSPC_P410_PARALLEL_CP=0"
    apply9       = "SSPC_P410_PARALLEL_CP=0;SSPC_P53_NATIVE_DRAW=apply:9;$quiet"
    apply19      = "SSPC_P410_PARALLEL_CP=0;SSPC_P53_NATIVE_DRAW=apply:1,9;$quiet"
    apply19s     = "SSPC_P410_PARALLEL_CP=0;SSPC_P53_NATIVE_DRAW=apply:1,9;SSPC_P53_NATIVE_STATE=apply;$quiet"
    par_base     = "SSPC_P410_PARALLEL_CP=1"
    par_apply19s = "SSPC_P410_PARALLEL_CP=1;SSPC_P53_NATIVE_DRAW=apply:1,9;SSPC_P53_NATIVE_STATE=apply;$quiet"
    # P5.3D: the shader frontend on its own thread (parallel_cp.h, Options::frontend_thread)
    par_ft       = "SSPC_P410_PARALLEL_CP=1;SSPC_P53_FRONTEND_THREAD=1"
    # P5.3.1-3 render modes (the parallel processor, the default): LEGACY / the first native pass (G-buffer, pass 1:
    # commit, draw, state, and the Native Render Frontend's cache) / that pass without the cache / HYBRID / NATIVE_EXP.
    legacy       = ""
    p1           = "SSPC_P53_NATIVE_DRAW=apply:1;SSPC_P53_NATIVE_STATE=apply;SSPC_P53_NATIVE_FRONTEND=1;$quiet"
    p1_nofe      = "SSPC_P53_NATIVE_DRAW=apply:1;SSPC_P53_NATIVE_STATE=apply;$quiet"
    hybrid       = "SSPC_RENDER_MODE=hybrid;SSPC_P53_NATIVE_DRAW_STATS=0"
    native       = "SSPC_RENDER_MODE=native_experimental;SSPC_P53_NATIVE_DRAW_STATS=0"
    # P5.3 marker transport A/B: HYBRID with its operations as PM4 markers / through the direct path (no markers:
    # native stream + descriptions in the draws' NOPs, SSPC_P53_DIRECT=1); both with the transport counters every 10 s
    hybrid_m     = "SSPC_RENDER_MODE=hybrid;SSPC_P53_NATIVE_DRAW_STATS=10"
    hybrid_d     = "SSPC_RENDER_MODE=hybrid;SSPC_P53_DIRECT=1;SSPC_P53_NATIVE_DRAW_STATS=10"
}
$out = "$root\logs\p53\$Tag"
New-Item -ItemType Directory -Force $out | Out-Null
"p53 perf series '$Tag': scenes $Scenes, repeats $Repeats, $Seconds s, configs $Configs, exe '$Exe'" | Out-File "$out\series.txt" -Encoding utf8
for ($rep = 1; $rep -le $Repeats; $rep++) {
    foreach ($scene in ($Scenes -split ',' | Where-Object { $_ })) {
        foreach ($cfg in ($Configs -split ',' | Where-Object { $_ })) {
            if (-not $defs.ContainsKey($cfg)) { throw "unknown config '$cfg'" }
            $runTag = "${cfg}_r$rep"
            if (Test-Path "$out\$runTag\${scene}_1\stats.json") {
                continue # done in an earlier (interrupted) invocation of the same series: resumed, not repeated
            }
            # CPU time and working set of the game's process, once a second, while the run lasts
            $sampler = Start-Job -ArgumentList "$out\$runTag", ($Seconds + 40) -ScriptBlock {
                param($dir, $limit)
                New-Item -ItemType Directory -Force $dir | Out-Null
                $rows = @("t_s,pid,cpu_s,working_set_mib,private_mib")
                $t0 = Get-Date
                while (((Get-Date) - $t0).TotalSeconds -lt $limit) {
                    # (the game's process: the SecondSonPC with the most private memory - not a helper of the same name)
                    $p = Get-Process SecondSonPC -ErrorAction SilentlyContinue | Sort-Object PrivateMemorySize64 -Descending |
                        Select-Object -First 1
                    if ($p) {
                        $rows += [string]::Format([Globalization.CultureInfo]::InvariantCulture, "{0:F1},{1},{2:F2},{3:F0},{4:F0}",
                            ((Get-Date) - $t0).TotalSeconds, $p.Id, $p.TotalProcessorTime.TotalSeconds,
                            ($p.WorkingSet64 / 1MB), ($p.PrivateMemorySize64 / 1MB))
                    }
                    Start-Sleep -Seconds 1
                }
                $rows | Out-File "$dir\cpu_mem_raw.csv" -Encoding ascii
            }
            $runArgs = @("-ExecutionPolicy", "Bypass", "-File", "$root\tools\tests\p46_runs.ps1", "-Tag", $runTag,
                "-Scenes", $scene, "-Repeats", "1", "-Seconds", "$Seconds", "-OutRoot", $out)
            if ($defs[$cfg]) { $runArgs += @("-Env", $defs[$cfg]) } # (base: no variable at all)
            if ($Exe) { $runArgs += @("-Exe", $Exe) }
            powershell @runArgs | Out-Null
            if (-not (Test-Path "$out\$runTag\${scene}_1\stats.json")) {
                "$(Get-Date -Format s) $runTag $scene FAILED: no stats.json" | Out-File "$out\series.txt" -Encoding utf8 -Append
            }
            Wait-Job $sampler -Timeout 60 | Out-Null
            Stop-Job $sampler -ErrorAction SilentlyContinue
            Remove-Job $sampler -Force -ErrorAction SilentlyContinue
            if (Test-Path "$out\$runTag\cpu_mem_raw.csv") {
                Move-Item "$out\$runTag\cpu_mem_raw.csv" "$out\$runTag\${scene}_1\cpu_mem.csv" -Force
            }
            "$(Get-Date -Format s) $runTag $scene done" | Out-File "$out\series.txt" -Encoding utf8 -Append
        }
    }
}
py -I -X utf8 "$root\tools\tests\p53_perf_summary.py" $out
