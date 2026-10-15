$ErrorActionPreference='Stop'
$env:TEMP='E:\SecondSonPC_AI\temp';$env:TMP=$env:TEMP;$env:TMPDIR=$env:TEMP
$pair='E:\SecondSonPC_AI\p415a\pairs\'+[DateTime]::UtcNow.ToString('yyyyMMdd_HHmmss')
New-Item -ItemType Directory -Path $pair|Out-Null
'isolated P415A cache test'|Set-Content "$pair\P415A-TEST-ONLY"
$env:SSPC_P415A_CACHE_ROOT=$pair
$env:SSPC_P415A_CACHE_ROLE='writer'
$argsList=@('-NoProfile','-File','E:\SecondSonPC_AI\p415a\run.ps1','-Exe','E:\SecondSonPC_AI\p415a\binaries\candidate1\SecondSonPC.exe','-Label','pair-writer','-Mode','stability','-Seconds','60')
$writer=Start-Process -FilePath (Get-Process -Id $PID).Path -ArgumentList $argsList -WindowStyle Hidden -PassThru -RedirectStandardOutput "$pair\writer.stdout" -RedirectStandardError "$pair\writer.stderr"
$deadline=[DateTime]::UtcNow.AddSeconds(25)
while(!(Test-Path "$pair\owner.ready") -and !$writer.HasExited -and [DateTime]::UtcNow -lt $deadline){Start-Sleep -Milliseconds 100}
if(!(Test-Path "$pair\owner.ready")){throw "Writer did not acquire cache: $pair"}
$env:SSPC_P415A_CACHE_ROLE='reader'
try { & E:\SecondSonPC_AI\p415a\run.ps1 -Exe E:\SecondSonPC_AI\p415a\binaries\candidate1\SecondSonPC.exe -Label pair-reader -Mode stability -Seconds 60 }
finally {$env:SSPC_P415A_CACHE_ROLE=$null;$env:SSPC_P415A_CACHE_ROOT=$null}
if(!$writer.WaitForExit(60000)){throw 'Writer did not finish'}
$writer.WaitForExit()
if($writer.ExitCode -ne 0){throw "Writer failed $($writer.ExitCode)"}
"PASS two processes $pair"|Tee-Object -FilePath "$pair\result.txt"

