param([switch]$Configure,[string]$Label='baseline')
$ErrorActionPreference='Stop'
$backup=Get-Content E:\SecondSonPC_AI\p415a\backup-path.txt
if(!(Test-Path "$backup\RESTORE-VERIFIED.txt")){throw 'Verified backup required'}
. E:\SecondSonPC\tools\env.ps1
$env:TEMP='E:\SecondSonPC_AI\temp';$env:TMP=$env:TEMP;$env:TMPDIR=$env:TEMP
$env:GIT_OPTIONAL_LOCKS='0';$env:GIT_NO_LAZY_FETCH='1'
$env:GIT_CONFIG_COUNT='3';$env:GIT_CONFIG_KEY_2='safe.directory';$env:GIT_CONFIG_VALUE_2='E:/SecondSonPC/*';$env:GIT_CONFIG_KEY_0='safe.directory';$env:GIT_CONFIG_VALUE_0='E:/SecondSonPC';$env:GIT_CONFIG_KEY_1='safe.directory';$env:GIT_CONFIG_VALUE_1='E:/SecondSonPC/third_party/shadPS4'
$env:PYTHONDONTWRITEBYTECODE='1';$env:PIP_CACHE_DIR='E:\SecondSonPC_AI\cache\pip';$env:CCACHE_DIR='E:\SecondSonPC_AI\cache\ccache'
$bld='E:\SecondSonPC_AI\build\p415a';$logs='E:\SecondSonPC_AI\p415a\build-logs'
New-Item -ItemType Directory -Force $bld,$logs | Out-Null
@(& cmake --version;& ninja --version;& clang-cl --version;& git --version) | Set-Content "$logs\$Label-tools.txt"
if($Configure -or !(Test-Path "$bld\build.ninja")){
 & cmake -S E:\SecondSonPC -B $bld -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER_TYPE=LLD '-DCMAKE_RC_COMPILER=rc.exe' '-DCMAKE_MT=mt.exe' -DCMAKE_INSTALL_PREFIX=E:/SecondSonPC_AI/install -DFETCHCONTENT_FULLY_DISCONNECTED=ON -DFETCHCONTENT_UPDATES_DISCONNECTED=ON -DFETCHCONTENT_SOURCE_DIR_FMT=E:/SecondSonPC/build/_deps/fmt-src -DFETCHCONTENT_SOURCE_DIR_ZLIB=E:/SecondSonPC/third_party/shadPS4/externals/zlib-ng *> "$logs\$Label-configure.txt"
 if($LASTEXITCODE){throw "Configure failed: $logs\$Label-configure.txt"}
}
& cmake --build $bld --target shadps4 sspc_settings_tests --parallel 6 *> "$logs\$Label-build.txt"
if($LASTEXITCODE){throw "Build failed: $logs\$Label-build.txt"}
Get-FileHash "$bld\bin\SecondSonPC.exe" -Algorithm SHA256 | Format-List | Out-String | Set-Content "$logs\$Label-binary.txt"
Write-Output "BUILD PASS $Label"