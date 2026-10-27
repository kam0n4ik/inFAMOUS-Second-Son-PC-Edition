# inFAMOUS PC Launcher - runs the tests of a build made with tools\build.ps1 -Tests (fixtures in %TEMP% only; the
# stub game is the only program started). The output of each test goes to build\<config>\tests\<name>.txt.
param([string]$Config = "Release")
. "$PSScriptRoot\env.ps1"
$ErrorActionPreference = "Continue"
$bin = Join-Path (Split-Path -Parent $PSScriptRoot) "build\$Config\bin"
$outDir = Join-Path (Split-Path -Parent $PSScriptRoot) "build\$Config\tests"
New-Item -ItemType Directory -Force $outDir | Out-Null
$failed = 0
foreach ($t in Get-ChildItem $bin -Filter "tst_*.exe") {
    $log = Join-Path $outDir ($t.BaseName + ".txt")
    & $t.FullName -o "$log,txt" | Out-Null
    if ($LASTEXITCODE -ne 0) { $failed++ }
    Get-Content $log -Encoding UTF8 | Where-Object { $_ -match "^(PASS|FAIL|SKIP|XFAIL|Totals)|^\s+(Loc|Actual|Expected)" }
}
"failed test programs: $failed"
exit $failed
