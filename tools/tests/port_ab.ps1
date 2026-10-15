# SecondSonPC port optimizations: interleaved A/B series (baseline vs candidates) over the village and the beach.
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC-port-optimizations\tools\tests\port_ab.ps1 -Tag <name>
#       -Configs "base=<exe>|ENV=1;ENV2=0,cand=<exe>|ENV=1" [-Scenes user1022,beach] [-Repeats 5] [-Seconds 90]
# Each configuration is "<name>=<exe>|<env list>" (the env list may be empty). Every repeat runs every scene with every
# configuration in turn, the order of the configurations alternating between repeats (ABBA), so slow drift of the
# machine falls on all of them alike. The runs go through the main tree's harness (E:\SecondSonPC\tools\tests\p46_runs.ps1:
# saves, scripted input, logs) with -Exe; an exe must lie under E:\SecondSonPC (e.g. build\ab_port\<name>\) so that it
# finds E:\SecondSonPC\SecondSonPC.toml as its root. Artifacts -> E:\SecondSonPC\logs\port\<Tag>\<config>_r<n>\<scene>_1\.
# Frame windows: village (user1022) 25..50 s, beach 62..88 s (the P4.9/P5.4 windows) -> win.json (tools\frame_stats.py).
# tools\tests\port_ab_summary.py sums the series up.
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [Parameter(Mandatory = $true)][string]$Configs,
    [string]$Scenes = "user1022,beach",
    [int]$Repeats = 5,
    [int]$Seconds = 90,
    [string]$ExtraArgs = "",
    [string]$Commit = "" # the source commit every exe was built from (recorded in series.txt)
)
$ErrorActionPreference = "Stop"
$root = "E:\SecondSonPC"
$here = Split-Path -Parent $PSCommandPath
$windows = @{ user1022 = @(25, 50); beach = @(62, 88); early = @(62, 88) }
$defs = [ordered]@{}
foreach ($c in ($Configs -split ',' | Where-Object { $_ })) {
    $name, $rest = $c -split '=', 2
    $exe, $envs = $rest -split '\|', 2
    if (-not (Test-Path $exe)) { throw "config '$name': no exe '$exe'" }
    if (-not $exe.StartsWith("$root\", [StringComparison]::OrdinalIgnoreCase)) { throw "config '$name': exe must lie under $root" }
    $defs[$name] = @{ exe = $exe; env = "$envs" }
}
$out = "$root\logs\port\$Tag"
New-Item -ItemType Directory -Force $out | Out-Null
"port A/B '$Tag': scenes $Scenes, repeats $Repeats, $Seconds s, extra '$ExtraArgs'" | Out-File "$out\series.txt" -Encoding utf8
if ($Commit) { "commit $Commit" | Out-File "$out\series.txt" -Encoding utf8 -Append }
foreach ($n in $defs.Keys) {
    $sha = (Get-FileHash $defs[$n].exe -Algorithm SHA256).Hash
    "  $n = $($defs[$n].exe) | $($defs[$n].env)" | Out-File "$out\series.txt" -Encoding utf8 -Append
    "    sha256 $sha" | Out-File "$out\series.txt" -Encoding utf8 -Append
}
$names = @($defs.Keys)
$reversed = @($names); [array]::Reverse($reversed)
for ($rep = 1; $rep -le $Repeats; $rep++) {
    $order = if ($rep % 2 -eq 1) { $names } else { $reversed }
    foreach ($scene in ($Scenes -split ',' | Where-Object { $_ })) {
        foreach ($cfg in $order) {
            $runTag = "${cfg}_r$rep"
            $dir = "$out\$runTag\${scene}_1"
            if (Test-Path "$dir\win.json") { continue } # resumed series: done earlier
            $sampler = Start-Job -ArgumentList "$out\$runTag", ($Seconds + 40) -ScriptBlock {
                param($d, $limit)
                New-Item -ItemType Directory -Force $d | Out-Null
                $rows = @("t_s,pid,cpu_s,working_set_mib,private_mib")
                $t0 = Get-Date
                while (((Get-Date) - $t0).TotalSeconds -lt $limit) {
                    $p = Get-Process SecondSonPC -ErrorAction SilentlyContinue | Sort-Object PrivateMemorySize64 -Descending |
                        Select-Object -First 1
                    if ($p) {
                        $rows += [string]::Format([Globalization.CultureInfo]::InvariantCulture, "{0:F1},{1},{2:F2},{3:F0},{4:F0}",
                            ((Get-Date) - $t0).TotalSeconds, $p.Id, $p.TotalProcessorTime.TotalSeconds,
                            ($p.WorkingSet64 / 1MB), ($p.PrivateMemorySize64 / 1MB))
                    }
                    Start-Sleep -Seconds 1
                }
                $rows | Out-File "$d\cpu_mem_raw.csv" -Encoding ascii
            }
            $runArgs = @("-ExecutionPolicy", "Bypass", "-File", "$root\tools\tests\p46_runs.ps1", "-Tag", $runTag,
                "-Scenes", $scene, "-Repeats", "1", "-Seconds", "$Seconds", "-OutRoot", $out, "-Exe", $defs[$cfg].exe)
            if ($defs[$cfg].env) { $runArgs += @("-Env", $defs[$cfg].env) }
            if ($ExtraArgs) { $runArgs += @("-ExtraArgs", $ExtraArgs) }
            powershell @runArgs | Out-Null
            Wait-Job $sampler -Timeout 60 | Out-Null
            Stop-Job $sampler -ErrorAction SilentlyContinue
            Remove-Job $sampler -Force -ErrorAction SilentlyContinue
            if (Test-Path "$out\$runTag\cpu_mem_raw.csv") { Move-Item "$out\$runTag\cpu_mem_raw.csv" "$dir\cpu_mem.csv" -Force }
            if (Test-Path "$dir\gpu_frames.csv") {
                $w = $windows[$scene]
                $ErrorActionPreference = "Continue"
                py -I "$root\tools\frame_stats.py" "$dir\gpu_frames.csv" --from $w[0] --to $w[1] --json "$dir\win.json" 2>&1 | Out-Null
                $ErrorActionPreference = "Stop"
            }
            $state = if (Test-Path "$dir\win.json") { "done" } else { "FAILED (no win.json)" }
            "$(Get-Date -Format s) $runTag $scene $state" | Out-File "$out\series.txt" -Encoding utf8 -Append
        }
    }
}
py -I -X utf8 "$here\port_ab_summary.py" $out
