# SecondSonPC P4.6: a measurement series of the native backend (or the bridge) over game scenes, repeated.
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\tests\p46_runs.ps1 -Tag <name>
#       [-Scenes beach,early,user1022] [-Repeats 3] [-Seconds 90] [-Backend sspc|bridge] [-Validate] [-Env "A=b;C=d"]
#       [-Exe <path to SecondSonPC.exe>] [-OutRoot <dir instead of logs\p46>]
# Scenes (each run from a fresh copy of the save; the originals are never written):
#   beach    logs\performance\p43_ab\snapshot\home   the P4.3-P4.5 reference route (beach, heavy foliage after the turn)
#   early    userdata\savedata_backups\2026-10-10_03-44-58_before_manual_sspc   the harbour (the user's earliest save)
#   user1022 logs\p32\scene_user_1022\home           the forest village with fires (autosave 2026-10-10 10:22)
# Every run: title -> Continue (space at 12 s and 22 s), three camera turns at 56..60 s, WM_CLOSE at the end
# (tools\run_phase0.ps1). Artifacts -> logs\p46\<Tag>\<scene>_<n>\ (SecondSonPC.log, gpu_native.txt, gpu_frames.csv,
# vk_validation.log, screen.png, run.json, stats.json = tools\frame_stats.py over 62..88 s, warmup.json over 30..55 s);
# one line per run in logs\p46\<Tag>\summary.txt.
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$Scenes = "beach,early,user1022",
    [int]$Repeats = 3,
    [int]$Seconds = 90,
    [ValidateSet("sspc", "bridge")][string]$Backend = "sspc",
    [switch]$Validate,
    [string]$Env = "",
    [string]$Exe = "",
    [string]$OutRoot = "",
    [string]$ExtraArgs = ""
)
$ErrorActionPreference = "Stop"
$root = "E:\SecondSonPC"
$sources = @{
    beach    = "$root\logs\performance\p43_ab\snapshot\home"
    early    = "$root\userdata\savedata_backups\2026-10-10_03-44-58_before_manual_sspc"
    # (logs\p32\scene_user_1022 was removed in the 2026-10-14 log cleanup; this backup holds the same 10:22 autosave)
    user1022 = "$root\userdata\savedata_backups\2026-10-10_12-47-35_before_manual_sspc"
}
$outRoot = if ($OutRoot) { "$OutRoot\$Tag" } else { "$root\logs\p46\$Tag" }
New-Item -ItemType Directory -Force $outRoot | Out-Null
$summary = "$outRoot\summary.txt"
"p46 series '$Tag': backend $Backend, validate $($Validate.IsPresent), $Seconds s, repeats $Repeats, env '$Env', exe '$Exe'" |
    Out-File $summary -Encoding utf8 -Append

for ($rep = 1; $rep -le $Repeats; $rep++) {
    foreach ($scene in ($Scenes -split ',' | Where-Object { $_ })) {
        if (-not $sources.ContainsKey($scene)) { throw "unknown scene '$scene'" }
        $profileDir = "$root\logs\p46\profile"
        if (Test-Path $profileDir) { Remove-Item $profileDir -Recurse -Force }
        New-Item -ItemType Directory -Force $profileDir | Out-Null
        Copy-Item $sources[$scene] "$profileDir\home" -Recurse
        foreach ($f in "gpu_device_lost.txt", "vk_validation.log", "gpu_native.txt", "gpu_frames.csv",
                    "shader_stall_events.csv", "shader_stall_frames.csv", "shader_stall_frames.txt", "shader_stall_summary.txt",
                    "gpu_cpu_profile.txt", "gpu_cpu_profile_game.txt", "cpu_samples_raw.txt") {
            Remove-Item "$root\logs\$f" -ErrorAction SilentlyContinue
        }
        $press = "space@12,space@22,move:-3000:0@56:0.25,move:-3000:0@58:0.25,move:-3000:0@60:0.25"
        $gameArgs = "--loader sspc --user-data $profileDir --vram-log 5 --press $press"
        if ($Backend -eq "sspc") { $gameArgs += " --gpu-backend sspc" }
        if ($Validate) { $gameArgs += " --gpu-native-validate" }
        if ($ExtraArgs) { $gameArgs += " $ExtraArgs" } # (P4.14: e.g. --shader-cache <dir> for a test cache)
        $saved = @{}
        foreach ($kv in ($Env -split ';' | Where-Object { $_ })) {
            $n, $v = $kv -split '=', 2
            $saved[$n] = [Environment]::GetEnvironmentVariable($n)
            [Environment]::SetEnvironmentVariable($n, $v)
        }
        $runTag = "p46_${Tag}_${scene}_$rep"
        $exeArgs = @()
        if ($Exe) { $exeArgs = @("-Exe", $Exe) }
        try {
            $ErrorActionPreference = "Continue"
            powershell -ExecutionPolicy Bypass -File "$root\tools\run_phase0.ps1" -Seconds $Seconds -Tag $runTag -NoReport -ExtraArgs $gameArgs @exeArgs 2>&1 |
                Select-Object -Last 1 | Out-Null
        } finally {
            $ErrorActionPreference = "Stop"
            foreach ($n in $saved.Keys) { [Environment]::SetEnvironmentVariable($n, $saved[$n]) }
        }
        $out = "$outRoot\${scene}_$rep"
        New-Item -ItemType Directory -Force $out | Out-Null
        foreach ($f in "gpu_device_lost.txt", "gpu_native.txt", "gpu_frames.csv", "SecondSonPC.log", "vk_validation.log",
                    "shader_stall_events.csv", "shader_stall_frames.csv", "shader_stall_frames.txt", "shader_stall_summary.txt",
                    "gpu_cpu_profile.txt", "gpu_cpu_profile_game.txt", "cpu_samples_raw.txt") { # (P5.2: the samplers' output)
            if (Test-Path "$root\logs\$f") { Copy-Item "$root\logs\$f" $out }
        }
        foreach ($f in "screen.png", "run.json") {
            if (Test-Path "$root\logs\runs\$runTag\$f") { Copy-Item "$root\logs\runs\$runTag\$f" $out }
        }
        $log = "$out\SecondSonPC.log"
        $lost = (Select-String -Path $log -Pattern "VK_ERROR_DEVICE_LOST|device is lost" -ErrorAction SilentlyContinue | Measure-Object).Count
        $errors = (Select-String -Path $log -Pattern "\[Error\]" -ErrorAction SilentlyContinue | Measure-Object).Count
        $vval = if (Test-Path "$out\vk_validation.log") { (Get-Content "$out\vk_validation.log" | Measure-Object -Line).Lines } else { "n/a" }
        $fps = "n/a"
        if (Test-Path "$out\gpu_frames.csv") {
            $ErrorActionPreference = "Continue"
            py -I "$root\tools\frame_stats.py" "$out\gpu_frames.csv" --from 62 --to 88 --json "$out\stats.json" 2>&1 | Out-Null
            py -I "$root\tools\frame_stats.py" "$out\gpu_frames.csv" --from 30 --to 55 --json "$out\warmup.json" 2>&1 | Out-Null
            $ErrorActionPreference = "Stop"
            if (Test-Path "$out\stats.json") {
                $s = Get-Content "$out\stats.json" -Raw | ConvertFrom-Json
                $fps = "avg {0} med {1} low1 {2} p95 {3} p99 {4} | cp {5} rec {6} lbl {7} flag {8} empty {9} gpu {10} tail {11}" -f `
                    $s.avg_fps, $s.median_fps, $s.low1_fps, $s.ft_p95_ms, $s.ft_p99_ms, $s.mean_cp_ms, $s.mean_record_ms,
                    $s.mean_blocked_label_ms, $s.mean_blocked_other_ms, $s.mean_empty_ms, $s.mean_gpu_busy_ms, $s.gpu_tail_ms
            }
        }
        $mem = (Select-String -Path $log -Pattern "VRAM sample: local usage ([0-9]+) MiB" -ErrorAction SilentlyContinue | Select-Object -Last 1)
        $vram = if ($mem) { $mem.Matches[0].Groups[1].Value } else { "n/a" }
        $cm = (Select-String -Path $log -Pattern "commit sample: process private ([0-9]+) MiB.*system committed ([0-9]+)" -ErrorAction SilentlyContinue | Select-Object -Last 1)
        $priv = if ($cm) { $cm.Matches[0].Groups[1].Value } else { "n/a" }
        $sys = if ($cm) { $cm.Matches[0].Groups[2].Value } else { "n/a" }
        $status = (Select-String -Path "$out\gpu_native.txt" -Pattern "^STATUS" -ErrorAction SilentlyContinue | Select-Object -First 1).Line
        $line = "{0,-12} lost {1} | validation {2} | [Error] {3} | {4} | VRAM {5} MiB, private {6} MiB, system commit {7} MiB | {8}" -f `
            "${scene}_$rep", $lost, $vval, $errors, $fps, $vram, $priv, $sys, $status
        $line | Out-File $summary -Append -Encoding utf8
        $line
    }
}
"summary: $summary"
