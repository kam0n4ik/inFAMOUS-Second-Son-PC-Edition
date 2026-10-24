# Builds SecondSonPC.exe and the folder that goes into the game's folder (README.md, Building from source).
#   powershell -ExecutionPolicy Bypass -File build.ps1 [-Config RelWithDebInfo] [-Fresh] [-NoPackage]
#                                                      [-VsPath <Visual Studio / Build Tools>] [-LlvmPath <LLVM\bin>]
# Steps:
#   1. third_party\shadPS4: the pinned shadPS4 commit with its submodules (cloned once; patches\shadps4 are applied by
#      the CMake configure step).
#   2. The compiler environment: the current one when it already has MSVC's headers and libraries (a Developer
#      PowerShell, or any script that set INCLUDE / LIB), otherwise vcvars64.bat of the newest Visual Studio 2022 /
#      Build Tools found by vswhere (or -VsPath).
#   3. CMake + Ninja with clang-cl and lld-link (LLVM 20 or later; -LlvmPath, PATH, C:\Program Files\LLVM\bin, or the
#      clang of Visual Studio).
#   4. dist\SecondSonPC: SecondSonPC.exe, its DLLs and fonts from build\bin plus everything in game\ - the files to put
#      into the game's folder - and dist\SecondSonPC-win64.zip of the same.
# DLSS / FSR need the vendors' SDKs in third_party\upscalers (README.md); without them the build has no upscalers.
param(
    [string]$Config = "RelWithDebInfo",
    [switch]$Fresh,
    [switch]$NoPackage,
    [string]$VsPath = "",
    [string]$LlvmPath = ""
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$bld = Join-Path $root "build"
$shad = Join-Path $root "third_party\shadPS4"
$shadCommit = "0fe263a4760dfbfa973366890061749b4af0de97" # docs/PATCHES.md: the patches are made against it
$log = Join-Path $bld "build.log"

function Invoke-Logged([string]$what, [string]$cmd) {
    Write-Host "== $what"
    # (Windows PowerShell 5.1 turns a native program's stderr into errors: run it through cmd, the log gets both)
    cmd /c "$cmd >> `"$log`" 2>&1"
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE), see $log" }
}

New-Item -ItemType Directory -Force $bld | Out-Null
"build started $(Get-Date -Format s)" | Set-Content $log

# 1. shadPS4 at the pinned commit
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw "git is not in PATH" }
if (-not (Test-Path (Join-Path $shad ".git"))) {
    Invoke-Logged "clone shadPS4" "git clone --no-checkout https://github.com/shadps4-emu/shadPS4.git `"$shad`""
}
$head = (git -C $shad rev-parse HEAD 2>$null)
if ($head -ne $shadCommit) {
    Invoke-Logged "checkout shadPS4 $($shadCommit.Substring(0, 10))" "git -C `"$shad`" checkout --quiet $shadCommit"
}
# (--force: an update stopped by a network error leaves the submodules it had cloned but not yet checked out empty, and
# a rerun without it takes them as up to date; patches\shadps4 change only shadPS4's src/, never a submodule)
Invoke-Logged "shadPS4 submodules" "git -C `"$shad`" submodule update --init --recursive --force --jobs 8"

# 2. compiler environment
$haveEnv = $env:INCLUDE -and $env:LIB -and (Get-Command link.exe -ErrorAction SilentlyContinue)
if (-not $haveEnv) {
    if (-not $VsPath) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
        if (-not (Test-Path $vswhere)) { throw "Visual Studio 2022 / Build Tools not found (no vswhere.exe); give -VsPath" }
        $VsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                             -property installationPath
        if (-not $VsPath) { throw "no Visual Studio with the C++ x64 tools (workload 'Desktop development with C++')" }
    }
    $vcvars = Join-Path $VsPath "VC\Auxiliary\Build\vcvars64.bat"
    if (-not (Test-Path $vcvars)) { throw "$vcvars not found" }
    Write-Host "== environment of $vcvars"
    foreach ($line in (cmd /c "`"$vcvars`" >nul && set")) {
        if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2]) }
    }
    if (-not $env:WindowsSdkDir) { throw "vcvars64.bat found no Windows SDK (install 'Windows 11 SDK' with the C++ tools)" }
}

# 3. clang-cl, CMake, Ninja
$candidates = @($LlvmPath, "", "$env:ProgramFiles\LLVM\bin")
if ($env:VCINSTALLDIR) { $candidates += (Join-Path $env:VCINSTALLDIR "Tools\Llvm\x64\bin") }
$clang = $null
foreach ($c in $candidates) {
    $exe = if ($c) { Join-Path $c "clang-cl.exe" } else { (Get-Command clang-cl.exe -ErrorAction SilentlyContinue).Source }
    if ($exe -and (Test-Path $exe)) { $clang = $exe; break }
}
if (-not $clang) { throw "clang-cl.exe not found: install LLVM 20 (or later) or give -LlvmPath <LLVM\bin>" }
$env:PATH = "$(Split-Path $clang);$env:PATH"
foreach ($tool in "cmake", "ninja") {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool is not in PATH (Visual Studio ships both)" }
}
Write-Host "== $(& $clang --version | Select-Object -First 1)"

if ($Fresh -or -not (Test-Path (Join-Path $bld "build.ninja"))) {
    Invoke-Logged "configure" ("cmake -S `"$root`" -B `"$bld`" -G Ninja -DCMAKE_BUILD_TYPE=$Config " +
        "-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER_TYPE=LLD " +
        "-DCMAKE_RC_COMPILER=rc.exe -DCMAKE_MT=mt.exe" + $(if ($Fresh) { " --fresh" } else { "" }))
}
Invoke-Logged "build" "cmake --build `"$bld`" --target shadps4 --parallel $([Environment]::ProcessorCount)"
$exePath = Join-Path $bld "bin\SecondSonPC.exe"
Get-Item $exePath | Select-Object FullName, Length, LastWriteTime | Format-Table -AutoSize | Out-String | Write-Host
if ($NoPackage) { return }

# 4. the folder for the game's folder
$dist = Join-Path $root "dist\SecondSonPC"
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force $dist | Out-Null
Copy-Item $exePath, (Join-Path $bld "bin\*.dll") $dist
Copy-Item -Recurse (Join-Path $bld "bin\fonts") $dist
Copy-Item -Recurse (Join-Path $root "game\*") $dist
# the vendors' licences of the upscaler DLLs shipped beside the executable
$ups = Join-Path $root "third_party\upscalers"
foreach ($l in @(@("ngx\LICENSE.txt", "NVIDIA-DLSS-SDK.txt"), @("ffx-api\LICENSE.txt", "AMD-FidelityFX-SDK.txt"),
                 @("streamline\license.txt", "NVIDIA-Streamline.txt"))) {
    $from = Join-Path $ups $l[0]
    if (Test-Path $from) {
        New-Item -ItemType Directory -Force (Join-Path $dist "licenses") | Out-Null
        Copy-Item $from (Join-Path $dist "licenses\$($l[1])")
    }
}
# LibAtrac9 (MIT), compiled into the executable by SecondSonPC's own AJM (src/runtime/audio/ajm.cpp)
New-Item -ItemType Directory -Force (Join-Path $dist "licenses") | Out-Null
Copy-Item (Join-Path $root "third_party\LibAtrac9\LICENSE") (Join-Path $dist "licenses\LibAtrac9.txt")
$zip = Join-Path $root "dist\SecondSonPC-win64.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $dist "*") -DestinationPath $zip
Write-Host "== $dist (copy its contents into the game's folder, the one with eboot.bin)"
Write-Host "== $zip"
