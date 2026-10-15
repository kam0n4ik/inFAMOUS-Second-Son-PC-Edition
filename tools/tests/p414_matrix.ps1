# SecondSonPC P4.14: the shader cache / precompiler matrix over the game scenes (docs\PHASE4_SHADER_PRECOMPILER.md §10).
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\tests\p414_matrix.ps1 -Tag <name>
#       [-Scenes user1022,beach,early] [-Configs A,R,B,C,CW] [-Seconds 90] [-Env "A=b"]
# Per scene, in this order (each a p46_runs.ps1 run with SSPC_SHADER_STALL_TRACE=1):
#   A   the NVIDIA driver's shader cache empty (__GL_SHADER_DISK_CACHE_PATH: a new folder), SecondSonPC's cache empty
#   R   the driver's cache warm (A's folder), no SecondSonPC cache (--no-shader-cache): a repeated run before P4.14
#   B   the driver's cache empty, SecondSonPC's cache warm, no precompilation (SSPC_SHADER_PRECOMPILE=0)
#   C   the driver's cache empty, SecondSonPC's cache warm, precompilation at start
#   CW  the driver's cache warm, SecondSonPC's cache warm, precompilation at start: a repeated run with P4.14
# Folders: logs\p414\<Tag>\<config>\<scene>_1 (p46_runs' artifacts); caches in logs\p414\<Tag>\cache\<scene>,
# driver caches in logs\p414\<Tag>\nv\<scene>_<config>. tools\p414\p414_report.py turns them into the tables.
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$Scenes = "user1022,beach,early",
    [string]$Configs = "A,R,B,C,CW",
    [int]$Seconds = 90,
    [string]$Env = ""
)
$ErrorActionPreference = "Stop"
$root = "E:\SecondSonPC"
$base = "$root\logs\p414\$Tag"
New-Item -ItemType Directory -Force $base | Out-Null
foreach ($scene in ($Scenes -split ',' | Where-Object { $_ })) {
    $cache = "$base\cache\$scene"
    $nvWarm = "$base\nv\${scene}_A"
    foreach ($cfg in ($Configs -split ',' | Where-Object { $_ })) {
        $nv = switch ($cfg) {
            "R" { $nvWarm }
            "CW" { $nvWarm }
            "A" { $nvWarm }
            default { "$base\nv\${scene}_$cfg" }
        }
        New-Item -ItemType Directory -Force $nv | Out-Null
        $envs = "__GL_SHADER_DISK_CACHE_PATH=$nv;SSPC_SHADER_STALL_TRACE=1"
        $extra = "--shader-cache $cache"
        switch ($cfg) {
            "R" { $extra = "--no-shader-cache" }
            "B" { $envs += ";SSPC_SHADER_PRECOMPILE=0" }
            default { $envs += ";SSPC_SHADER_PRECOMPILE=1" }
        }
        if ($Env) { $envs += ";$Env" }
        "[$(Get-Date -Format HH:mm:ss)] $scene $cfg : nv $nv, $extra, $envs" | Out-File "$base\matrix.txt" -Append -Encoding utf8
        & "$root\tools\tests\p46_runs.ps1" -Tag $cfg -Scenes $scene -Repeats 1 -Seconds $Seconds -OutRoot $base -Env $envs `
            -ExtraArgs $extra | Select-Object -Last 1 | Out-File "$base\matrix.txt" -Append -Encoding utf8
    }
}
"[$(Get-Date -Format HH:mm:ss)] done" | Out-File "$base\matrix.txt" -Append -Encoding utf8
