@echo off
rem play.cmd plus experiments that are off for players until they are confirmed (docs/R9_REA3_FIX.md,
rem docs/R10_PACING.md):
rem   REA-3     - the game's second async compute wait of a frame done by the GPU,
rem   AQ        - the game's compute on a Vulkan queue of its own, beside the graphics,
rem   MAIN_FREE - the graphics queue waits only for the compute it depends on (R10).
rem R10 A/B (medians, FPS / 1 % low): owner's save 31.6 / 19.0 -> 37.6 / 19.7, fight 46.6 / 24.8 -> 53.0 / 26.9,
rem city 48.4 / 22.7 -> 50.7 / 23.2, beach the same; Neon Dash 38.9 / 18.9 -> 38.1 / 16.3 (worse: why it is not in
rem play.cmd). Harbour, village and a 20-minute run not tested.
set SSPC_PLAY_EXPERIMENTAL=1
call "%~dp0play.cmd" %*
