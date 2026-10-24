# SecondSonPC R10 sched stand (docs/R10_CPU_GPU_SCHEDULER.md): one run of a worktree's SecondSonPC.exe with the
# preset of game\play.cmd (variant A of the R10 A/B) + this run's switches, in a scene of the R9 stand
# (tools\r9\r9run.ps1: city/neon/user + a route) or of the deep-perf stand (E:\research\deep_perf\tools\run1.ps1:
# beach/harbor/village, their fixed camera route). After the run: r10_latency.csv (SSPC_R10_LATENCY=1) copied out and
# summarized by tools\r10\latency.py (latency.json), p412_trace.bin (SSPC_P412_STALL_TRACE) by tools\r10\timeline.py.
#   powershell -File tools\r10\sched_run.ps1 -Tag <tag> -Scene city|neon|user|beach|harbor|village [-Route fight|...]
#       [-Env "K=V;K=V"] [-Out E:\research\r10\runs\sched\<series>] [-Shots route|"40,50"] [-Json] [-Seconds 0]
param(
    [Parameter(Mandatory)] [string]$Tag,
    [ValidateSet("city", "user", "neon", "beach", "harbor", "village")] [string]$Scene = "city",
    [string]$Route = "traverse",
    [string]$Env = "",
    [string]$Worktree = "E:\SecondSonPC-r10-sched",
    [string]$Out = "E:\research\r10\runs\sched",
    [string]$Shots = "",
    [string]$MoreArgs = "",
    [string]$ShaderCache = "E:\SecondSonPC\cache\shaders",
    [int]$Seconds = 0,
    [switch]$Json,
    # "70:20": a kernel ETW trace of the context switches from 70 s for 20 s (tools\etw_capture.ps1 -NoStacks),
    # summarized by tools\r10\cores.py into <run>\etw\cores.txt (which logical CPUs run which threads; the trace and
    # its dump are removed after)
    [string]$Etw = ""
)
$ErrorActionPreference = "Stop"
# game\play.cmd (what the owner plays): r9run.ps1 sets the first 13 itself, run1.ps1 only the first 6
$play = "SSPC_P53_FRONTEND_THREAD=1;SSPC_CP_ASYNC_WRITES=1;SSPC_CP_ASYNC_FLIP=1;SSPC_VK_EMIT_THREAD=1;SSPC_VK_EMIT_BATCH=1;" +
        "SSPC_CP_RING=128;SSPC_NATIVE_GBUFFER=native;SSPC_R9_PCACHE=1;SSPC_R9_HEAP_WINDOW=1"
$envAll = "$play;$Env".Trim(';')
$routes = @{ # R10 routes of their own (tools\r9\routes.ps1 has the rest; a press spec is passed on as it is)
    # input-to-present latency: standing, then a sharp camera turn (one SDL motion event of 1500 px, no hold) every
    # 2.5 s, left and right in turn (window 50:118)
    latency = (@("space@12", "space@22", "mark:calm@45") + @(for ($i = 0; $i -lt 27; $i++) {
        "move:$(if ($i % 2) { -1500 } else { 1500 }):0@$(50 + 2.5 * $i)" }) + @("mark:end@118")) -join ","
    # fast movement without turns: W held, the dash (E) again every 2 s and jumps, straight ahead (window 55:118)
    flight = (@("space@12", "space@22", "mark:calm@45", "mark:flight@55", "w@55:63") + @(for ($t = 56; $t -lt 118; $t += 2) { "e@$($t):1.2" }) +
              @(for ($t = 57; $t -lt 118; $t += 3) { "space@$($t + 0.5)" }) + @("mark:end@118")) -join ","
}
$logs = "$Worktree\logs"
# (R10 rules: no run while another agent's measured series holds the quiet flag; sched's own series runs under it.
# Checked together with the stand's game lock, right before the run takes it - a run that passed the flag and then
# waited for the lock started inside another agent's series on 10-24)
function Test-Quiet {
    (Test-Path "E:\research\r10\quiet.flag") -and
        ((Get-Content "E:\research\r10\quiet.flag" -Raw -ErrorAction SilentlyContinue) -notlike "sched *")
}
$lockDir = "E:\research\deep_perf\game.lock"
while ((Test-Quiet) -or (Get-Process SecondSonPC -ErrorAction SilentlyContinue) -or
       ((Test-Path $lockDir) -and ((Get-Date) - (Get-Item $lockDir).LastWriteTime).TotalMinutes -lt 15)) {
    Start-Sleep -Seconds 5
}
Remove-Item "$logs\r10_latency.csv" -ErrorAction SilentlyContinue
$etwJob = $null
if ($Etw) {
    $ed, $edur = $Etw.Split(':')
    $etwDir = Join-Path (Join-Path $Out $Tag) "etw"
    New-Item -ItemType Directory -Force $etwDir | Out-Null
    $etwJob = Start-Job -ArgumentList "$Worktree\tools\etw_capture.ps1", $etwDir, ([int]$ed), ([int]$edur),
                        "\$Tag\save" -ScriptBlock {
        param($script, $dir, $delay, $dur, $mark)
        # (this run's own game - its command line names this run's save copy: another run, even of the same exe,
        # is not traced; up to 1 h)
        $deadline = (Get-Date).AddHours(1)
        while ((Get-Date) -lt $deadline -and -not (Get-CimInstance Win32_Process -Filter "Name='SecondSonPC.exe'" |
                                                   Where-Object { $_.CommandLine -and $_.CommandLine.Contains($mark) })) {
            Start-Sleep -Milliseconds 500
        }
        & powershell -ExecutionPolicy Bypass -File $script -Out $dir -Delay $delay -Duration $dur -NoStacks -Profile 100 -WaitSeconds 240
    }
}
if ($Scene -in "beach", "harbor", "village") {
    & "E:\research\deep_perf\tools\run1.ps1" -Tag $Tag -Scene $Scene -Worktree $Worktree -Out $Out -Env $envAll `
        -Seconds $Seconds -MoreArgs $MoreArgs -ShaderCache $ShaderCache | Select-Object -Last 1
} else {
    $r = $Route
    $secs = $Seconds
    if ($routes.ContainsKey($Route)) { $r = $routes[$Route]; if ($secs -le 0) { $secs = 125 } }
    & "$Worktree\tools\r9\r9run.ps1" -Tag $Tag -Route $r -Scene $Scene -Env $envAll -Worktree $Worktree -Out $Out `
        -Seconds $secs -Shots $Shots -MoreArgs $MoreArgs -ShaderCache $ShaderCache | Select-Object -Last 1
    if ($routes.ContainsKey($Route) -and (Test-Path "$Out\$Tag\gpu_frames.csv")) { # (the window of the R10 route)
        $w = if ($Route -eq "latency") { 50, 118 } else { 55, 118 }
        & python -I "$Worktree\tools\frame_stats.py" "$Out\$Tag\gpu_frames.csv" --from $w[0] --to $w[1] --json "$Out\$Tag\stats.json" | Out-Null
    }
}
$run = Join-Path $Out $Tag
if ($etwJob) {
    Receive-Job $etwJob -Wait -AutoRemoveJob | Set-Content -Encoding UTF8 "$etwDir\capture.txt"
    if (Test-Path "$etwDir\trace.etl") {
        & "C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit\xperf.exe" -i "$etwDir\trace.etl" -o "$etwDir\dump.txt" -a dumper | Out-Null
        & python -I -X utf8 "$Worktree\tools\r10\cores.py" $etwDir --log "$run\SecondSonPC.log" --trace "$run\p412_trace.bin" |
            Select-Object -First 3
        Remove-Item "$etwDir\trace.etl", "$etwDir\dump.txt" -ErrorAction SilentlyContinue
    }
}
if (Test-Path "$logs\r10_latency.csv") {
    Copy-Item "$logs\r10_latency.csv" $run
    & python -I -X utf8 "$Worktree\tools\r10\latency.py" $run --json "$run\latency.json" | Select-Object -Last 12 |
        Set-Content -Encoding UTF8 "$run\latency.txt"
}
if (Test-Path "$run\p412_trace.bin") {
    $a = @("$run")
    if (-not $Json) { $a += "--no-json" }
    & python -I -X utf8 "$Worktree\tools\r10\timeline.py" @a 2>&1 | Select-Object -Last 3
}
"done -> $run"
