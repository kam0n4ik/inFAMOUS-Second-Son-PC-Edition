# Builds the SecondSonPC native runtime with MSVC (no vcvars needed).
#   powershell -ExecutionPolicy Bypass -File native\build.ps1 [-Config Release|Debug] [-Test] [-Asan] [-Clean]
# Toolchain locations (override with environment variables):
#   SSPC_MSVC_ROOT  E:\BuildTools            (MSVC 14.x, CMake, Ninja)
#   SSPC_WINSDK     E:\SecondSonPC\toolchain\winsdk   (Windows SDK 10.0.26100 unpacked from NuGet)
#   VULKAN_SDK      E:\VulkanSDK
param(
    [ValidateSet("Release", "Debug", "RelWithDebInfo")] [string]$Config = "Release",
    [switch]$Test,
    [switch]$Asan,
    [switch]$Clean
)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path

$BuildTools = if ($env:SSPC_MSVC_ROOT) { $env:SSPC_MSVC_ROOT } else { "E:\BuildTools" }
$WinSdk = if ($env:SSPC_WINSDK) { $env:SSPC_WINSDK } else { "E:\SecondSonPC\toolchain\winsdk" }
$SdkVer = "10.0.26100.0"
$Msvc = (Get-ChildItem "$BuildTools\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1).FullName
$SdkInc = "$WinSdk\cpp\c\Include\$SdkVer"
$SdkLib = "$WinSdk\cpp.x64\c"
$SdkBin = "$WinSdk\buildtools\bin\$SdkVer\x64"
$CMakeBin = "$BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$NinjaBin = "$BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
foreach ($p in @($Msvc, $SdkInc, $SdkLib, $SdkBin, $CMakeBin, $NinjaBin)) {
    if (-not (Test-Path $p)) { throw "toolchain path missing: $p" }
}
$env:INCLUDE = @("$Msvc\include", "$SdkInc\ucrt", "$SdkInc\um", "$SdkInc\shared", "$SdkInc\winrt") -join ";"
$env:LIB = @("$Msvc\lib\x64", "$SdkLib\ucrt\x64", "$SdkLib\um\x64") -join ";"
$env:PATH = @("$Msvc\bin\Hostx64\x64", $CMakeBin, $NinjaBin, $SdkBin, $env:PATH) -join ";"
if (-not $env:VULKAN_SDK) { $env:VULKAN_SDK = "E:\VulkanSDK" }

$suffix = if ($Asan) { "-asan" } else { "" }
$Build = Join-Path $Root ("build\" + $Config.ToLower() + $suffix)
if ($Clean -and (Test-Path $Build)) { Remove-Item -Recurse -Force $Build }
$asanFlag = if ($Asan) { "-DSSPC_ASAN=ON" } else { "-DSSPC_ASAN=OFF" }

& cmake -S $Root -B $Build -G Ninja "-DCMAKE_BUILD_TYPE=$Config" -DCMAKE_CXX_COMPILER=cl $asanFlag
if ($LASTEXITCODE -ne 0) { throw "configure failed" }
& cmake --build $Build
if ($LASTEXITCODE -ne 0) { throw "build failed" }
if ($Test) {
    Push-Location $Build
    try {
        & ctest --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw "tests failed" }
    } finally { Pop-Location }
}
Write-Host "OK: $Build\bin"
