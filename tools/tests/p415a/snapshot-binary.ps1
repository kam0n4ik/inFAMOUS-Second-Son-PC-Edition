param([Parameter(Mandatory)][string]$Label)
$ErrorActionPreference='Stop'
$root='E:\SecondSonPC';$dest="E:\SecondSonPC_AI\p415a\binaries\$Label"
if(Test-Path $dest){throw 'Binary snapshot exists'}
New-Item -ItemType Directory -Path $dest|Out-Null
Copy-Item E:\SecondSonPC_AI\build\p415a\bin\SecondSonPC.exe $dest
$files=@(& git -c safe.directory=E:/SecondSonPC -C $root ls-files --cached --others --exclude-standard -- src cmake patches CMakeLists.txt SecondSonPC.toml)
$rows=foreach($rel in $files){$p=Join-Path $root $rel;if(Test-Path -LiteralPath $p -PathType Leaf){[pscustomobject]@{Path=$rel;SHA256=(Get-FileHash -LiteralPath $p).Hash}}}
$rows|Export-Csv "$dest\source-sha256.csv" -NoTypeInformation
Get-FileHash "$dest\SecondSonPC.exe"|Format-List|Out-String|Set-Content "$dest\binary-sha256.txt"
& git -c safe.directory=E:/SecondSonPC -C $root diff --binary --full-index "--output=$dest\source.patch"
& git -c safe.directory=E:/SecondSonPC -C $root rev-parse HEAD|Set-Content "$dest\head.txt"
Write-Output $dest