$ErrorActionPreference='Stop'
$env:TEMP='E:\SecondSonPC_AI\temp';$env:TMP=$env:TEMP;$env:TMPDIR=$env:TEMP;$env:GIT_OPTIONAL_LOCKS='0'
$base='E:\SecondSonPC_Backups\P415A_20261012_234725'
$manifest=Import-Csv "$base\manifest-sha256.csv"
$checked=0
$failures=@()
foreach($row in $manifest){
 $path=$null
 if($row.Entry -in @('main/play.cmd','main/tools/manual_run.ps1')){$path=Join-Path E:\SecondSonPC $row.Entry.Substring(5)}
 elseif($row.Entry.StartsWith('task2/')){$path=Join-Path E:\SecondSonPC-task2 $row.Entry.Substring(6)}
 elseif($row.Entry.StartsWith('main/third_party/shadPS4/') -and $row.Entry -notmatch '/\.git(?:/|$)'){$path=Join-Path E:\SecondSonPC $row.Entry.Substring(5)}
 if($path){
  $checked++
  if(!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-FileHash -LiteralPath $path).Hash -ne $row.SHA256){$failures+=$path}
 }
}
$taskNow=@(& git -c safe.directory=E:/SecondSonPC-task2 -C E:\SecondSonPC-task2 status --porcelain=v1 --untracked-files=all --ignored)
$taskThen=@(Get-Content "$base\task2-status.txt")
$stateEqual=($taskNow -join "\n") -eq ($taskThen -join "\n")
[ordered]@{checked_files=$checked;different=$failures;task2_status_unchanged=$stateEqual;game_mount_read_only=$true;game_full_hash_before_unavailable=$true}|ConvertTo-Json -Depth 3|Set-Content E:\SecondSonPC_AI\p415a\preservation.json
if($failures.Count -or !$stateEqual){throw 'Preservation mismatch: see preservation.json'}
"PASS $checked protected/nested/task2 file hashes; task2 status unchanged"

