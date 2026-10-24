# SecondSonPC R10 sched series (docs/R10_CPU_GPU_SCHEDULER.md): tools\r9\series.ps1 for the R10 worktree - the
# configurations of one exe in ABBA rounds, each run a tools\r10\sched_run.ps1 (the play.cmd preset + the
# configuration's switches) of the same scene and route; the background CPU checked before (gate) and while measured
# (over $Dirty %: renamed <tag>_dirty<k> and run again, at most $Tries times). Summary: tools\r9\ab_summary.py (+ the
# latency of the runs with SSPC_R10_LATENCY=1, latency.json).
#   powershell -File tools\r10\sched_series.ps1 -Name <series> -Scene city -Route fight -Rounds 4
#       -Configs "A|#B|SSPC_R10_SCHED=1;SSPC_PIPE_PREFETCH=1"   (name|env;env ... configurations separated by #)
param([Parameter(Mandatory)] [string]$Name, [Parameter(Mandatory)] [string]$Configs, [string]$Route = "traverse",
      [ValidateSet("city", "user", "neon", "beach", "harbor", "village")] [string]$Scene = "city", [int]$Rounds = 4,
      [double]$Gate = 8, [double]$Dirty = 10, [int]$Tries = 3, [int]$GateMinutes = 15,
      [string]$Worktree = "E:\SecondSonPC-r10-sched", [string]$Out = "E:\research\r10\runs\sched")
$ErrorActionPreference = "Continue"
$list = @($Configs.Split('#', [StringSplitOptions]::RemoveEmptyEntries) | ForEach-Object {
    $n, $e = $_.Split('|', 2); [pscustomobject]@{ Name = $n.Trim(); Env = "$e".Trim() } })
$dir = Join-Path $Out $Name
New-Item -ItemType Directory -Force $dir | Out-Null
"series $Name, scene $Scene, route $Route, rounds $Rounds, gate $Gate%, dirty over $Dirty% ($Tries tries), exe $Worktree, started $(Get-Date -Format 's')" |
    Add-Content "$dir\series.txt"
$list | Format-Table -AutoSize | Out-String -Width 400 | Add-Content "$dir\series.txt"

function Measure-Background {
    $c = New-Object System.Diagnostics.PerformanceCounter("Processor", "% Processor Time", "_Total")
    [void]$c.NextValue(); $v = @()
    for ($i = 0; $i -lt 10; $i++) { Start-Sleep -Seconds 1; $v += $c.NextValue() }
    return [math]::Round(($v | Measure-Object -Average).Average, 1)
}

for ($r = 1; $r -le $Rounds; $r++) {
    $order = @($list)
    if ($r % 2 -eq 0) { $order = @($list)[($list.Count - 1)..0] }
    foreach ($c in $order) {
        $tag = "$($c.Name)_$r"
        if (Test-Path "$dir\$tag\stats.json") { continue } # (a series run again goes on where it stopped)
        if (Test-Path "$dir\$tag") { Rename-Item "$dir\$tag" "$($tag)_aborted$(Get-Date -Format 'HHmmss')" }
        for ($k = 1; $k -le $Tries; $k++) {
            $bg = Measure-Background
            $deadline = (Get-Date).AddMinutes($GateMinutes)
            while ($bg -ge $Gate -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 20; $bg = Measure-Background }
            "$(Get-Date -Format 'HH:mm:ss') round $r $tag" | Add-Content "$dir\series.txt"
            "  gate: background $bg%$(if ($bg -ge $Gate) { " (over the gate after $GateMinutes min, run anyway)" })" |
                Add-Content "$dir\series.txt"
            & "$PSScriptRoot\sched_run.ps1" -Tag $tag -Scene $Scene -Route $Route -Env $c.Env -Worktree $Worktree -Out $dir |
                Select-Object -Last 1
            $during = [double](& python -I "E:\research\deep_perf\tools\bgof.py" "$dir\$tag")
            if ($during -le $Dirty -and $during -ge 0) {
                "  measured with background $during%" | Add-Content "$dir\series.txt"
                break
            }
            Rename-Item "$dir\$tag" "$($tag)_dirty$k"
            if ($k -eq $Tries) {
                "  background $during% while measured - renamed $($tag)_dirty$k, the last try: left out" | Add-Content "$dir\series.txt"
                break
            }
            "  background $during% while measured - renamed $($tag)_dirty$k, again" | Add-Content "$dir\series.txt"
        }
    }
}
"$(Get-Date -Format 'HH:mm:ss') done" | Add-Content "$dir\series.txt"
$summary = & python -I -X utf8 "$Worktree\tools\r9\ab_summary.py" $dir
$lat = Get-ChildItem $dir -Directory | Where-Object { Test-Path "$($_.FullName)\latency.json" } | ForEach-Object {
    $j = Get-Content "$($_.FullName)\latency.json" -Raw | ConvertFrom-Json
    "{0,-12} latency p50 {1:N1} ms, p95 {2:N1} ms, mean {3:N1} ms ({4}/{5} turns)" -f $_.Name, $j.summary.p50_ms,
        $j.summary.p95_ms, $j.summary.mean_ms, $j.summary.detected, $j.summary.turns }
$all = @($summary) + @("") + @($lat)
$all | Set-Content -Encoding UTF8 "$dir\summary.txt" # (Tee-Object of PowerShell 5.1 writes UTF-16)
$all
