# inFAMOUS PC Launcher - one screenshot of the launcher (never the game): Launcher.exe --screenshot.
#   tools\shot.ps1 -Root <game folder> -Out <png> [-Page home] [-Concept cinematic] [-Size 1280x760] [-Scale 1] [-Lang ru] [-Reduced] [-Exe <Launcher.exe>]
param(
    [Parameter(Mandatory)] [string]$Root,
    [Parameter(Mandatory)] [string]$Out,
    [string]$Page = "home",
    [string]$Concept = "cinematic",
    [string]$Size = "1280x760",
    [string]$Scale = "1",
    [string]$Lang = "ru",
    [switch]$Reduced,
    [string]$Exe = ""
)
if (-not $Exe) { $Exe = Join-Path (Split-Path -Parent $PSScriptRoot) "build\Release\bin\Launcher.exe" }
. "$PSScriptRoot\env.ps1"
$a = @("--game-root", "`"$Root`"", "--screenshot", "`"$Out`"", "--page", $Page, "--concept", $Concept, "--size", $Size, "--lang", $Lang)
if ($Scale -ne "1") { $a += @("--scale", $Scale) }
if ($Reduced) { $a += "--reduced" }
New-Item -ItemType Directory -Force (Split-Path -Parent $Out) | Out-Null
$p = Start-Process -FilePath $Exe -ArgumentList $a -PassThru -Wait
"exit=$($p.ExitCode) $Out"
