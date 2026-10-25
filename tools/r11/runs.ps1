# R11 neon: diagnostic / trace runs one after another (not a measured series: no quiet flag), each with the R11 preset
# (play.cmd: r9run without -NoLauncher + PCACHE/HEAP_WINDOW/AJM/SYSRT) + the run's switches.
#   powershell -File tools\r11\runs.ps1 -Out E:\research\r11\runs\neon\<set> -Runs "tag|scene|route|K=V;K=V#..." [-MoreArgs ...]
param([Parameter(Mandatory)] [string]$Out, [Parameter(Mandatory)] [string]$Runs, [string]$Worktree = "E:\SecondSonPC-r10-pacing",
      [string]$Base = "SSPC_R9_PCACHE=1;SSPC_R9_HEAP_WINDOW=1;SSPC_R10_AJM=native;SSPC_R10_SYSRT=native",
      [string]$MoreArgs = "", [string]$Shots = "", [int]$Seconds = 0)
$ErrorActionPreference = "Continue"
New-Item -ItemType Directory -Force $Out | Out-Null
foreach ($r in $Runs.Split('#', [StringSplitOptions]::RemoveEmptyEntries)) {
    $tag, $scene, $route, $e = $r.Split('|', 4)
    $envs = "$Base;$e".Trim(';')
    "$(Get-Date -Format 'HH:mm:ss') $tag ($scene/$route): $envs" | Add-Content "$Out\runs.txt"
    $a = @{ Worktree = $Worktree; Tag = $tag; Route = $route; Scene = $scene; Env = $envs; Out = $Out }
    if ($MoreArgs) { $a.MoreArgs = $MoreArgs }
    if ($Shots) { $a.Shots = $Shots }
    if ($Seconds -gt 0) { $a.Seconds = $Seconds }
    & "$Worktree\tools\r9\r9run.ps1" @a | Select-Object -Last 1 | Add-Content "$Out\runs.txt"
    & python -I "$Worktree\tools\r10\bg.py" "$Out\$tag" 2>&1 | Add-Content "$Out\runs.txt"
}
"$(Get-Date -Format 'HH:mm:ss') done" | Add-Content "$Out\runs.txt"
