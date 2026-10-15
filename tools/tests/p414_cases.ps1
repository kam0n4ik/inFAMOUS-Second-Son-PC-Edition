# SecondSonPC P4.14: the shader cache's failure and compatibility cases (docs\PHASE4_SHADER_PRECOMPILER.md §10 D-H) on
# one scene, each from a copy of a warm cache (p414_matrix.ps1's) so that the cases do not change one another.
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\tests\p414_cases.ps1 -Tag <name> -Source <cache dir>
#       [-Scene user1022] [-WarmNv <driver cache folder>] [-Only D,E,F,G,V,H]
#   D   another driver (SSPC_DIAG_PIPELINE_CACHE_DRIVER=1): vulkan.bin refused, an empty driver cache; the driver's own
#       cache empty too (as after an update): everything is compiled from the cached SPIR-V
#   E   shaders.pack damaged in the middle (16 bytes flipped): the records from there on cut, the rest used
#   F   a crash while writing shaders.pack (SSPC_DIAG_SHADER_CACHE_TEAR, an empty cache: writes start at once), then a
#       normal run on it; a crash while saving vulkan.bin (SSPC_DIAG_PIPELINE_CACHE_TEAR), then a normal run
#   G   the parallel command processor + block upload (SSPC_P410_PARALLEL_CP=1, SSPC_P49_BLOCK_UPLOAD=1), warm caches
#   V   Vulkan validation + synchronization validation (--gpu-native-validate), warm caches with precompilation
#   H   the settings menu's "clear the shader cache" (clear_shaders.request): the next start clears it, then fills it
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [Parameter(Mandatory = $true)][string]$Source,
    [string]$Scene = "user1022",
    [string]$WarmNv = "",
    [string]$Only = "D,E,F,G,V,H"
)
$ErrorActionPreference = "Stop"
$root = "E:\SecondSonPC"
$base = "$root\logs\p414\$Tag"
New-Item -ItemType Directory -Force $base | Out-Null
$log = "$base\cases.txt"
$cases = $Only -split ','

function Copy-Cache([string]$name) {
    $dst = "$base\cache_$name"
    if (Test-Path $dst) { Remove-Item $dst -Recurse -Force }
    Copy-Item $Source $dst -Recurse
    return $dst
}
function Run([string]$cfg, [string]$cache, [string]$envs, [int]$seconds = 60, [switch]$Validate) {
    "[$(Get-Date -Format HH:mm:ss)] $cfg : cache $cache, $envs" | Out-File $log -Append -Encoding utf8
    $extra = if ($cache) { "--shader-cache $cache" } else { "--no-shader-cache" }
    $a = @{ Tag = $cfg; Scenes = $Scene; Repeats = 1; Seconds = $seconds; OutRoot = $base; Env = $envs; ExtraArgs = $extra }
    if ($Validate) { $a.Validate = $true }
    # (a case that ends the game on purpose must not end the series: the error is logged, the next case runs)
    try { & "$root\tools\tests\p46_runs.ps1" @a | Select-Object -Last 1 | Out-File $log -Append -Encoding utf8 }
    catch { "  $cfg : the run script stopped: $($_.Exception.Message)" | Out-File $log -Append -Encoding utf8 }
}
function New-Nv([string]$name) {
    $d = "$base\nv_$name"
    New-Item -ItemType Directory -Force $d | Out-Null
    return $d
}
$warm = if ($WarmNv) { $WarmNv } else { New-Nv "warm" }
$trace = "SSPC_SHADER_STALL_TRACE=1"

if ($cases -contains "D") {
    $c = Copy-Cache "D"
    Run "D" $c "__GL_SHADER_DISK_CACHE_PATH=$(New-Nv 'D');$trace;SSPC_DIAG_PIPELINE_CACHE_DRIVER=1"
}
if ($cases -contains "E") {
    $c = Copy-Cache "E"
    $pack = Get-ChildItem $c -Recurse -Filter shaders.pack | Select-Object -First 1
    $bytes = [IO.File]::ReadAllBytes($pack.FullName)
    $mid = [int]($bytes.Length / 2)
    for ($i = 0; $i -lt 16; $i++) { $bytes[$mid + $i] = $bytes[$mid + $i] -bxor 0x5A }
    [IO.File]::WriteAllBytes($pack.FullName, $bytes)
    "E: 16 bytes flipped at $mid of $($bytes.Length) in $($pack.FullName)" | Out-File $log -Append -Encoding utf8
    Run "E" $c "__GL_SHADER_DISK_CACHE_PATH=$warm;$trace"
}
if ($cases -contains "F") {
    $c = "$base\cache_F"
    if (Test-Path $c) { Remove-Item $c -Recurse -Force }
    Run "F1" $c "__GL_SHADER_DISK_CACHE_PATH=$warm;$trace;SSPC_DIAG_SHADER_CACHE_TEAR=40" 40
    Run "F2" $c "__GL_SHADER_DISK_CACHE_PATH=$warm;$trace"
    $before = Get-ChildItem $c -Recurse -Filter vulkan.bin | Select-Object -First 1
    "F3: vulkan.bin before: $($before.Length) bytes, $($before.LastWriteTime)" | Out-File $log -Append -Encoding utf8
    Run "F3" $c "__GL_SHADER_DISK_CACHE_PATH=$warm;$trace;SSPC_DIAG_PIPELINE_CACHE_TEAR=4096"
    $after = Get-ChildItem $c -Recurse -Filter vulkan.bin | Select-Object -First 1
    $tmp = Get-ChildItem $c -Recurse -Filter vulkan.bin.tmp | Select-Object -First 1
    "F3: vulkan.bin after: $($after.Length) bytes, $($after.LastWriteTime); vulkan.bin.tmp $($tmp.Length) bytes" |
        Out-File $log -Append -Encoding utf8
    Run "F4" $c "__GL_SHADER_DISK_CACHE_PATH=$warm;$trace"
}
if ($cases -contains "G") {
    $c = Copy-Cache "G"
    Run "G" $c "__GL_SHADER_DISK_CACHE_PATH=$warm;$trace;SSPC_P410_PARALLEL_CP=1;SSPC_P49_BLOCK_UPLOAD=1" 90
}
if ($cases -contains "V") {
    $c = Copy-Cache "V"
    Run "V" $c "__GL_SHADER_DISK_CACHE_PATH=$warm" 60 -Validate
}
if ($cases -contains "H") {
    $c = Copy-Cache "H"
    "requested by p414_cases.ps1" | Out-File "$c\clear_shaders.request" -Encoding ascii
    Run "H" $c "__GL_SHADER_DISK_CACHE_PATH=$warm;$trace"
}
"[$(Get-Date -Format HH:mm:ss)] done" | Out-File $log -Append -Encoding utf8
