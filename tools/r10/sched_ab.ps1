# SecondSonPC R10 sched A/B (docs/R10_CPU_GPU_SCHEDULER.md §5): the measured series of the sched branch under the
# stand's quiet flag (E:\research\r10\tools\stand_quiet.ps1 -Owner sched), one after another; a series already complete
# is skipped (sched_series.ps1 goes on where it stopped). The flag is held at most -MaxMinutes (R10 rules: 2 h) - no new
# series starts after that; it is released at the end.
#   powershell -File tools\r10\sched_ab.ps1 [-Only fight,traverse] [-MaxMinutes 115]
param([string[]]$Only = @(), [int]$MaxMinutes = 115)
$ErrorActionPreference = "Continue"
$S = "$PSScriptRoot\sched_series.ps1"
$A = "A|"
$B = "B|SSPC_R10_SCHED=1;SSPC_PIPE_PREFETCH=1"
$Sp = "S|SSPC_R10_SPREAD=1"
$L = "SSPC_R10_LATENCY=1"
$series = @(
    @{ Name = "ab_fight"; Scene = "city"; Route = "fight"; Rounds = 3; Configs = "$A#$B#$Sp" },
    @{ Name = "ab_traverse"; Scene = "city"; Route = "traverse"; Rounds = 3; Configs = "$A#$B#$Sp" },
    @{ Name = "ab_neon"; Scene = "neon"; Route = "neon"; Rounds = 2; Configs = "$A#$B" },
    @{ Name = "ab_latency"; Scene = "city"; Route = "latency"; Rounds = 2;
       Configs = "A|$L#R0|$L;SSPC_REFLEX=0#S|$L;SSPC_R10_SPREAD=1" },
    @{ Name = "ab_beach"; Scene = "beach"; Route = "x"; Rounds = 1; Configs = "$A#$Sp" },
    @{ Name = "ab_harbor"; Scene = "harbor"; Route = "x"; Rounds = 1; Configs = "$A#$Sp" },
    @{ Name = "ab_village"; Scene = "village"; Route = "x"; Rounds = 1; Configs = "$A#$Sp" }
)
$q = "E:\research\r10\tools\stand_quiet.ps1"
$t0 = Get-Date
for (;;) { # (pacing, then cp have the stand first: wait for a free flag)
    $st = & powershell -ExecutionPolicy Bypass -File $q -Wait -Minutes 30
    if ("$st" -like "free*") {
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
        & powershell -ExecutionPolicy Bypass -File $S -Name $s.Name -Scene $s.Scene -Route $s.Route -Rounds $s.Rounds `
            -Configs $s.Configs | Select-Object -Last 12
    }
} finally {
    & powershell -ExecutionPolicy Bypass -File $q -Release -Owner sched
    "$(Get-Date -Format 'HH:mm:ss') quiet flag released (held $([int]((Get-Date) - $t1).TotalMinutes) min)"
}
