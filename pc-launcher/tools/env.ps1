# inFAMOUS PC Launcher - the build environment: MSVC 14.44 of E:\BuildTools, the Windows SDK 10.0.26100 kept in
# E:\SecondSonPC\toolchain (read only: nothing there is changed), CMake and Ninja of E:\BuildTools, Qt 6.10.3 (msvc2022_64)
# in E:\Qt. Dot-source it: . tools\env.ps1  (each path can be overridden by the variable of the same name).
$ErrorActionPreference = "Stop"
$env:VSLANG = "1033"   # the compiler's messages in English (the logs are read by tools)
if (-not $env:LAUNCHER_QT) { $env:LAUNCHER_QT = "E:\Qt\6.10.3\msvc2022_64" }
if (-not $env:LAUNCHER_MSVC_ROOT) { $env:LAUNCHER_MSVC_ROOT = "E:\BuildTools\VC\Tools\MSVC" }
if (-not $env:LAUNCHER_WINSDK) { $env:LAUNCHER_WINSDK = "E:\SecondSonPC\toolchain\winsdk" }
if (-not $env:LAUNCHER_CMAKE_BIN) { $env:LAUNCHER_CMAKE_BIN = "E:\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin" }
if (-not $env:LAUNCHER_NINJA_BIN) { $env:LAUNCHER_NINJA_BIN = "E:\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja" }

$msvc = (Get-ChildItem $env:LAUNCHER_MSVC_ROOT -Directory | Sort-Object Name | Select-Object -Last 1).FullName
$sdkVer = "10.0.26100.0"
$sdkInc = "$env:LAUNCHER_WINSDK\cpp\c\Include\$sdkVer"
$sdkLib = "$env:LAUNCHER_WINSDK\cpp.x64\c"
$sdkBin = "$env:LAUNCHER_WINSDK\buildtools\bin\$sdkVer\x64"
foreach ($p in @($env:LAUNCHER_QT, $msvc, $sdkInc, $sdkLib, $sdkBin, $env:LAUNCHER_CMAKE_BIN, $env:LAUNCHER_NINJA_BIN)) {
    if (-not (Test-Path $p)) { throw "build tool missing: $p (docs\BUILD.md)" }
}
$env:INCLUDE = @("$msvc\include", "$sdkInc\ucrt", "$sdkInc\um", "$sdkInc\shared", "$sdkInc\winrt") -join ";"
$env:LIB = @("$msvc\lib\x64", "$sdkLib\ucrt\x64", "$sdkLib\um\x64") -join ";"
$env:PATH = @("$msvc\bin\Hostx64\x64", $sdkBin, $env:LAUNCHER_CMAKE_BIN, $env:LAUNCHER_NINJA_BIN, "$env:LAUNCHER_QT\bin", $env:PATH) -join ";"
$env:LAUNCHER_MSVC = $msvc
# the MSVC runtime the release package carries (no installer needed on the player's PC)
$redist = Get-ChildItem "E:\BuildTools\VC\Redist\MSVC" -Directory -ErrorAction SilentlyContinue | Where-Object { Test-Path "$($_.FullName)\x64\Microsoft.VC143.CRT" } | Sort-Object Name | Select-Object -Last 1
if ($redist) { $env:LAUNCHER_CRT = "$($redist.FullName)\x64\Microsoft.VC143.CRT" }
