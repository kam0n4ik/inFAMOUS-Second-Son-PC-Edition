# R10 pacing: deep P4.12 + P5.4 frame traces of several configurations, one after another (tools\r9\r9run.ps1 of this
# worktree), each analysed by tools\r10\c1chain.py into <run>\c1chain.txt. Not a measured series (the traces cost FPS).
#   powershell -File tools\r10\traces.ps1 -Out E:\research\r10\runs\pacing\<set> -Scene user -Route look
#       -Window "50:60,records=0,sample=0" -Configs "aq|SSPC_REA_DEFER_WAIT_B=1;SSPC_FB_ASYNC_COMPUTE=1#..." [-NoTrace]
param([Parameter(Mandatory)] [string]$Out, [Parameter(Mandatory)] [string]$Configs, [string]$Scene = "user",
      [string]$Route = "look", [string]$Window = "50:60,records=0,sample=0", [string]$Worktree = "E:\SecondSonPC-r10-pacing",
      [string]$Base = "SSPC_R9_PCACHE=1;SSPC_R9_HEAP_WINDOW=1", [switch]$NoTrace)
$ErrorActionPreference = "Continue"
# (a build of another agent started before the quiet flag runs on: wait for its lock - stale after 120 min - and for
# a quiet machine, < 8 % of the CPUs over 10 s, before each run; a lock older than 30 min is stale)
function Wait-Stand {
    $lock = "E:\research\r10\build.lock"
    while ((Test-Path $lock) -and ((Get-Date) - (Get-Item $lock).LastWriteTime).TotalMinutes -lt 30) { Start-Sleep -Seconds 15 }
    $c = New-Object System.Diagnostics.PerformanceCounter("Processor", "% Processor Time", "_Total")
    for ($k = 0; $k -lt 60; $k++) {
        [void]$c.NextValue(); $v = @()
        for ($i = 0; $i -lt 10; $i++) { Start-Sleep -Seconds 1; $v += $c.NextValue() }
        if ((($v | Measure-Object -Average).Average) -lt 8) { return }
    }
}
New-Item -ItemType Directory -Force $Out | Out-Null
foreach ($c in $Configs.Split('#', [StringSplitOptions]::RemoveEmptyEntries)) {
    $n, $e = $c.Split('|', 2)
    $env1 = "$Base;$e"
    if (-not $NoTrace) { $env1 += ";SSPC_P54_FRAME_TRACE=1;SSPC_P412_STALL_TRACE=deep,window=$Window" }
    Wait-Stand
    "$(Get-Date -Format 'HH:mm:ss') $n : $env1" | Add-Content "$Out\traces.txt"
    & "$Worktree\tools\r9\r9run.ps1" -Worktree $Worktree -Tag $n -Route $Route -Scene $Scene -Env $env1 -Out $Out |
        Select-Object -Last 1 | Add-Content "$Out\traces.txt"
    & python -I "$Worktree\tools\r10\bg.py" "$Out\$n" 2>&1 | Add-Content "$Out\traces.txt"
    if (Test-Path "$Out\$n\p412_trace.bin") {
        & python -I -X utf8 "$Worktree\tools\r10\c1chain.py" "$Out\$n" --rows 60 --csv "$Out\$n\c1chain.csv" 2>&1 |
            Set-Content -Encoding UTF8 "$Out\$n\c1chain.txt"
    }
}
"$(Get-Date -Format 'HH:mm:ss') done" | Add-Content "$Out\traces.txt"
