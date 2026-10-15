$ErrorActionPreference='Stop'
$env:TEMP='E:\SecondSonPC_AI\temp';$env:TMP=$env:TEMP;$env:TMPDIR=$env:TEMP
$env:SSPC_SHADER_PRECOMPILE='1'
$log='E:\SecondSonPC_AI\p415a\ab-progress.txt'
foreach($scene in 'beach','early','user1022'){
 foreach($repeat in 1..3){
  $variants=if($repeat % 2 -eq 1){@('baseline','candidate1')}else{@('candidate1','baseline')}
  foreach($variant in $variants){
   $label="ab1-$variant"
   $done=@(Get-ChildItem E:\SecondSonPC_AI\p415a\runs -Directory -Filter "${label}_game_${scene}_${repeat}_*" | Where-Object {
     $file=Join-Path $_.FullName 'run.json'
     if(Test-Path $file){$j=Get-Content $file -Raw|ConvertFrom-Json; $null -ne $j.exit_code -and $j.exit_code -eq 0 -and !$j.forced -and $j.elapsed_s -ge 90}
   })
   if($done.Count){continue}
   "$(Get-Date -Format o) START/RESUME $scene $repeat $variant"|Add-Content $log
   try {
    & E:\SecondSonPC_AI\p415a\run.ps1 -Exe "E:\SecondSonPC_AI\p415a\binaries\$variant\SecondSonPC.exe" -Label $label -Mode game -Scene $scene -Seconds 90 -Repeat $repeat -Visible | Add-Content $log
   } catch { "$_"|Add-Content $log; throw }
  }
 }
}
"DONE $(Get-Date -Format o)"|Add-Content $log

