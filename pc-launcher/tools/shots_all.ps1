# inFAMOUS PC Launcher - the screenshot set of docs\screenshots: 3 concepts x 2 games x 4 pages, plus sizes, scales,
# English, reduced effects and the not-found / ambiguity screens. Only the launcher is started, never a game.
param(
    [string]$SS = "E:\SecondSonPC",
    [string]$FL = "E:\inFAMOUS-First-Light-PC-Edition",
    [string]$Out = "",
    [string]$Exe = "",
    [switch]$Quick
)
$root = Split-Path -Parent $PSScriptRoot
if (-not $Out) { $Out = Join-Path $root "docs\screenshots" }
$shot = Join-Path $PSScriptRoot "shot.ps1"
$extra = @{}
if ($Exe) { $extra.Exe = $Exe }
function Shot($name, $gameRoot, $params) {
    $file = Join-Path $Out "$name.png"
    $r = & $shot -Root $gameRoot -Out $file @params @extra
    $r
}
$games = @{ ss = $SS; fl = $FL }
foreach ($g in @("ss", "fl")) {
    foreach ($c in @("cinematic", "neon", "desktop")) {
        $pages = if ($Quick) { @("home") } else { @("home", "settings", "saves", "diagnostics") }
        foreach ($p in $pages) { Shot "$c-$g-$p" $games[$g] @{ Concept = $c; Page = $p } }
    }
}
if ($Quick) { return }
Shot "cinematic-ss-home-1920x1080" $SS @{ Size = "1920x1080" }
Shot "cinematic-fl-home-2560x1440" $FL @{ Size = "2560x1440" }
Shot "cinematic-ss-home-3840x2160-scale2" $SS @{ Size = "1920x1080"; Scale = "2" }
Shot "neon-fl-settings-1440-scale1.5" $FL @{ Concept = "neon"; Page = "settings"; Size = "1707x960"; Scale = "1.5" }
Shot "cinematic-ss-home-960x600" $SS @{ Size = "960x600" }
Shot "desktop-fl-settings-960x600" $FL @{ Concept = "desktop"; Page = "settings"; Size = "960x600" }
Shot "cinematic-ss-home-en" $SS @{ Lang = "en" }
Shot "neon-fl-diagnostics-en" $FL @{ Concept = "neon"; Page = "diagnostics"; Lang = "en" }
Shot "cinematic-ss-home-reduced" $SS @{ Reduced = $true }
$none = Join-Path $env:TEMP "launcher-shot-nogame"
New-Item -ItemType Directory -Force $none | Out-Null
Shot "notfound" $none @{}
