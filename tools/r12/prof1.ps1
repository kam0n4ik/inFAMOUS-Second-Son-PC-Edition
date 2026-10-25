# R12 lights: the first profile runs of the base (not measured series: no quiet flag; r9run waits for the stand).
#   powershell -File tools\r12\prof1.ps1 [-Name prof1] [-Runs "user_gpuprof,user_diag,..."]
param([string]$Name = "prof1", [string]$Runs = "user_census,user_gpuprof,user_diag,fight_gpuprof,user_gprof")
$ErrorActionPreference = "Continue"
$wt = "E:\SecondSonPC-r10-pacing"
$out = "E:\research\r12\runs\lights\$Name"
$preset = "SSPC_R9_PCACHE=1;SSPC_R9_HEAP_WINDOW=1;SSPC_R10_AJM=native;SSPC_R10_SYSRT=native;SSPC_R11_EARLY_SUBMIT=256"
$defs = @{
    user_census   = @("user", "calm", "SSPC_R12_LIGHT_CENSUS=1")
    user_gpuprof  = @("user", "calm", "SSPC_GPU_PROFILE=1;SSPC_R12_LIGHT_CENSUS=1")
    user_diag     = @("user", "calm", "SSPC_DIAG_LARGE_BINDS=1;SSPC_DIAG_WB_DIRTY=1;SSPC_R12_LIGHT_CENSUS=1")
    fight_gpuprof = @("city", "fight", "SSPC_GPU_PROFILE=1;SSPC_R12_LIGHT_CENSUS=1")
    user_gprof    = @("user", "calm", "SSPC_R9_GPROF=2;SSPC_R12_LIGHT_CENSUS=1")
    fight_census  = @("city", "fight", "SSPC_R12_LIGHT_CENSUS=1")
    user_writers  = @("user", "calm", "SSPC_R12_LIGHT_CENSUS=1;SSPC_R12_WRITER_DETAIL=2edaa0c00")
    fight_writers = @("city", "fight", "SSPC_R12_LIGHT_CENSUS=1;SSPC_R12_WRITER_DETAIL=2edafa400")
    user_gprof2   = @("user", "calm", "SSPC_R9_GPROF=2")
    user_readers  = @("user", "calm", "SSPC_R12_LIGHT_CENSUS=1;SSPC_R12_READERS_OF=2edc52000;SSPC_DIAG_READ_WATCH=2edc12000:640000")
    user_keep     = @("user", "calm", "SSPC_R12_LIGHT_LIST_KEEP=1")
    user_keep_poison = @("user", "calm", "SSPC_R12_LIGHT_LIST_KEEP=1;SSPC_REA_POISON=1")
    user_rea_poison  = @("user", "calm", "SSPC_FB_LAZY_WB=1;SSPC_REA_HEAP_READS=1;SSPC_REA_POISON=1")
    fight_keep_poison = @("city", "fight", "SSPC_R12_LIGHT_LIST_KEEP=1;SSPC_REA_POISON=1")
    fight_rea_poison  = @("city", "fight", "SSPC_FB_LAZY_WB=1;SSPC_REA_HEAP_READS=1;SSPC_REA_POISON=1")
    neon_keep_poison = @("neon", "neon", "SSPC_R12_LIGHT_LIST_KEEP=1;SSPC_REA_POISON=1")
    neon_rea_poison  = @("neon", "neon", "SSPC_FB_LAZY_WB=1;SSPC_REA_HEAP_READS=1;SSPC_REA_POISON=1")
    neon_base     = @("neon", "neon", "")
    user_base_shots = @("user", "calm", "", "50,60,70,80,90,100,110")
    user_keep_shots = @("user", "calm", "SSPC_R12_LIGHT_LIST_KEEP=1;SSPC_REA_POISON=1", "50,60,70,80,90,100,110")
    user_rea_shots  = @("user", "calm", "SSPC_FB_LAZY_WB=1;SSPC_REA_HEAP_READS=1;SSPC_REA_POISON=1", "50,60,70,80,90,100,110")
    neon_keep_shots = @("neon", "neon", "SSPC_R12_LIGHT_LIST_KEEP=1;SSPC_REA_POISON=1", "50,70,90,110")
    neon_rea_shots  = @("neon", "neon", "SSPC_FB_LAZY_WB=1;SSPC_REA_HEAP_READS=1;SSPC_REA_POISON=1", "50,70,90,110")
    neon_base_shots = @("neon", "neon", "", "50,70,90,110")
    user_keep_val   = @("user", "calm", "SSPC_R12_LIGHT_LIST_KEEP=1", "", "--gpu-native-validate")
    fight_keep_val  = @("city", "fight", "SSPC_R12_LIGHT_LIST_KEEP=1", "", "--gpu-native-validate")
    fight_readers = @("city", "fight", "SSPC_R12_LIGHT_CENSUS=1;SSPC_R12_READERS_OF=2edc52000;SSPC_DIAG_READ_WATCH=2edc12000:640000")
}
New-Item -ItemType Directory -Force $out | Out-Null
foreach ($r in $Runs.Split(',')) {
    $d = $defs[$r]
    "$(Get-Date -Format 'HH:mm:ss') $r" | Add-Content "$out\series.txt"
    $shots = if ($d.Count -gt 3) { $d[3] } else { "" }
    $more = if ($d.Count -gt 4) { $d[4] } else { "" }
    & "$wt\tools\r9\r9run.ps1" -Worktree $wt -Tag $r -Scene $d[0] -Route $d[1] -Env "$preset;$($d[2])" -Out $out -Shots $shots -MoreArgs $more | Out-Null
}
"$(Get-Date -Format 'HH:mm:ss') done" | Add-Content "$out\series.txt"
