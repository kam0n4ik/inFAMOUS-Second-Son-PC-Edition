# inFAMOUS PC Launcher - configures and builds with CMake + Ninja (MSVC x64, Qt 6).
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 [-Config Release|Debug] [-Fresh] [-Tests]
# Output: build\<config>\bin\Launcher.exe (+ the tests in build\<config>\tests when -Tests). The log: build\<config>\build.log.
param(
    [ValidateSet("Release", "Debug", "RelWithDebInfo")] [string]$Config = "Release",
    [switch]$Fresh,
    [switch]$Tests,
    [switch]$Light   # 2 jobs at idle priority: allowed while a game runs (it must not feel the build)
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot\env.ps1"

# never compete with a game being played: no build while SecondSonPC.exe / FirstLightPC.exe run
$playing = Get-Process -Name SecondSonPC, FirstLightPC -ErrorAction SilentlyContinue
if ($playing -and -not $Light) { throw "a game is running ($($playing.Name -join ', ')): build later, or with -Light" }
$jobs = if ($Light) { 2 } else { [Environment]::ProcessorCount }
try { (Get-Process -Id $PID).PriorityClass = $(if ($Light) { "Idle" } else { "BelowNormal" }) } catch {}

$bld = Join-Path $root "build\$Config"
New-Item -ItemType Directory -Force $bld | Out-Null
$log = Join-Path $bld "build.log"
$tmp = Join-Path $root "build\.tmp"
New-Item -ItemType Directory -Force $tmp | Out-Null
$env:TEMP = $tmp; $env:TMP = $tmp

$ErrorActionPreference = "Continue"
if ($Fresh -or $Tests -or -not (Test-Path "$bld\build.ninja")) {
    $testsFlag = if ($Tests) { "ON" } else { "OFF" }
    cmd /c "cmake -S `"$root`" -B `"$bld`" -G Ninja -DCMAKE_BUILD_TYPE=$Config -DCMAKE_PREFIX_PATH=`"$env:LAUNCHER_QT`" -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DCMAKE_RC_COMPILER=rc.exe -DCMAKE_MT=mt.exe -DLAUNCHER_TESTS=$testsFlag > `"$log`" 2>&1"
    if ($LASTEXITCODE -ne 0) { Get-Content $log -Tail 40; throw "cmake configure failed ($LASTEXITCODE), see $log" }
} else {
    "" | Set-Content -Encoding ASCII $log
}
$t0 = Get-Date
cmd /c "cmake --build `"$bld`" --parallel $jobs >> `"$log`" 2>&1"
$rc = $LASTEXITCODE
"build rc=$rc in $([int]((Get-Date) - $t0).TotalSeconds) s" | Add-Content -Encoding ASCII $log
if ($rc -ne 0) { Get-Content $log | Select-String -Pattern "error|FAILED" | Select-Object -First 40 | ForEach-Object { $_.Line }; throw "build failed ($rc), see $log" }
Get-Item "$bld\bin\Launcher.exe" | Select-Object FullName, Length, LastWriteTime
