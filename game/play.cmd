@echo off
rem inFAMOUS: Second Son - PC Edition. Lives in the game's folder (the one with eboot.bin) next to SecondSonPC.exe.
rem Starts the game with every measured improvement that is on for players (README.md, Performance):
rem the native command processor preset, the city preset, the second Vulkan recording thread and its batching,
rem F1 (no pipeline cache freezes), F3 (no vertex heap re-imports) and R10's own audio and system libraries.
rem Saves: userdata\ here, copied to userdata\savedata_backups\<date_time> before every start.
rem Arguments are passed on to SecondSonPC.exe.
cd /d "%~dp0"
if not exist "%~dp0eboot.bin" (
    echo eboot.bin is not in this folder - put SecondSonPC into the game's folder ^(README.md, Installation^).
    pause
    exit /b 1
)
rem Windows' own find by its path: with Git's Unix tools in PATH a bare "find" is the Unix one
"%SystemRoot%\System32\tasklist.exe" /fi "imagename eq SecondSonPC.exe" 2>nul | "%SystemRoot%\System32\find.exe" /i "SecondSonPC.exe" >nul
if not errorlevel 1 (
    echo SecondSonPC.exe is already running.
    pause
    exit /b 1
)
set SSPC_CP_MODE=native
set SSPC_CITY_PERFORMANCE=1
set SSPC_REA_MERGE_RUNS=1
set SSPC_CP_ROUND_NODRAIN=1
set SSPC_P411_ASYNC_ACQUIRE=1
set SSPC_ASYNC_PIPELINES=1
set SSPC_P53_FRONTEND_THREAD=1
set SSPC_CP_ASYNC_WRITES=1
set SSPC_CP_ASYNC_FLIP=1
set SSPC_VK_EMIT_THREAD=1
set SSPC_VK_EMIT_BATCH=1
set SSPC_CP_RING=128
set SSPC_NATIVE_GBUFFER=native
set SSPC_R9_PCACHE=1
set SSPC_R9_HEAP_WINDOW=1
rem R10: libSceAjm (ATRAC9 audio, docs/R10_AJM.md) and CommonDialog/libSceNet/libc (docs/R10_SYSRT.md) on
rem SecondSonPC's own code; bridge is the way back for either
set SSPC_R10_AJM=native
set SSPC_R10_SYSRT=native
if defined SSPC_PLAY_EXPERIMENTAL (
    rem play_experimental.cmd: REA-3 and AQ (docs/R9_REA3_FIX.md) with R10's main queue free of the lane's
    rem unrelated batches (docs/R10_PACING.md) - faster and even, but Neon Dash has a lower 1-percent low
    set SSPC_REA_DEFER_WAIT_B=1
    set SSPC_FB_ASYNC_COMPUTE=1
    set SSPC_R10_AQ_MAIN_FREE=1
)
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set STAMP=%%i
if exist "%~dp0userdata\home" if defined STAMP xcopy /e /i /q /y "%~dp0userdata\home" "%~dp0userdata\savedata_backups\%STAMP%" >nul
start "" /wait /d "%~dp0" "%~dp0SecondSonPC.exe" %*
