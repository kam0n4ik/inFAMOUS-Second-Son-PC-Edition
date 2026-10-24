# SecondSonPC R10 sched A/B (docs/R10_CPU_GPU_SCHEDULER.md §5): the measured series of the sched branch under the
# stand's quiet flag (E:\research\r10\tools\stand_quiet.ps1 -Owner sched), one after another; a series already complete
# is skipped (sched_series.ps1 goes on where it stopped). The flag is held at most -MaxMinutes (R10 rules: 2 h) - no new
# series starts after that; it is released at the end.
#   powershell -File tools\r10\sched_ab.ps1 [-Only fight,traverse] [-MaxMinutes 115]
param([string[]]$Only = @(), [int]$MaxMinutes = 115)
$ErrorActionPreference = "Continue"
$SeriesPs = "$PSScriptRoot\sched_series.ps1" # (not $S: PowerShell names ignore case - the loop variable $s)
$A = "A|"
$B = "B|SSPC_R10_SCHED=1;SSPC_PIPE_PREFETCH=1"
$Sp = "S|SSPC_R10_SPREAD=1"
$L = "SSPC_R10_LATENCY=1"
$series = @( # (in order of what matters: the SPREAD check in a fight, the world on neon, latency, then the rest)
    @{ Name = "ab_fight"; Scene = "city"; Route = "fight"; Rounds = 3; Configs = "$A#$B#$Sp" },
    @{ Name = "ab_neon"; Scene = "neon"; Route = "neon"; Rounds = 2; Configs = "$A#$B"; Shots = "route" },
    @{ Name = "ab_latency"; Scene = "city"; Route = "latency"; Rounds = 2;
       Configs = "A|$L#R0|$L;SSPC_REFLEX=0#S|$L;SSPC_R10_SPREAD=1" },
    @{ Name = "ab_traverse"; Scene = "city"; Route = "traverse"; Rounds = 2; Configs = "$A#$B#$Sp" },
    @{ Name = "x_validate"; Scene = "city"; Route = "fight"; Tag = "val_B"; Env = "SSPC_R10_SCHED=1;SSPC_PIPE_PREFETCH=1";
       MoreArgs = "--gpu-native-validate" },
    @{ Name = "x_cores"; Scene = "city"; Route = "fight"; Tag = "fight_S_etw2"; Etw = "90:20";
       Env = "SSPC_R10_AFFINITY_LOG=1;SSPC_R10_SPREAD=1" },
    @{ Name = "x_cores"; Scene = "city"; Route = "traverse"; Tag = "traverse_A_etw"; Etw = "90:20";
       Env = "SSPC_R10_AFFINITY_LOG=1" },
    @{ Name = "ab_beach"; Scene = "beach"; Route = "x"; Rounds = 1; Configs = "$A#$Sp" },
    @{ Name = "ab_harbor"; Scene = "harbor"; Route = "x"; Rounds = 1; Configs = "$A#$Sp" },
    @{ Name = "ab_village"; Scene = "village"; Route = "x"; Rounds = 1; Configs = "$A#$Sp" }
)
$q = "E:\research\r10\tools\stand_quiet.ps1"
$t0 = Get-Date
for (;;) { # (pacing, then cp have the stand first: wait for a free flag)
    if ((Test-Path "E:\research\r10\quiet.flag") -and ((Get-Content "E:\research\r10\quiet.flag" -Raw) -like "sched *")) {
        break # (sched's own flag, left by a stopped run of this script: go on under it)
    }
    $st = & powershell -ExecutionPolicy Bypass -File $q -Wait -Minutes 30
    if ("$st" -like "free*") {
        Start-Sleep -Seconds 150 # (cp's series has the stand before sched: it takes a freed flag within this)
        $st = & powershell -ExecutionPolicy Bypass -File $q -Wait -Minutes 0
        if ("$st" -notlike "free*") { continue }
        $tk = & powershell -ExecutionPolicy Bypass -File $q -Take -Owner sched -Note "sched A/B series" 2>&1
        if ("$tk" -like "taken*") { break }
    }
}
$t1 = Get-Date
"$(Get-Date -Format 'HH:mm:ss') quiet flag taken (waited $([int]($t1 - $t0).TotalMinutes) min)"
try {
    foreach ($s in $series) {
        if ($Only.Count -and $Only -notcontains $s.Name.Substring(3)) { continue }
        if (((Get-Date) - $t1).TotalMinutes -ge $MaxMinutes) { "$(Get-Date -Format 'HH:mm:ss') time up - $($s.Name) not started"; continue }
        "$(Get-Date -Format 'HH:mm:ss') series $($s.Name)"
        try { # (a run that throws ends its series, not the plan)
            if ($s.Tag) { # (one run, not a series: validation / the ETW per-core trace)
                $d = "E:\research\r10\runs\sched\$($s.Name)"
                if (Test-Path "$d\$($s.Tag)\stats.json") { continue }
                # (a script call, not powershell.exe: an empty or "--..." value is passed as it is)
                $extra = @{}
                if ($s.MoreArgs) { $extra.MoreArgs = $s.MoreArgs }
                if ($s.Etw) { $extra.Etw = $s.Etw }
                & "$PSScriptRoot\sched_run.ps1" -Tag $s.Tag -Scene $s.Scene -Route $s.Route -Env $s.Env -Out $d @extra |
                    Select-Object -Last 3
                continue
            }
            $extra = @{}
            if ($s.Shots) { $extra.Shots = $s.Shots }
            & $SeriesPs -Name $s.Name -Scene $s.Scene -Route $s.Route -Rounds $s.Rounds -Configs $s.Configs @extra |
                Select-Object -Last 12
        } catch { "$(Get-Date -Format 'HH:mm:ss') $($s.Name) failed: $_" }
    }
} finally {
    & powershell -ExecutionPolicy Bypass -File $q -Release -Owner sched
    "$(Get-Date -Format 'HH:mm:ss') quiet flag released (held $([int]((Get-Date) - $t1).TotalMinutes) min)"
}
