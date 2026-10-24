# R10 sysrt: SSPC_SELFTEST_ONLY=sysrt --selftest --loader sspc of a worktree's exe (docs/R10_SYSRT.md §6), under the
# stand's game lock (a SecondSonPC.exe of its own: a stand run waits for it). Report: <Worktree>\logs\selftest.txt,
# copied to -Out when given.
#   powershell -File tools\r10\sysrt_selftest.ps1 [-Out <dir>] [-Only sysrt]
param(
    [string]$Out = "",
    [string]$Only = "sysrt",
    [string]$Worktree = (Resolve-Path "$PSScriptRoot\..\..").Path
)
$ErrorActionPreference = "Stop"
$lock = "E:\research\deep_perf\game.lock"
$quiet = "E:\research\r10\quiet.flag"
for (;;) { # (the quiet flag first - stand_quiet.ps1 -Wait - then the stand, then the flag again just before the lock)
    while ((& "E:\research\r10\tools\stand_quiet.ps1" -Wait) -ne "free") { }
    if (-not ((Get-Process SecondSonPC -ErrorAction SilentlyContinue) -or ((Test-Path $lock) -and
              ((Get-Date) - (Get-Item $lock).LastWriteTime).TotalMinutes -lt 15)) -and -not (Test-Path $quiet)) { break }
    Start-Sleep -Seconds 10
}
New-Item -ItemType Directory -Force $lock | Out-Null
"$(Get-Date -Format 'HH:mm:ss') sysrt selftest" | Set-Content -Encoding ASCII "$lock\owner.txt"
$report = "$Worktree\logs\selftest.txt"
try {
    $env:SSPC_SELFTEST_ONLY = $Only
    if (Test-Path $report) { Remove-Item $report }
    $p = Start-Process -FilePath "$Worktree\build\bin\SecondSonPC.exe" -ArgumentList "--selftest", "--loader", "sspc" `
        -WorkingDirectory $Worktree -PassThru -Wait -NoNewWindow
    "selftest exit $($p.ExitCode)"
} finally {
    Remove-Item env:SSPC_SELFTEST_ONLY -ErrorAction SilentlyContinue
    Remove-Item $lock -Recurse -Force -ErrorAction SilentlyContinue
}
if ($Out) { New-Item -ItemType Directory -Force $Out | Out-Null; Copy-Item $report $Out }
