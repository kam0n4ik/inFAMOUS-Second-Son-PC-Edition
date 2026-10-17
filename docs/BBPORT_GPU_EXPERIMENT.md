# BBPort GPU Integration Experiment (experiment/bbport-gpu)

Measured figures only. RTX 2060 6 GB, Ryzen 5 5500, 32 GB RAM without a page file, Windows 10 22H2; window 1296x759,
no upscaler, no frame generation.

## 1. Source

- BBPort: `E:\BBPort-GPU-Test` = github.com/Phroster/bloodborne_pc_windows, commit `b8e1071` (2026-10-07). Its GPU library
  vendors shadPS4 `b4e7ae7` (2026-10-02) + recompiler fixes (gpu/VENDOR.txt). Its README: the Windows fork "does not
  reach the first frame yet" (Bloodborne).
- Our changes to it: local branch `sspc-integration` (no push).
- SecondSonPC: worktree `E:\SecondSonPC-BBPort-Experiment`, branch `experiment/bbport-gpu` from `ec0ab8d`.

## 2. What is integrated

`SecondSonPC runtime -> GnmDriver/VideoOut/equeue imports -> bbgpu-sspc.dll (BBPort GPU) -> Vulkan`

- From BBPort, unchanged in design: GnmDriver, VideoOut, kernel event queues (all 98 GnmDriver/VideoOut imports of the
  game + 6 equeue functions bound to it, none stubbed), Liverpool command processor (PM4), shader recompiler,
  texture/buffer caches with its page tracking, Vulkan scheduler with its threaded recording (`parallel_gpu.md`),
  presenter and its own SDL window.
- From SecondSonPC: loader, kernel, memory manager (`--native memory`), threads, input, audio, files.
- The boundary (`gpu/sspc_host.h`, shared verbatim): the 20 `runtime_*` functions BBPort's GPU library takes from its own
  loader, given as a C table: guest memory on SecondSonPC's `VirtualMemory` (`write_backing` with BBPort's single-store
  labels, `region`, `clamp`, `vma_info`, `gpu_protect` never beyond the guest's protection), map/unmap/invalidate hooks
  driven by SecondSonPC's GPU memory listener, the clock. Access violations reach BBPort's page tracking first through
  the bridge's signal dispatch (`bbgpu_sspc_exception`: windows/compat context conversion with its siglongjmp recovery).
- Switch: `[Graphics] backend = "native" | "bbport"`, `--gpu-backend bbport`, `SSPC_GPU_BACKEND=bbport` (test runs);
  start-up only; needs `--native memory`. The DLL and its 99 MSYS2 dependencies (263 MB, mostly ffmpeg) live in
  `<exe>\bbport\`. BBPort's Bloodborne-specific upscaling is off (`BB_UPSCALER=off`, `BB_JITTER=0`,
  `BB_OBJECT_MOTION=0`); `BB_VBLANK_HZ=0` removes its 60 Hz cap. `SSPC_FLIP_LOG=1` times every flip call with either
  backend (`logs\flips.csv`).

## 3. Stages

| Stage | Result |
|---|---|
| BB-0 build | done: MSYS2 CLANG64 (`E:\msys64`), forced standard includes (libc++ 23), MinGW fixes for FSR-Vulkan. `bb-probe --vulkan-only`: PASS |
| BB-1 init in SecondSonPC | done: library loaded, window + Vulkan device, symbols bound, the game boots |
| BB-2 first frame | done: frames presented (24 640 flips in a 115 s run: menus and the village) |
| BB-3 scenes | village runs **without smoke and fire**; beach / harbour not measured |
| BB-4 stability | 115 s village run without a crash after the fixes below; nothing longer |

Fixes it took (branch `sspc-integration`):

1. `IsWriteError`/`GetRip` read `EXCEPTION_POINTERS` under `_WIN32`, but the Windows fork hands handlers its Linux-layout
   `ucontext_t` (page_manager.cpp reads `gregs[]`): write faults were taken for reads. Fixed under MinGW - a plausible
   cause of the fork's own missing first frame.
2. Two SDLs in one process: BBPort's window failed ("parameter is incorrect": class `SDL_app` is SecondSonPC's) -
   `SDL_RegisterApp` with a class of its own (host side).
3. Threaded recording asserted: the game flushed 16 384 image barriers at once (1.1 MiB of record data, chunks of
   128 KiB). Barriers and copy regions recorded in pieces of 4096; chunk 1 MiB.
4. A 640 MiB `HostUncached` staging buffer failed (the machine's commit limit): runs use `--memory-commit on-demand`.

## 4. Correctness

Not equivalent to the native backend:

- 62 textures not created: 1D block-compressed images (`Bc4UnormBlock` x45, `Bc5UnormBlock` x11, `Bc1Rgba*` x6: "type 1D
  is not supported"); the native backend handles them.
- `SanitizeCopyLayers: Coercing copy source layers 1 and destination layers 256` (repeated): texture array copies cut.
- Smoke and fire missing in the village (seen by the user). No screenshot diff: it would only measure the missing effects.

## 5. Performance (village, flip intervals `logs\bbport\bb_fps_village2\flips.csv`, warm shader cache, uncapped)

| | BBPort GPU | Native GPU (`logs/p53/cp9_ab3`, n = 3) |
|---|---|---|
| FPS | 19.3-20.1 (30-60 s), **25.1-25.7** (60-115 s) | **40.34** [39.93..40.85] |
| frame p50 / p95 ms | 41.0 / 59.8-63.5 | ~24.8 / 27.0 |
| VRAM (process) | **4889 of 5186 MiB budget** | ~1540 MiB |
| process private memory | **10.4 GB** (system commit 29.7 / 32.6 GB) | ~3.0 GB |
| access violations (CPU write tracking) | **~4 400 / s** | ~1 000 / s |

One BBPort run, not a 5-run A/B: the gap (25 vs 40 FPS, with effects missing on the BBPort side) is far outside the
run-to-run spread on this machine (< 3 %). Beach and harbour were not run on BBPort (the user stopped the series).

## 6. Why it is slower here

- **Memory model.** BBPort keeps video-memory copies of the guest's GPU memory (sparse arenas, "Garlic" in VRAM) and
  stages uploads through host memory. Bloodborne fits a 6 GB card; inFAMOUS maps ~5 GB of GPU-visible memory: VRAM ran
  at its budget (`GetArena: Migrating arena`), host memory at the commit limit. SecondSonPC's backend imports the guest's
  host memory and keeps only GPU-written ranges in VRAM (1.5 GB).
- **Write tracking** faults 4x as often (page protection over everything it copied).
- **Work not done:** missing textures and effects - slower while doing less.
- Its CPU side (threaded recording, draw preparation) cannot be judged alone here: the memory pressure dominates.

## 7. Decision

**Rejected as a backend** for this game on this hardware: the limit is its memory model, not the integration. Making it
fit 6 GB would mean rewriting its buffer cache into what the native backend already is. The integration stays in this
branch (inactive unless `bbport` is chosen) as a reproducible prototype.

Worth carrying into the native backend (from its `parallel_gpu.md`; not measured here): speculative draw preparation on
worker threads verified word by word against the registers; render-state / texture-set memos; recording in fixed
chunks of closures handed to recording threads.

## 8. Reproduce

1. MSYS2 CLANG64 in `E:\BBPort-GPU-Test` (branch `sspc-integration`): `bash windows/build.sh`,
   `ninja -C out/gpu bbgpu-sspc`, `bash out/deploy.sh`.
2. Build SecondSonPC from the worktree (cmake + ninja, target `shadps4`); copy `out\sspc-deploy\*` to `<exe>\bbport\`.
3. `p46_runs.ps1 -Scenes user1022 -ExtraArgs "--native memory --memory-commit on-demand" -Env
   "SSPC_CP_MODE=native;SSPC_FB_FRAME_LIMIT=unlimited;SSPC_GPU_BACKEND=bbport;SSPC_FLIP_LOG=1;BB_VBLANK_HZ=0" -Exe <exe>`.
   BBPort's FPS counter: `BB_CONFIG=<ini with show_fps=1>`; its log: `logs\bbport.log`. Two windows appear.
