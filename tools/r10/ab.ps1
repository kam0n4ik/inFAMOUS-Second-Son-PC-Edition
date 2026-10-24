# R10 pacing A/B series (docs/R10_PACING.md): configurations of one exe over several scenes, rounds in ABBA order per
# scene, each run with the play.cmd preset + the configuration's switches. Scenes:
#   user  - the owner's save, route look (tools\r9\r9run.ps1)      fight  - the city stand, route fight (r9run)
#   city  - the city stand, route traverse (r9run)                 neon   - the Neon save, route neon (r9run)
#   beach | harbor | village - E:\research\deep_perf\tools\run1.ps1 (standing after camera turns, window 62-88 s)
# Before each run: no build of another agent running (E:\research\r10\build.lock) and < $Gate % CPU for 10 s; after it
# the background during the run (tools\r10\bg.py-style, 40-100 s) over $Dirty % renames it <tag>_dirty<k> and runs it
# again (at most $Tries). A run whose stats.json exists is not run again (a series continues where it stopped).
#   powershell -File tools\r10\ab.ps1 -Name ab1 -Scenes "user:4,fight:4,city:2" -Configs "off|#rea3|SSPC_REA_DEFER_WAIT_B=1#..."
param([Parameter(Mandatory)] [string]$Name, [Parameter(Mandatory)] [string]$Configs, [Parameter(Mandatory)] [string]$Scenes,
      [string]$Worktree = "E:\SecondSonPC-r10-pacing", [string]$Out = "E:\research\r10\runs\pacing",
      [double]$Gate = 8, [double]$Dirty = 10, [int]$Tries = 3,
      [string]$Base = "SSPC_R9_PCACHE=1;SSPC_R9_HEAP_WINDOW=1", [datetime]$Until = [datetime]::MaxValue)
$ErrorActionPreference = "Continue"
$launcher = "SSPC_P53_FRONTEND_THREAD=1;SSPC_CP_ASYNC_WRITES=1;SSPC_CP_ASYNC_FLIP=1;SSPC_VK_EMIT_THREAD=1;SSPC_VK_EMIT_BATCH=1;" +
            "SSPC_CP_RING=128;SSPC_NATIVE_GBUFFER=native"
$list = @($Configs.Split('#', [StringSplitOptions]::RemoveEmptyEntries) | ForEach-Object {
    $n, $e = $_.Split('|', 2); [pscustomobject]@{ Name = $n.Trim(); Env = "$e".Trim() } })
$dir = Join-Path $Out $Name
New-Item -ItemType Directory -Force $dir | Out-Null
"series $Name, scenes $Scenes, started $(Get-Date -Format 's')" | Add-Content "$dir\series.txt"
$list | Format-Table -AutoSize | Out-String -Width 400 | Add-Content "$dir\series.txt"

function Wait-Stand {
    $lock = "E:\research\r10\build.lock"
    while ((Test-Path $lock) -and ((Get-Date) - (Get-Item $lock).LastWriteTime).TotalMinutes -lt 30) { Start-Sleep -Seconds 15 }
    $c = New-Object System.Diagnostics.PerformanceCounter("Processor", "% Processor Time", "_Total")
    for ($k = 0; $k -lt 90; $k++) {
        [void]$c.NextValue(); $v = @()
        for ($i = 0; $i -lt 10; $i++) { Start-Sleep -Seconds 1; $v += $c.NextValue() }
        $avg = ($v | Measure-Object -Average).Average
        if ($avg -lt $Gate) { return [math]::Round($avg, 1) }
    }
    return [math]::Round($avg, 1)
}
function Get-Background([string]$run, [double]$from, [double]$to) {
    $f = "$run\cpu_bg.csv"
    if (-not (Test-Path $f)) { return -1 }
    $rows = @(Import-Csv $f | Where-Object { $_.game_cpu_s -and $_.total })
    $vals = @()
    for ($i = 1; $i -lt $rows.Count; $i++) {
        $t = [double]$rows[$i].t
        if ($t -lt $from -or $t -gt $to) { continue }
        $num = { param($x) [double]::Parse($x.Replace(',', '.'), [Globalization.CultureInfo]::InvariantCulture) }
        $game = ((& $num $rows[$i].game_cpu_s) - (& $num $rows[$i - 1].game_cpu_s)) * 100.0 / 12.0
        $vals += [math]::Max(0.0, (& $num $rows[$i].total) - $game)
    }
    if ($vals.Count -lt 10) { return 100 } # (the game's CPU not sampled: the machine was overloaded)
    return [math]::Round(($vals | Measure-Object -Average).Average, 1)
}

foreach ($sc in $Scenes.Split(',', [StringSplitOptions]::RemoveEmptyEntries)) {
    $scene, $rounds = $sc.Split(':'); $rounds = [int]$rounds
    for ($r = 1; $r -le $rounds; $r++) {
        $order = @($list)
        if ($r % 2 -eq 0) { $order = @($list)[($list.Count - 1)..0] }
        foreach ($c in $order) {
            $tag = "$($scene)_$($c.Name)_$r"
            if (Test-Path "$dir\$tag\stats.json") { continue }
            if (Test-Path "$dir\$tag") { Rename-Item "$dir\$tag" "$($tag)_aborted$(Get-Date -Format 'HHmmss')" }
            for ($k = 1; $k -le $Tries; $k++) {
                if ((Get-Date) -gt $Until) { "$(Get-Date -Format 'HH:mm:ss') past -Until, stopped before $tag" | Add-Content "$dir\series.txt"; return }
                $g = Wait-Stand
                "$(Get-Date -Format 'HH:mm:ss') $tag (gate $g%)" | Add-Content "$dir\series.txt"
                $envs = "$Base;$($c.Env)".Trim(';')
                switch ($scene) {
                    "user"  { & "$Worktree\tools\r9\r9run.ps1" -Worktree $Worktree -Tag $tag -Route look -Scene user -Env $envs -Out $dir | Out-Null; $w = 40, 100 }
                    "fight" { & "$Worktree\tools\r9\r9run.ps1" -Worktree $Worktree -Tag $tag -Route fight -Scene city -Env $envs -Out $dir | Out-Null; $w = 45, 140 }
                    "city"  { & "$Worktree\tools\r9\r9run.ps1" -Worktree $Worktree -Tag $tag -Route traverse -Scene city -Env $envs -Out $dir | Out-Null; $w = 45, 140 }
                    "neon"  { & "$Worktree\tools\r9\r9run.ps1" -Worktree $Worktree -Tag $tag -Route neon -Scene neon -Env $envs -Out $dir | Out-Null; $w = 42, 133 }
                    default { & "E:\research\deep_perf\tools\run1.ps1" -Worktree $Worktree -Tag $tag -Scene $scene -Env "$launcher;$envs" -Out $dir | Out-Null; $w = 62, 88 }
                }
                $bg = Get-Background "$dir\$tag" $w[0] $w[1]
                if ($bg -le $Dirty -and $bg -ge 0) { "  measured with background $bg%" | Add-Content "$dir\series.txt"; break }
                Rename-Item "$dir\$tag" "$($tag)_dirty$k"
                "  background $bg% - renamed $($tag)_dirty$k$(if ($k -lt $Tries) { ', again' })" | Add-Content "$dir\series.txt"
            }
        }
    }
}
"$(Get-Date -Format 'HH:mm:ss') done" | Add-Content "$dir\series.txt"
