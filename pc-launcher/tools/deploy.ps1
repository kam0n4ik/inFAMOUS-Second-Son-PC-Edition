# inFAMOUS PC Launcher - the standalone package: Launcher.exe of the Release build, the Qt it needs (windeployqt, release
# DLLs only) and the MSVC runtime, in dist\inFAMOUS-PC-Launcher. Copy that folder's content into a game folder's
# Launcher\ subfolder (or next to the game's exe): the launcher finds the game by itself (docs\GAME_DETECTION.md).
param([string]$Out = "")
. "$PSScriptRoot\env.ps1"
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Out) { $Out = Join-Path $root "dist\inFAMOUS-PC-Launcher" }
$exe = Join-Path $root "build\Release\bin\Launcher.exe"
if (-not (Test-Path $exe)) { throw "no Release build: run build_launcher.cmd first" }
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force $Out | Out-Null
Copy-Item $exe $Out

$ErrorActionPreference = "Continue"
& "$env:LAUNCHER_QT\bin\windeployqt.exe" --release --qmldir (Join-Path $root "qml") --no-translations --no-opengl-sw `
    --no-system-d3d-compiler --no-compiler-runtime --skip-plugin-types qmltooling,generic,networkinformation,tls `
    (Join-Path $Out "Launcher.exe") | Out-File -Encoding utf8 (Join-Path $root "dist\windeployqt.log")
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed ($LASTEXITCODE), see dist\windeployqt.log" }
$ErrorActionPreference = "Stop"

# the MSVC runtime next to the exe (no VC++ redistributable to install)
if (-not $env:LAUNCHER_CRT) { throw "the MSVC runtime (Microsoft.VC143.CRT) was not found" }
foreach ($dll in @("msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "vcruntime140.dll", "vcruntime140_1.dll")) {
    Copy-Item (Join-Path $env:LAUNCHER_CRT $dll) $Out
}

# Qt's LGPL, the font's OFL and the notice of what is whose
New-Item -ItemType Directory -Force (Join-Path $Out "licenses") | Out-Null
Copy-Item (Join-Path $root "licenses\*") (Join-Path $Out "licenses")
Copy-Item (Join-Path $root "assets\fonts\OFL-RobotoCondensed.txt") (Join-Path $Out "licenses")

# no debug DLLs, no PDBs
$debug = Get-ChildItem $Out -Recurse -File | Where-Object { $_.Name -match "d\.dll$" -and (Test-Path ($_.FullName -replace "d\.dll$", ".dll")) }
$pdb = Get-ChildItem $Out -Recurse -Filter *.pdb
if ($debug -or $pdb) { throw "debug files in the package: $($debug + $pdb)" }

# no build paths in the exe
$bytes = [IO.File]::ReadAllBytes((Join-Path $Out "Launcher.exe"))
$ascii = [Text.Encoding]::ASCII.GetString($bytes)
$wide = [Text.Encoding]::Unicode.GetString($bytes)
$hits = @()
foreach ($needle in @("E:\inFAMOUS-PC-Launcher", "E:/inFAMOUS-PC-Launcher", "E:\Qt", "E:/Qt", "C:\Users")) {
    if ($ascii.Contains($needle) -or $wide.Contains($needle)) { $hits += $needle }
}
if ($hits) { throw "absolute paths in Launcher.exe: $($hits -join ', ')" }

$files = Get-ChildItem $Out -Recurse -File
"{0}: {1} files, {2:N1} MB, no debug DLLs, no absolute paths in Launcher.exe" -f $Out, $files.Count, (($files | Measure-Object Length -Sum).Sum / 1MB)
