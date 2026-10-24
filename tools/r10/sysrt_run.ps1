# R10 sysrt: one stand run (tools\r9\r9run.ps1: its lock, save copy, preset) with SSPC_R10_SYSRT=<Mode>, plus the
# import counter's logs\calls.json and call_order.txt, which r9run does not copy (docs/R10_SYSRT.md).
#   powershell -File tools\r10\sysrt_run.ps1 -Tag <tag> -Mode native|bridge [-Scene city|user] [-Route calm|<press spec>]
#       [-Seconds N] [-Shots "s,s,..."] [-KeepSave] [-ExtraEnv "K=V;..."] [-Series E:\research\r10\runs\sysrt\<series>]
param(
    [Parameter(Mandatory)] [string]$Tag,
    [Parameter(Mandatory)] [ValidateSet("native", "bridge")] [string]$Mode,
    [ValidateSet("city", "user")] [string]$Scene = "city",
    [string]$Route = "calm",
    [int]$Seconds = 0,
    [string]$Shots = "",
    [switch]$KeepSave,
    [string]$ExtraEnv = "",
    [string]$Series = "E:\research\r10\runs\sysrt\s1",
    [string]$Worktree = ""
)
$ErrorActionPreference = "Stop"
if (-not $Worktree) { $Worktree = Split-Path (Split-Path (Split-Path $MyInvocation.MyCommand.Path)) } # (tools\r10\..)
$logs = "$Worktree\logs"
# (R10_RULES: no game while another agent's measured series holds the quiet flag; r9run itself waits for the game lock)
while (Test-Path "E:\research\r10\quiet.flag") { Start-Sleep -Seconds 20 }
# (a stale calls.json of an earlier run must not pass for this one's when the game dies before writing it)
foreach ($f in "calls.json", "call_order.txt") { Remove-Item "$logs\$f" -ErrorAction SilentlyContinue }
$envs = "SSPC_R10_SYSRT=$Mode"
if ($ExtraEnv) { $envs += ";$ExtraEnv" }
$a = @{ Tag = $Tag; Route = $Route; Scene = $Scene; Env = $envs; Out = $Series; Worktree = $Worktree }
if ($Seconds -gt 0) { $a.Seconds = $Seconds }
if ($Shots) { $a.Shots = $Shots }
if ($KeepSave) { $a.KeepSave = $true }
& "$Worktree\tools\r9\r9run.ps1" @a
$run = Join-Path $Series $Tag
foreach ($f in "calls.json", "call_order.txt") { if (Test-Path "$logs\$f") { Copy-Item "$logs\$f" $run } }
if (Test-Path "$run\calls.json") { "calls.json copied" } else { "WARNING: no calls.json" }
