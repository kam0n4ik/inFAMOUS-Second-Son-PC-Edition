param(
 [Parameter(Mandatory)][string]$Exe,[Parameter(Mandatory)][string]$Label,
 [ValidateSet('selftest','probe','game','stability')][string]$Mode='game',
 [string]$Scene='user1022',[int]$Seconds=90,[int]$Repeat=1,
 [switch]$Visible,[switch]$Validate,[switch]$Parallel,[switch]$Block,[string[]]$ExtraArgs=@()
)
$ErrorActionPreference='Stop'
$env:TEMP='E:\SecondSonPC_AI\temp';$env:TMP=$env:TEMP;$env:TMPDIR=$env:TEMP
$env:PYTHONDONTWRITEBYTECODE='1';$env:GIT_OPTIONAL_LOCKS='0';$env:GIT_NO_LAZY_FETCH='1'
$backup=Get-Content E:\SecondSonPC_AI\p415a\backup-path.txt
if(!(Test-Path "$backup\RESTORE-VERIFIED.txt")){throw 'Verified backup required'}
$root='E:\SecondSonPC'
$run="E:\SecondSonPC_AI\p415a\runs\$($Label)_$($Mode)_$($Scene)_$($Repeat)_"+[DateTime]::UtcNow.ToString('yyyyMMdd_HHmmssfff')
if(!([IO.Path]::GetFullPath($Exe).StartsWith('E:\',[StringComparison]::OrdinalIgnoreCase))){throw 'Binary must be on E:'}
New-Item -ItemType Directory -Path $run | Out-Null
foreach($n in 'bin','logs','userdata','driver-cache','settings','runtime\sspc\user'){New-Item -ItemType Directory -Force "$run\$n"|Out-Null}
foreach($id in 1000..1003){foreach($sub in 'savedata','trophy','inputs'){New-Item -ItemType Directory -Force "$run\runtime\sspc\user\home\$id\$sub"|Out-Null}}
Copy-Item -LiteralPath $Exe -Destination "$run\bin\SecondSonPC.exe"
$cache="E:\SecondSonPC_AI\p415a\caches\$($Label)_$Scene"
New-Item -ItemType Directory -Force $cache | Out-Null
$config=[IO.File]::ReadAllText("$root\SecondSonPC.toml")
$config=[regex]::Replace($config,'(?m)^userData\s*=.*$',"userData = '$run\userdata'")
$config=[regex]::Replace($config,'(?m)^logs\s*=.*$',"logs = '$run\logs'")
$config=[regex]::Replace($config,'(?m)^dir\s*=.*$',"dir = '$cache'")
[IO.File]::WriteAllText("$run\SecondSonPC.toml",$config,[Text.UTF8Encoding]::new($false))
$sources=@{beach="$root\logs\performance\p43_ab\snapshot\home";early="$root\userdata\savedata_backups\2026-10-10_03-44-58_before_manual_sspc";user1022="$root\logs\p32\scene_user_1022\home"}
if($Mode -eq 'game'){
 if(!$sources.ContainsKey($Scene) -or !(Test-Path -LiteralPath $sources[$Scene])){throw "Missing scene $Scene"}
 Copy-Item -LiteralPath $sources[$Scene] -Destination "$run\userdata\home" -Recurse
}
$env:__GL_SHADER_DISK_CACHE_PATH="$run\driver-cache"
$env:SSPC_P410_PARALLEL_CP=if($Parallel){'1'}else{'0'}
$env:SSPC_P49_BLOCK_UPLOAD=if($Block){'1'}else{'0'}
$env:SSPC_SHADER_STALL_TRACE=if($Mode -eq 'game'){'1'}else{'0'}
$env:SSPC_GPU_TIMINGS='off'
$argsList=@('--settings',"$run\settings\settings.toml",'--loader','sspc')
switch($Mode){
 'selftest' {$argsList+= '--selftest'}
 'probe' {$argsList+= '--gpu-probe'}
 'stability' {$argsList+= '--gpu-stability-test'}
 'game' {$argsList+=@('--gpu-backend','sspc','--user-data',"$run\userdata",'--shader-cache',$cache,'--frame-log','--vram-log','5','--press','space@12,space@22,move:-3000:0@56:0.25,move:-3000:0@58:0.25,move:-3000:0@60:0.25')}
}
if($Validate){$argsList+='--gpu-native-validate'}
$argsList+=$ExtraArgs
$sourceFiles=@(& git -c safe.directory=E:/SecondSonPC -C $root ls-files --cached --others --exclude-standard -- src cmake patches CMakeLists.txt SecondSonPC.toml)
$sourceManifest=foreach($rel in $sourceFiles){$p=Join-Path $root $rel;if(Test-Path -LiteralPath $p -PathType Leaf){[pscustomobject]@{Path=$rel;SHA256=(Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash}}}
$sourceManifest|Export-Csv "$run\live-source-sha256.csv" -NoTypeInformation
$builtManifest=Join-Path (Split-Path $Exe) 'source-sha256.csv'
if(Test-Path -LiteralPath $builtManifest){Copy-Item -LiteralPath $builtManifest -Destination "$run\source-sha256.csv"}else{Copy-Item "$run\live-source-sha256.csv" "$run\source-sha256.csv"}
& git -c safe.directory=E:/SecondSonPC -C $root status --short | Set-Content "$run\git-status.txt"
& git -c safe.directory=E:/SecondSonPC -C $root rev-parse HEAD | Set-Content "$run\git-head.txt"
$info=[ordered]@{label=$Label;mode=$Mode;scene=$Scene;repeat=$Repeat;args=$argsList;binary_sha256=(Get-FileHash "$run\bin\SecondSonPC.exe").Hash;source_manifest_sha256=(Get-FileHash "$run\source-sha256.csv").Hash;start_utc=[DateTime]::UtcNow.ToString('o');validation=[bool]$Validate;parallel=[bool]$Parallel;block=[bool]$Block;duration=$Seconds;driver_cache=$env:__GL_SHADER_DISK_CACHE_PATH;cache=$cache}
$info|ConvertTo-Json -Depth 4|Set-Content "$run\run.json"
try { Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,DriverDate | ConvertTo-Json | Set-Content "$run\driver.json" } catch { @{unavailable=$_.Exception.Message;fallback="Vulkan probe/application device log"} | ConvertTo-Json | Set-Content "$run\driver.json" }
$p=Start-Process -FilePath "$run\bin\SecondSonPC.exe" -ArgumentList $argsList -WorkingDirectory $run -WindowStyle $(if($Visible){'Normal'}else{'Hidden'}) -PassThru -RedirectStandardOutput "$run\stdout.txt" -RedirectStandardError "$run\stderr.txt"
$maxPrivate=0L;$maxWorking=0L;$sw=[Diagnostics.Stopwatch]::StartNew()
while(!$p.HasExited -and $sw.Elapsed.TotalSeconds -lt $Seconds){
 $p.Refresh();$maxPrivate=[math]::Max($maxPrivate,$p.PrivateMemorySize64);$maxWorking=[math]::Max($maxWorking,$p.WorkingSet64);Start-Sleep -Milliseconds 500
}
$shutdown=[Diagnostics.Stopwatch]::StartNew();$forced=$false
if(!$p.HasExited){$null=$p.CloseMainWindow();if(!$p.WaitForExit(20000)){$forced=$true;$p.Kill();$p.WaitForExit()}}
$p.WaitForExit();$p.Refresh()
$info.exit_code=$p.ExitCode;$info.forced=$forced;$info.elapsed_s=$sw.Elapsed.TotalSeconds;$info.shutdown_s=$shutdown.Elapsed.TotalSeconds;$info.private_max=$maxPrivate;$info.working_max=$maxWorking
$info|ConvertTo-Json -Depth 4|Set-Content "$run\run.json"
Write-Output "$run : exit $($p.ExitCode), forced $forced"
if($forced -or $p.ExitCode -ne 0){throw "Run did not exit successfully: $run"}
