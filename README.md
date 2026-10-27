# inFAMOUS: Second Son — PC Edition

**inFAMOUS: Second Son on Windows, on a PC runtime of its own.**

The original PlayStation 4 game runs on Windows on SecondSonPC; its executable and data are left unchanged. SecondSonPC brings its own loader, kernel, memory manager and system services, its own GPU command processor, Vulkan 1.3 backend and GCN → SPIR-V shader translator, and native replacements of selected functions of the game's engine. The game's own x86-64 code runs directly on the PC's processor: there is no CPU emulation.

> **Status: in development, playable.** The game starts, loads saves and is played through. On the development PC (Ryzen 5 5500, RTX 2060 6 GB), gameplay in the city runs at 48–50 FPS, the beach at 66 FPS and a heavy save over the waterfront at 33 FPS; with R12's experimental switches that save runs at 42 FPS and the beach at 72 ([Benchmarks](#benchmarks)).

<p align="center">
  <img src=".github/readme/pc-edition-banner.png" alt="inFAMOUS: Second Son — PC Edition" width="100%">
</p>

<p align="center">
  <a href="#installation">Installation</a> ·
  <a href="#requirements">Requirements</a> ·
  <a href="#what-makes-it-different">Advantages</a> ·
  <a href="#online-missions-add-ons-and-trophies">Online, DLC, trophies</a> ·
  <a href="cut-content/README.md">Cut content</a> ·
  <a href="#benchmarks">Benchmarks</a> ·
  <a href="#cpu-native-x86-64-and-avx">CPU and AVX</a> ·
  <a href="#building-from-source">Building</a> ·
  <a href="#where-the-project-is">Status</a> ·
  <a href="#launcher">Launcher</a> ·
  <a href="#pc-settings">PC settings</a> ·
  <a href="#known-issues">Known issues</a> ·
  <a href="#reports">Reports</a> ·
  <a href="#infamous-first-light--pc-edition">First Light</a>
</p>

---

## inFAMOUS: First Light — PC Edition

<a href="https://github.com/kam0n4ik/inFAMOUS-First-Light-PC-Edition"><img src="https://raw.githubusercontent.com/kam0n4ik/inFAMOUS-First-Light-PC-Edition/main/.github/readme/first-light-banner.jpg" alt="inFAMOUS: First Light — PC Edition" width="100%"></a>

The standalone add-on **inFAMOUS: First Light** (CUSA00897) runs on this same runtime: **[inFAMOUS-First-Light-PC-Edition](https://github.com/kam0n4ik/inFAMOUS-First-Light-PC-Edition)**. Both games share one engine, so First Light is a second product of the same code (`SSPC_PRODUCT=FirstLight`), with its own import table, system libraries, engine hook addresses, button prompts, settings and saves; a fix in the shared runtime reaches both games. It is early: the game reaches gameplay, and Second Son's engine-specific optimisations are not mapped to it yet. Download it from its [Releases](https://github.com/kam0n4ik/inFAMOUS-First-Light-PC-Edition/releases).

## What makes it different

- **The game's code runs natively on your CPU.** The PS4's processor is an x86-64 AMD Jaguar, so the game's executable is ordinary x86-64 code: 2.3 million instructions, 397 000 of them AVX. SecondSonPC loads it like a native program. There is no interpreter, no JIT and no binary translation, and every instruction runs at the CPU's full speed. Only what touches PS4-specific state is patched once at load time: thread-pointer (`fs:`) accesses (3 616 sites) and, on Intel CPUs, AMD's SSE4a ([CPU: native x86-64 and AVX](#cpu-native-x86-64-and-avx)).
- **A GPU path built for a PC processor.** The game's PM4 command buffers go through SecondSonPC's command processor, which is spread over four threads:
  - packet parsing;
  - draw preparation;
  - Vulkan recording;
  - Vulkan submission.

  Then:
  - The 5 600–12 000 draws of a frame are pipelined across cores.
  - Runs of draws that differ only in geometry are recorded as one unit (CITY-OPT).
  - A frame is handed over as soon as the game finishes it.
  - Memory waits whose condition always holds skip the guest read.
- **No shader-compile stutter after the first start.**
  - The game's shaders are predicted from its own pack files and compiled once, before the game starts.
  - Translated SPIR-V and the driver's pipeline cache are kept on disk.
  - The pipeline cache is saved without blocking the renderer (R9 F1), which removed 1–5 s freezes once a minute.
- **Free frame pacing.** Frame pacing is not tied to the PS4's 59.94 Hz vblank. The frame-rate limit is 30, 40, 50, 60, 90, 120, 144, 165, 240 or unlimited, with vertical sync on or off and both switched without a restart, and the game's clock was measured to be independent of FPS. The 1 % lows went from 29 to 47–49 FPS on the beach and in the harbour.
- **DLSS, FSR 3.1, FSR 3.1 frame generation and NVIDIA Reflex inside the game's frame.** Motion vectors, depth and jitter are taken from the game. The HUD is drawn afterwards at full resolution.
- **The engine's own functions, rebuilt and proven.** 18 render functions of the engine (G-buffer and shadow passes, materials, the constant update engine) have SecondSonPC implementations. They are hand-written or produced by the project's static recompiler (Ghidra P-code → C++, AVX), and each is verified call by call against the original in the running game: 0 mismatches over tens of millions of calls.
- **PC-native around the game.**
  - A launcher opens before the game: every PC setting, the launch profile and Play ([Launcher](#launcher)). The same settings live inside the game's own menu.
  - Keyboard and mouse are supported, and every button prompt is redrawn for keyboard, Xbox or PlayStation.
  - The mouse turns the camera directly, as in a PC game: no stick emulation, dead zone, acceleration or speed cap ([Native mouse camera](#native-mouse-camera)).
  - Any resolution and aspect ratio from 4:3 to 32:9, with Hor+, an unstretched HUD, an FOV setting and movies kept 16:9 ([Resolutions and aspect ratios](#resolutions-and-aspect-ratios)).
  - Audio goes out through WASAPI; cutscene video is decoded natively.
  - `play.cmd` backs up your saves before every start.
- **What PSN gave, without PSN.** The game is signed in from the start (as `t.me/cloudeluxe`), and the shut-down Paper Trail server is answered locally, so the online Paper Trail missions open. The pre-order and promotion add-ons on the disc are unlocked (Cole's Legacy: Cole's jacket and its missions; the Penny Arcade, Exist, Jos, Skeleton and Handprint vests). Trophies are earned and kept on your PC, with a notice and the platinum ([Online missions, add-ons and trophies](#online-missions-add-ons-and-trophies)).
- **No PS4 firmware.** The system libraries the game imports are implemented by the project (336 of 417 natively, the rest from the shadPS4 core linked in). Only your own copy of the game is needed.
- **Measured, not guessed.** Every change is decided by an interleaved A/B on the same build, and experiments that lost are documented too: [135 reports](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-20261023/docs/REPORTS.md).

## Installation

**This project does not distribute the game, its executable, assets or any copyrighted content.** You need your own, legally obtained copy of inFAMOUS: Second Son and must prepare its files yourself.

1. **The game's folder.** You need a decrypted dump of your copy of **CUSA00004** with **update 01.07** applied: one folder that holds `eboot.bin`, `sce_module`, `sce_sys` and `art` (about 30 GB). The European release is the one that was tested; its folder has the marker file `++ retail 0212.1422 i3_eu ship ++`. No PS4 firmware files are needed.
2. **SecondSonPC.** Download `SecondSonPC-win64.zip` from [Releases](../../releases), or [build it](#building-from-source), and copy everything in it **into the game's folder**, next to `eboot.bin`. None of the game's files are replaced. The zip contains:

   | File | What |
   |---|---|
   | `SecondSonPC.exe` | the runtime; started on its own it opens the [launcher](#launcher) first |
   | `play.cmd` | starts the game at once, without the launcher, with every improvement that is on for players, after backing up the saves |
   | `play_experimental.cmd` | `play.cmd` plus faster experiments (REA-3 + AQ + R10's MAIN_FREE + R11's AQ_IMAGES and AQ_ORDER_EPOCH, [R9_REA3_FIX.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-20261023/docs/R9_REA3_FIX.md), [R10_PACING.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-r10-20261024/docs/R10_PACING.md), [R11_NEON.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-r11-20261025/docs/R11_NEON.md)): +17 % FPS on the owner's save and +9 % in a fight against `play.cmd`, but a lower 1 % low in Neon Dash |
   | `SecondSonPC.toml` | defaults: window, language, keys, PC settings |
   | `data\static_templates.txt` | the data the shader preparation predicts the game's shaders from |
   | `sspc_dlss.dll`, `nvngx_dlss.dll`, `nvngx_dlssg.dll`, `sl.*.dll`, `NvLowLatencyVk.dll` | NVIDIA DLSS 310.9.1, Streamline 2.14.1, Reflex |
   | `amd_fidelityfx_vk.dll` | AMD FidelityFX SDK 1.1.4 (FSR 3.1 upscaling and frame generation) |
   | `fonts\`, `licenses\` | the menu fonts and the vendors' licences |
   | `SecondSonPC-README.txt` | these steps in short |

3. **The Visual C++ runtime.** Install the [Microsoft Visual C++ 2015–2022 Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe) if it is not installed yet.
4. **Start `SecondSonPC.exe`** and press **Играть** (Play) in the launcher. `play.cmd` still works and starts the game at once with the standard profile.
   - On the first start a console window prepares the game's shaders: they are compiled before the game starts. This happens once: on the development PC (6 cores / 12 threads) it took 5.5 minutes from an empty cache, about 13 400 pipelines. Later starts skip it in under two seconds.
   - It runs again only after a driver or SecondSonPC update.
5. **PC settings:** in the launcher, or in the game: **Esc (Options) → Settings → "Additional settings"**.

Where things are kept:

| Path | What |
|---|---|
| `userdata\` (in the game's folder) | saves; `play.cmd` copies them to `userdata\savedata_backups\<date_time>` before every start |
| `%LOCALAPPDATA%\SecondSonPC\settings.toml` | your changes from the launcher and the in-game settings window, one file for both (they override `SecondSonPC.toml`) |
| `%LOCALAPPDATA%\SecondSonPC\launcher.toml` | the launcher's own choices: launch profile, close after Play |
| `cache\shaders\` | prepared shaders and the driver's pipeline cache (about 1.6 GB; safe to delete, it is prepared again) |
| `logs\SecondSonPC.log` | the log; attach it to a bug report |
| `logs\sessions\<date_time>\` | every slow frame of a session (50 ms or more), when and why: `hitches.txt`, per second `seconds.csv`, `summary.txt` at the exit; the newest 30 sessions are kept. Press **F12** when you see a stutter to mark it there |

To keep SecondSonPC outside the game's folder, set `[Game] path` in `SecondSonPC.toml` to the game's folder instead of `"."`.

## Requirements

| | Required | Tested |
|---|---|---|
| OS | Windows 10 or 11, 64-bit | Windows 10 Pro 22H2 (19045) |
| CPU | x86-64 with SSE2–SSE4.2, POPCNT, **AVX**, F16C, BMI1, LZCNT, MOVBE: any AMD Ryzen, or an Intel Core of the 4th generation (Haswell) or later. Pentium and Celeron models without AVX cannot run it. SecondSonPC checks this at start and names what is missing. 6 cores / 12 threads recommended. | AMD Ryzen 5 5500 (6 cores / 12 threads) |
| GPU | Vulkan 1.3 with dynamic rendering, push descriptors, robustness2, a vertex attribute divisor, tessellation shaders, swapchain maintenance 1 | NVIDIA GeForce RTX 2060 6 GB, driver 596.21 |
| VRAM | 6 GB | 6 GB |
| RAM | 32 GB was tested, smaller amounts were not; keep the Windows page file on (the game maps several GB of guest memory) | 32 GB |
| Disk | the game (~30 GB) + about 1.6 GB for the shader cache | — |
| DLSS | NVIDIA GeForce RTX; DLSS frame generation is not implemented | RTX 2060 |
| FSR 3.1 | any GPU above | RTX 2060 |

AMD and Intel GPUs have not been tested. An Intel CPU has not been tested either; see [Known issues](#known-issues) for its expected cost.

## Benchmarks

All measurements are interleaved A/B series on one build, usually 3–6 runs per side with the median shown. The stand is a Ryzen 5 5500, RTX 2060 6 GB and 32 GB RAM, with the game rendering at 1920 × 1080 internally. The test scenes:

| Scene | What |
|---|---|
| fight / traverse | live gameplay in the city: powers, or running and dashing with sharp camera turns (R9 routes) |
| beach | heavy foliage |
| harbour | the docks |
| village | fires and smoke |
| live city / city bench | the open city from a save; the bench route opens the map at 75 s |
| owner's save | a rooftop over the waterfront with traffic |

<picture>
  <source media="(prefers-color-scheme: dark)" srcset=".github/readme/current-dark.svg">
  <img alt="Average FPS and 1 % low by scene" src=".github/readme/current.svg" width="880">
</picture>

The chart above is from 10-23; the table has the current numbers. "R12 exp." is `play.cmd` plus `SSPC_FB_LAZY_WB=1` and `SSPC_REA_HEAP_READS=1` (experimental, see [What is new](#what-is-new)).

| Scene | `play.cmd` FPS | 1 % low | R12 exp. FPS | 1 % low | Date, runs | Source |
|---|---:|---:|---:|---:|---|---|
| Traverse (city) | 47.8 | 20.5 | 50.1 | 24.2 | 10-26, 3 | [R12_LIGHTS](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-r12-20261026/docs/R12_LIGHTS.md), series `ab3` |
| Fight (city) | 49.0 | 26.6 | 48.8 | 25.1 | 10-26, 3 | same |
| Owner's save (dusk, wet) | 33.1 | 27.9 | 41.8 | 33.1 | 10-26, 3 | same |
| Beach | 65.7 | 50.7 | 71.5 | 58.3 | 10-26, 2 | [R12_LIGHTS](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-r12-20261026/docs/R12_LIGHTS.md), series `ab2` |
| Neon Dash (rain at night) | 50.2 | 22.0 | 55.2 | 18.7 | 10-26, 3 | same; the route's spread is 46–59 FPS |
| Rain by day, standing | 42.6 | 37.6 | – | – | 10-26, 3 | [R12_WEATHER](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-r12-20261026/docs/R12_WEATHER.md) §3, series `ab4` |
| Harbour | 43.2 | 32.6 | – | – | 10-22, 3 | [NATIVE_GBUFFER_DRAW_0x1a1770](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-20261023/docs/NATIVE_GBUFFER_DRAW_0x1a1770.md) §9.10 (before emitter batching, F1, F3) |
| Village | 37.6 | 30.2 | – | – | 10-22, 3 | same |

With the particle sort on top (`SSPC_R12_PSORT=1`) the owner's save went 38.5 → 42.3 FPS in a series run under background load; it is being measured again.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset=".github/readme/fps-ab-dark.svg">
  <img alt="Average FPS before and after each measured change" src=".github/readme/fps-ab.svg" width="880">
</picture>

<picture>
  <source media="(prefers-color-scheme: dark)" srcset=".github/readme/low1-ab-dark.svg">
  <img alt="1 % low FPS before and after each measured change" src=".github/readme/low1-ab.svg" width="880">
</picture>

The worst frames are where the gameplay work (R9) shows most:
- traverse: max frame 4 645 → 156 ms with F1;
- fight: max frame 3 166 → 82 ms with F1 + F3.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset=".github/readme/cp-modes-dark.svg">
  <img alt="Average FPS by command processor mode" src=".github/readme/cp-modes.svg" width="880">
</picture>

<picture>
  <source media="(prefers-color-scheme: dark)" srcset=".github/readme/upscaling-dark.svg">
  <img alt="Average FPS by upscaling mode" src=".github/readme/upscaling.svg" width="880">
</picture>

<picture>
  <source media="(prefers-color-scheme: dark)" srcset=".github/readme/shader-cache-dark.svg">
  <img alt="Frames over 100 ms on a first and on a repeat start" src=".github/readme/shader-cache.svg" width="880">
</picture>

**Where the frame time goes.** In gameplay the game's own work on its main thread is flat at about 4.3 ms a frame. The rest of the frame waits for its async compute and for the GPU: in a fight the GPU needs ~17.5 ms and the command processor ~16.5 ms of a ~21 ms frame. In the open city, ~12 000 draws a frame make command processing and Vulkan recording the limit. Upscaling therefore helps little, and frame generation helps most: on the beach, 43–53 game frames become 86–105 shown.

Not counted: no quality setting, light or shadow was lowered for any number here. The chart data and the script that draws them are in [`make_charts.py`](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-20261023/docs/benchmarks/make_charts.py); every report behind them is listed in [`REPORTS.md`](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-20261023/docs/REPORTS.md) (tag `archive/docs-20261023`).

## CPU: native x86-64 and AVX

The PS4's CPU is an 8-core AMD Jaguar, an x86-64 processor. The game's executable was compiled for it, so it is valid code for a PC processor too. SecondSonPC maps it into memory at its PS4 addresses, resolves its imports to its own system libraries and jumps into it, and from then on the game's code runs on the PC's cores like any program's. What the game's modules use (decoded function by function, [PHASE3_P34_EXCEPTIONS_CPU.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-20261023/docs/PHASE3_P34_EXCEPTIONS_CPU.md) §5):

| Module | Functions | Instructions | Instruction sets |
|---|---:|---:|---|
| `eboot.bin` | 42 483 | 2 313 273 | **AVX 396 817**, BMI 1 365, SSE2, SSE4a 96, F16C 13; `vrsqrtps` 2 699, `vrcpps` 1 684, `popcnt` 132, `lzcnt` 129, `movbe` 16 |
| `libc.prx` | 2 442 | 163 524 | AVX, BMI, `lzcnt`, `movbe` |
| `libSceFios2.prx` | 730 | 71 274 | AVX, BMI |

What follows from that:

- **The host CPU needs every set the game uses:** SSE2–SSE4.2, POPCNT, MOVBE, AVX (with the OS saving the `ymm` registers), F16C, BMI1 and LZCNT. `Cpu::HostMeetsGameRequirements` checks them before the game starts and names the missing ones.
- **AVX runs as AVX.** The game's 397 000 AVX instructions are executed as they are, with no emulation or splitting. `vrsqrtps` and `vrcpps` give the result of the host's CPU, which differs from the Jaguar's only in the last bits of an approximation, as it does between PC CPUs.
- **SSE4a** (`extrq`) exists only on AMD CPUs, and Ryzen has it. On Intel, SecondSonPC's CPU patcher replaces the 96 `extrq` of the game: 23 are rewritten at load time to jump into a trampoline, and the 73 that are too short for a jump are emulated when they fault ([KNOWN_ISSUES.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-20261023/docs/KNOWN_ISSUES.md) SSPC-011).
- **Thread pointer.** The PS4 keeps the thread pointer in `fs:`; Windows does not. The 3 616 `fs:` accesses are patched to SecondSonPC's per-thread slot at load time.
- **The rebuilt engine functions speak AVX too.** The native reconstructions of the engine's functions are compiled with `/arch:AVX` and `-ffp-contract=off`. Critical vector operations are inline assembly with the original's operand order and a 3-byte VEX prefix (`{vex3}`), because x86 keeps the NaN payload of the *first* operand and LLVM swaps commutative operands for a shorter encoding. Without that, 5–11 thousand results in 200 000 differed in one NaN bit, and so did the commands the engine sends to the GPU.
- **The static recompiler v2** lifts the engine's x86-64 with Ghidra's P-code into C++ that is compiled with `/arch:AVX`. AVX and floating-point operations become the same processor instructions, under the current MXCSR. It is checked against the processor itself: 654 instructions × 1 000 states × 5 MXCSR modes, 3.27 million runs with 0 differences. A recompiled function is checked offline (up to 1 million generated cases) before it may be installed ([NATIVE_ENGINE_RECONSTRUCTION.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-20261023/docs/NATIVE_ENGINE_RECONSTRUCTION.md) §9.1).
- **Hash-pinned.** A rebuilt function names its address in `eboot.bin` and the hash of its original body. On any other executable it is not installed, and the original code runs.

## Building from source

This repository holds only what builds SecondSonPC: `src/`, `cmake/`, `patches/`, `assets/`, `third_party/spirv_headers`, `CMakeLists.txt` and `build.ps1`; plus the files that go into the game's folder, in `game/`. The finished build is on the [Releases](../../releases) page. The reports are kept at the tag [`archive/docs-20261023`](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/tree/archive/docs-20261023/docs). The development scripts and the measurement stand that used to live in `tools/` are kept at the tag [`archive/dev-tools-20261023`](../../tree/archive/dev-tools-20261023). The offline tests are kept at the tag [`archive/tests-20261023`](../../tree/archive/tests-20261023/tests): the reconstructed engine functions and the recompiler checked against the game's `eboot.bin` (the numbers quoted above), and the PC settings.

### What you need

| Tool | Version | Note |
|---|---|---|
| Git | any recent | `build.ps1` clones shadPS4 with its submodules (~4 GB) |
| Visual Studio 2022 or Build Tools 2022 | workload *Desktop development with C++*, with a Windows 11 SDK (10.0.26100 is used) | the MSVC STL and the SDK's headers and libraries; also brings CMake and Ninja |
| LLVM | **20** or later (20.1.8 is used) | `clang-cl` and `lld-link`; MSVC's `cl.exe` is not supported |
| CMake | 3.24 or later | the one of Visual Studio is fine |
| Ninja | any recent | the one of Visual Studio is fine |
| Disk | ~8 GB | sources, shadPS4 with submodules, the build |

### Steps

```powershell
git clone https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition.git
cd inFAMOUS-Second-Son-PC-Edition
powershell -ExecutionPolicy Bypass -File build.ps1
```

`build.ps1` does all of it:

1. It clones shadPS4 into `third_party\shadPS4` at the pinned commit `0fe263a4760d` with its submodules. The CMake configure step applies the project's 34 patches from `patches\shadps4` to it.
2. It sets up the compiler environment. In a *Developer PowerShell* the current one is used; otherwise `build.ps1` runs `vcvars64.bat` of the newest Visual Studio found by `vswhere` (or `-VsPath`).
3. It configures with Ninja, `clang-cl` and LLD and builds the target `shadps4`, which produces `build\bin\SecondSonPC.exe`. `clang-cl` comes from `-LlvmPath`, `PATH`, `C:\Program Files\LLVM\bin` or Visual Studio's LLVM.
4. It packs `dist\SecondSonPC\`, everything that goes into the game's folder, and `dist\SecondSonPC-win64.zip` of the same.

A first build takes about 12 minutes on 12 threads. Options: `-Config RelWithDebInfo|Release|Debug`, `-Fresh` (configure again), `-NoPackage`. The log is `build\build.log`.

### DLSS and FSR (optional)

The vendors' SDKs are not part of the sources. Without them the build works, with no DLSS / FSR (the settings say why). For upscaling, put them into `third_party\upscalers` before the build:

```text
third_party\upscalers\
  ngx\include\nvsdk_ngx*.h      NVIDIA DLSS SDK 310.9.1: its headers
  ngx\lib\nvsdk_ngx_s.lib       its static NGX library (x64)
  ngx\LICENSE.txt
  ffx-api\include\ffx_api\*.h   AMD FidelityFX SDK 1.1.4: the FidelityFX API headers
  ffx-api\LICENSE.txt
  streamline\include\sl*.h      NVIDIA Streamline 2.14.1: its headers
  streamline\license.txt
  bin\                          the runtime DLLs: nvngx_dlss.dll, nvngx_dlssg.dll (DLSS SDK),
                                amd_fidelityfx_vk.dll (FidelityFX SDK, signed prebuilt),
                                sl.common.dll, sl.dlss_g.dll, sl.interposer.dll, sl.pcl.dll, sl.reflex.dll,
                                NvLowLatencyVk.dll (Streamline)
```

CMake picks up what it finds. `sspc_dlss.dll` is the only binary that links NGX; `SecondSonPC.exe` loads it at run time.

## Where the project is

### PlayStation functions the game imports

The game imports 417 system functions. Their owner in the current build (snapshot [`generated/CALLS.md`](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-20261023/docs/generated/CALLS.md)):

| Owner | Functions | Called in a 2-minute gameplay run |
|---|---:|---:|
| **Native (SecondSonPC)** | **336** | 163 |
| shadPS4 (bridge) | 61 | 8 |
| shadPS4 common stub | 20 | 1 |

R10 moved the rest of what the game calls onto SecondSonPC's own code, and `play.cmd` turns it on:
- `SSPC_R10_SYSRT=native`: CommonDialog, libSceNet and the libc functions the game calls (`sceLibcHeapGetTraceInfo`, `fflush`, `vprintf`). The owners of the 417 imports go from 338 / 59 / 18 / 2 (native / bridge / common stub / unresolved) to 350 / 49 / 17 / 1, and the imports called in a normal run that SecondSonPC does not own go from 9 to 2, both libSceAjm's ([R10_SYSRT.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-r10-20261024/docs/R10_SYSRT.md)).
- `SSPC_R10_AJM=native`: all 11 libSceAjm imports, with ATRAC9 decoded by LibAtrac9 (MIT); 15 192 frames decode the same as with shadPS4 ([R10_AJM.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-r10-20261024/docs/R10_AJM.md)). The game itself only calls `sceAjmInitialize` and `sceAjmModuleRegister`: its music and dialogue go through Wwise.

`bridge` instead of `native` gives either one back to shadPS4.

shadPS4 is also what the build is assembled on: its CMake target, its third-party libraries (SDL3, Vulkan headers, VMA, fmt, toml11, xbyak, Zydis) and, as A/B references only, its renderer (`--gpu-backend bridge`) and shader recompiler (`SSPC_OWN_SHADERS=bridge`). Building without it is on the [roadmap](#roadmap).

### Components

| Component | State |
|---|---|
| Executable, loader (SELF/ELF, relocations, TLS, NID table) | own |
| Kernel: threads, synchronization, time, event queues, virtual / direct memory | own |
| Exception handling, CPU patcher (`fs:` patches, SSE4a) | own |
| File system, system / user services, PlayGo, app content | own |
| Save data | own |
| Audio output (WASAPI), video decoding (`sceVideodec*`, cutscenes) | own |
| Network, NP, HTTP | own: signed in to an emulated network, Paper Trail answered locally, nothing else goes online |
| Add-ons (app content) | own: the add-ons on the disc unlocked |
| Trophies | own: kept on this PC, notice over the game, platinum |
| Keyboard / mouse input, pad API, button prompts | own |
| PC settings window inside the game's menu | own |
| GPU: PM4 command processor (graphics + async compute), Vulkan backend | own, default |
| Shader translation GCN → SPIR-V | own, default for compute, vertex and pixel programs; shadPS4's recompiler only as an A/B reference and for the programs the own translator declines |
| Engine render functions (G-buffer and shadow passes, materials, constant update engine) | 18 own reconstructions, verified in game; off by default (`SSPC_NATIVE_ENGINE`) except the native G-buffer draw `0x1a1770` in `play.cmd` |
| Ajm / ATRAC9 audio decoding, common dialogs, network, the libc functions the game calls | own in `play.cmd` (R10: `SSPC_R10_AJM`, `SSPC_R10_SYSRT`); shadPS4 with `bridge` |

### What is new

**October 2026, the last week** (all in `main`):

- **Online missions, add-ons and trophies** ([details](#online-missions-add-ons-and-trophies)): the game is signed in, the Paper Trail missions answered locally, the add-ons on the disc (Cole's jacket and missions, the promotion vests) unlocked, and all 48 trophies can be earned, with a notice and the platinum.
- **REA-3 safe reset**: it waits up to 20 ms for a late position label (was 2 ms), so a frame held up by new shaders no longer triggers it.

- **Fixed: the game hanging with the experimental switches** (the async compute lane with early or threaded submission). The log said `too many fences held on queue 1 … 128 fences held waiting for the GPU`, after 2 to 20 minutes of play. The GPU was not stuck: the game's compute queue had run 128 completion fences ahead of a GPU a few batches behind, and at that limit the command processor dropped the queue's whole submission, so the game waited for work that never came. The queue now waits for the GPU there and goes on, as the console's does. If the command processor ever stops a submission again, the log carries every batch in flight, what each waits for and the state of each queue.

- **A launcher and four more graphics settings** ([Launcher](#launcher)).
  - `SecondSonPC.exe` started on its own opens a launcher window: every PC setting by section, the launch profile (`play.cmd` or `play_experimental.cmd`), the video card, processor and memory, and Play. It is SecondSonPC's own code (Direct2D and DirectWrite, no browser engine), and it is part of `SecondSonPC.exe`: no console window flashes up before it.
  - **Graphics → "Shadow quality"**: the size of the sun's shadow maps, 2048 in the game. *High* 4096 (rain's 2048), sharper shadow edges, about +75 MiB of video memory; *Low* 1024 (rain's 512). From the next start: the game creates those maps once.
  - **Graphics → "Texture quality"**: the game streams full-size textures for the 4 city blocks around the player. *Medium* for 2, *Low* for none: less video memory (−45 MiB on the test route) and less disk reading, blurrier close up. There is no level above the game's. From the next start.
  - **Graphics → "Anisotropic filtering"** (live): *as in the game* (4x for most materials), 2x, 4x, 8x or 16x. Roads, walls and roofs at an angle get sharper at 16x; shadows, the HUD and full-screen effects are not touched.
  - **Graphics → "Anti-aliasing"** (live): *as in the game* (SMAA T2x, and under GPU load the game switches itself to FXAA), *SMAA T2x* always, *FXAA*, or *Off*.
  - What each of them costs in FPS is still being measured. All four apply on the native backend; settings self-test: 196 checks, 0 failures.

- **R12 — weather, local lights and reflections** (reports: [R12_WEATHER.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-r12-20261026/docs/R12_WEATHER.md), [R12_WEATHER_PROFILE.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-r12-20261026/docs/R12_WEATHER_PROFILE.md), [R12_LIGHTS.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-r12-20261026/docs/R12_LIGHTS.md), [R12_REFLECTIONS.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-r12-20261026/docs/R12_REFLECTIONS.md), [R12_RAIN_SHADOWS.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-r12-20261026/docs/R12_RAIN_SHADOWS.md)). Medians on the development PC, vertical sync off, no frame limit:
  - **Where the weather's cost is.** The game picks its time of day (sky, rain, wetness) when a world loads, from the mission and the story's progress; it cannot be switched live. Every city time of day is wet. Wetness costs 4.5 % FPS at dusk and 9.8 % in rain by day, and almost all of it is the **rain shadow maps**: a view from above, about 1 000 depth draws a frame, which the game uses to keep the ground dry under bridges and roofs. The wet-surface pass itself is one full-screen draw, 0.4 ms on the GPU.
  - **Graphics → "Rain shadows"** (new, live): *Original* (default) or *Off — nothing is sheltered from the rain*. Off, nothing is drawn into those maps: about 1 000 draws a frame less in wet weather, and the ground under bridges, roofs and in passages is as wet as the open street. Rain, fog and puddles in the open do not change. This trades picture for speed; its FPS is still being measured.
  - **Graphics → "Reflection quality"** (new, live): *Original* (default) or *Low — no screen-space reflections*. Low saves about 0.4 ms of GPU time: owner's save +1.7 % (3 of 3 pairs), Neon Dash 51.7 → 55.0 FPS. The water's own reflection stays (without it the bay is black). Original and a "High" level were the same, so there is one.
  - **GPU writes kept in video memory, experimental** (`SSPC_FB_LAZY_WB=1` with `SSPC_REA_HEAP_READS=1`, on the Experiments page, off by default). Every frame the GPU writes the particle pools (85 % of the bytes) and the lights' per-tile list, and they were copied back to the CPU's memory each frame. Now a block is copied only when the CPU reads it. Owner's save 33.1 → 41.8 FPS (+26 %), 1 % low 27.9 → 33.1; city traverse +4.8 %; beach +8.8 %; fight unchanged (n = 3). A race that crashed Neon Dash in 2 of 11 runs is fixed (0 of 14 after it), and a 32-minute run with them was clean.
  - **Particle sort, experimental** (`SSPC_R12_PSORT=1`). The game's particle ranking shader spent 8.5 ms of GPU time a frame on a loop that emulated the console's lane masks; an equivalent loop takes 1.5 ms. It replaces only that exact shader (SPIR-V length and hash), and matches the original bit for bit on a test stand (200 of 200). On top of the previous switches: owner's save 38.5 → 42.3 FPS, frames over 33 ms 106 → 2 a minute.
  - **Local lights themselves are cheap**: street lamps, headlights and spotlights cost 1.6–1.8 ms of tiled lighting and 0.33 ms of shadow atlas on the GPU; the glows' occlusion queries still keep them from shining through walls.
  - **A weather tool for measurements** (`SSPC_R12_WEATHER=log|dry`, Experiments → Stand tools): logs the engine's weather values, or dries the surfaces of the loaded time of day and restores them on exit; saves are never written.
  - **Checks**: settings self-test 177 checks, 0 failures; Vulkan Validation and Synchronization Validation with the experimental switches: no messages; a 32-minute fight-and-run session with all of them: no GPU loss, no errors. The experimental switches go into `play.cmd` only after the final comparison with R11 that is still running.
- **R11 — even frames without frame generation, more PC settings** ([R11_FINAL_REPORT.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-r11-20261025/docs/R11_FINAL_REPORT.md)). Medians against R10's `play.cmd`, vertical sync off and no frame limit, n = 3 (city scenes n = 2):
  - **Early batch submission, on in `play.cmd`** (`SSPC_R11_EARLY_SUBMIT=256`): while the GPU is idle, the recording thread hands it a batch after 256 commands instead of 500 ([R11_T2.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-r11-20261025/docs/R11_T2.md)).
    - Neon Dash: frames over 33 ms 566 → 100 a minute, 1 % low 17.1 → 23.3 FPS, P99 44.8 → 37.8 ms, 48.2 → 50.1 FPS.
    - Owner's save 31.8 → 32.6 FPS. Fight, traverse, beach, harbour and village change by 0 to +2.5 %; none got slower.
  - **Texture compute on the async queue, order waits only between epochs, in `play_experimental.cmd`** (`SSPC_R11_AQ_IMAGES`, `SSPC_R11_AQ_ORDER_EPOCH`, on top of REA-3 + AQ + MAIN_FREE): fight 46.5 → 51.0 FPS, owner's save 31.8 → 38.2 FPS. Neon Dash is still less even (1 % low 16.2, P99 47.0 ms), so it stays out of `play.cmd` ([R11_NEON.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-r11-20261025/docs/R11_NEON.md)).
  - **Display, live** ([R11_DISPLAY_SETTINGS.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-r11-20261025/docs/R11_DISPLAY_SETTINGS.md)):
    - vertical sync On (FIFO), Off (IMMEDIATE) or Off without tearing (MAILBOX), switched without a restart;
    - frame limits 30, 40, 50, 60, 90, 120, 144, 165, 240 and unlimited, with vertical sync on or off;
    - one "Window mode" row (windowed / borderless) and the monitor;
    - window and resolution changes roll back after 15 s unless confirmed.
  - **Motion blur, in Graphics**: on / off and a strength of 0–100 %, live ([R11_POSTFX.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-r11-20261025/docs/R11_POSTFX.md)).
    - It switches the game's own blur pass and changes nothing else of the picture. It is on by default, as in the game.
    - Turned off, Neon Dash goes 53.0 → 63.6 FPS and a fight 47.4 → 48.5: that is the effect's cost, not an optimization.
    - Film grain and depth of field were not added: in the tested scenes they do not change the frame.
  - **Every R11 switch is on the Experiments page**, also the ones that are off by default. Measured and left off: submission on a thread of its own (`SSPC_R11_ASYNC_SUBMIT`), finished compute seen earlier (`SSPC_R11_FAST_LABEL`, no gain on top of early submission), and the game's compute waits asleep instead of spinning (`SSPC_R11_LABEL_WAIT`, Neon Dash −3.4 %).
  - **The game's main thread** spends 75–79 % of its time waiting for its own async compute, and its own logic takes at most 0.12 ms a frame. Native replacements there would not help ([R11_MAIN_THREAD.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-r11-20261025/docs/R11_MAIN_THREAD.md)).
  - **Checks**:
    - settings self-test: 165 checks, 0 failures;
    - Vulkan Validation and Synchronization Validation: no new messages, including three swapchain re-creations from vertical sync switches;
    - 30-minute runs with vertical sync and limit switches during play;
    - one of three 30-minute runs with early submission lost the GPU (see [Known issues](#known-issues)).
- **R10 — five directions at once** ([R10_FINAL_REPORT.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-r10-20261024/docs/R10_FINAL_REPORT.md)):
  - **Own Ajm / ATRAC9, common dialogs, network and libc** (see [above](#playstation-functions-the-game-imports)) are on in `play.cmd`.
  - **Experiments in the settings menu.** Every switch of `play.cmd` and every experiment is on the new **Experiments** page, with what it measured, so a player can try what works better on their PC (see [PC settings](#pc-settings)).
  - **MAIN_FREE** makes REA-3 + AQ faster and more even, but it is in `play_experimental.cmd` only (below).
  - Measured and left off: compute pipeline prefetch and a shared work pool for pipeline builds (with an empty shader cache Neon Dash 12.9 → 26.1 FPS, one run; with a warm cache no change), spreading heavy threads over cores (no gain), runs across pixel shader changes (city slower), native shadow draws (no FPS change) ([R10_CP.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-r10-20261024/docs/R10_CP.md), [R10_CPU_GPU_SCHEDULER.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-r10-20261024/docs/R10_CPU_GPU_SCHEDULER.md)).
- **R9 — gameplay, not just standing scenes.** A gameplay profiler and fixed routes (fight, traverse, a player's save), on top of which two changes went into `play.cmd`:
  - **F1:** the driver's 827 MiB pipeline cache was saved under the shader cache's lock once a minute, freezing the game for 1–5 s. It is now saved off that lock.
  - **F3:** the vertex heap's window into guest memory is reserved with headroom, and old windows are released, so 2.5 GB re-imports and 220–250 ms stalls are gone.

  Fight 44.5 → 46.0 FPS, 1 % low 7.5 → 25.6, worst frame 3 166 → 82 ms; traverse 47.4 → 51.3, 1 % low 6.1 → 27.9 ([R9_GAMEPLAY_PERFORMANCE.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-20261023/docs/R9_GAMEPLAY_PERFORMANCE.md)).
- **R9 REA-3 and AQ, experimental.** REA-3 has the GPU take the game's second async compute wait of a frame; AQ runs the game's compute on a Vulkan queue of its own. The hang after 16 minutes is fixed by a safe reset, and the device loss with AQ is fixed. With both, the owner's save runs at +11 % FPS, but frames still alternate 17 / 43 ms, so both stay out of `play.cmd` and only `play_experimental.cmd` turns them on ([R9_REA3_FIX.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-20261023/docs/R9_REA3_FIX.md)).
- **R10 MAIN_FREE, experimental.** The 17 / 43 ms pairs came from AQ ordering every graphics batch after every compute batch before it: the two queues overlapped 2–3 ms of a 28 ms frame. With `SSPC_R10_AQ_MAIN_FREE=1` a graphics batch waits only for the compute it depends on (the game's waits, shared memory, the queue's own order). REA-3 + AQ + MAIN_FREE against `play.cmd` (medians, FPS / 1 % low / P99 frame time): owner's save 31.6 / 19.0 / 38.5 ms → 37.6 / 19.7 / 38.5 ms (n = 4), fight 46.6 / 24.8 / 31.2 → 53.0 / 26.9 / 27.8 (n = 4), city traverse 48.4 / 22.7 → 50.7 / 23.2 (n = 2), beach the same; Neon Dash 38.9 / 18.9 → 38.1 / 16.3 (n = 2) is worse, so it is in `play_experimental.cmd` only. Vulkan Validation with Synchronization Validation: no messages. Harbour, village and a 20-minute run were not tested ([R10_PACING.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-r10-20261024/docs/R10_PACING.md)).
- **R8 — the command processor thread.**
  - **Emitter batching:** the Vulkan recording thread publishes its commands once per record. This takes 2.2 ms a frame off it, and the city bench goes 39.1 → 41.5 FPS ([R8_T2_OPTIMIZATION.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-20261023/docs/R8_T2_OPTIMIZATION.md)).
  - The frontend and the Vulkan emitter run on threads of their own, with a 128-entry command ring: the live city goes 28.6 → 36.1 FPS.
  - **The native G-buffer draw `0x1a1770`** is prepared from the engine's own state, without walking its shader resources.
  - A profiler of the command processor thread, and a channel that publishes the engine's constant update engine snapshots to it. The reuse of pixel-shader preparations (`SSPC_R8_CP_STAGE_REUSE`) is in, off by default.
- **R5–R7 — native engine.** 18 engine render functions have SecondSonPC implementations, and the static recompiler v2 handles AVX, floating point and calls. All 13 functions of R7 at once: 0 mismatches, 0 fallbacks, FPS neutral (explained under [Native engine](#native-engine-reconstruction)).
- **R6-P and CITY-OPT (city).** Trivial memory waits no longer read guest memory, which took the city from 28.8 to 32.8 FPS. The engine's draw functions tag their draws, and runs of them are recorded as one unit: 25.3 → 29.4 FPS (Phase 5) and 27.9 → 31.6 FPS (Phase 6).
- **The repository** now holds only what is needed to build plus the files for the game's folder, with `build.ps1` and a release package.

**October 2026, earlier**:
- the own Vulkan backend and own shader translator became the default;
- shader cache and prediction;
- DLSS / FSR 3.1 and FSR frame generation;
- free frame pacing;
- rendering fixes (light glow through walls, flickering smoke, lighting colours);
- button prompts for each input device;
- WASAPI audio, video decoding and saves became native.

## Graphics

### Own command processor and Vulkan backend

- The PM4 packets of the game's command buffers (graphics and async compute) are decoded and executed by SecondSonPC.
- It handles resources, descriptors, GPU memory import, synchronization, barriers by ranges, and write-back of the GPU results the game reads.
- Threads: packet parsing (`GpuNative`), draw preparation (`GpuFrontend`), Vulkan recording (`GpuRecord`), and Vulkan submission (`GpuEmit`).
- Early frame submission: the frame goes to the command processor as soon as the game has finished it.
- Vulkan Validation and Synchronization Validation were clean in the documented test scenes.

### Rendering fixes

| Problem | Cause | Fix |
|---|---|---|
| Vehicle-light glow visible through fences and walls | visibility queries always reported "fully visible" | real Vulkan occlusion queries, results given to the game's compute shader |
| Smoke flickering | the particle sort merged all widths in one dispatch; on the PS4 that is one 64-lane wave, on PC two 32-lane subgroups raced | one dispatch per merge level with a barrier |
| Wrong lighting colours | colour target component swap was masked out | COMP_SWAP honoured |
| Races in the game's own code that the PC's timing exposes | — | targeted fixes, [`GAME_FIXES.md`](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-20261023/docs/GAME_FIXES.md) |

### Shader cache and preparation

- There is a disk cache of translated SPIR-V with reflection, plus the driver's `VkPipelineCache` with device and driver checks.
- Static prediction derives shader programs, descriptor layouts and pipeline states from the game's pack files and engine structures. Hold-out accuracy for whole render-pass pipelines is 87–90 %, and ~87 % of compute keys.
- The preparation window builds them before the game starts. It runs once, and again after a driver or SecondSonPC update.

### Upscaling and frame generation

Upscaling works on the native backend. The upscaler replaces the game's own (PS4 Pro-style) scaling pass, and the HUD is drawn afterwards at full resolution. Motion vectors, reversed depth and jitter come from the game's frame. The modes are DLSS Super Resolution, FSR 3.1 Super Resolution, FSR 3.1 Frame Generation (with the HUD kept out of generated frames) and NVIDIA Reflex. DLSS Frame Generation is not implemented (it needs an RTX 40/50).

## Own shader translator

SecondSonPC's GCN → SPIR-V translator lives in `src/runtime/gpu/gcn/`. It has a decoder, a control-flow graph, structurization, resource analysis with its own SRT walk, and a SPIR-V emitter. It follows the bridge's binding conventions, so its modules can be mixed with the bridge's in one pipeline. It decides per program, and a program it does not handle goes to the bridge's recompiler with the reason logged.

| Stage | Own / sent to the bridge (2- and 6-minute gameplay runs) | `spirv-val` |
|---|---:|---|
| Compute | 95 / 0, 103 / 0 | all valid |
| Vertex (incl. rect / quad lists) | 133 / 0, 149 / 0 | all valid |
| Pixel | 369 / 0, 516 / 0 | all valid |

Offline, over all 10 301 GCN programs in the game's files, 7 934 modules were emitted, all valid; the rest need the game's runtime state. Vulkan Validation with Synchronization Validation reported 0 messages. Details: [`OWN_SHADER_TRANSLATOR.md`](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-20261023/docs/OWN_SHADER_TRANSLATOR.md).

## Native engine reconstruction

The native engine work replaces individual engine functions with SecondSonPC's implementations, in memory, after loading; the files on disk are never changed.

- A registry entry names the function by its `eboot.bin` offset and pins it by a hash of its original body.
- The entry is redirected to SecondSonPC's implementation, and a relocated clone of the original stays callable.
- Each function runs in one of three modes:
  - `verify`: both run, and every write and every call to other engine code is compared;
  - `guarded`: native, with sampled comparisons; a mismatch switches the function back to the original;
  - `native`.
- Implementations are hand-written C++ or recompiled (Ghidra P-code → C++), and are verified offline first.
- `SSPC_NATIVE_ENGINE=verify|guarded|native` with `SSPC_NATIVE_ENGINE_FUNCS=<names>` turns them on. `play.cmd` uses only the native G-buffer draw preparation `SSPC_NATIVE_GBUFFER=native`.

| Area | Functions (eboot offsets) | In-game check |
|---|---|---|
| Object constants, frame allocator | `0x11c890`, `0x1f0870` | 19.0 M / 21.2 M calls in `verify`, 0 mismatches |
| Constant update engine: setters | `0x16aa0`, `0x16b70`, `0x169d0`, `0x16d10`, `0x16eb0`, `0x16c40`, `0x16de0` | e.g. 219 M calls of the first two in `verify`, 0 mismatches |
| Constant update engine: commit | `0x14ad0` | 47.4 M native, 185 k compared, 0 mismatches |
| Material setup, draw constants | `0x448640`, `0x11e5d0` | 15.4 M / 9.0 M calls in `verify`, 0 mismatches |
| G-buffer pass and its draws | `0x1a2030`, `0x1a1770`, `0x1a1960` | 6.3 M / 21.3 M draws in `verify`, 0 mismatches |
| Shadow pass, instanced shadow draw, shadow UVs | `0x43d590`, `0x43d480`, `0x451700` | 17.5 M native shadow draws, 4.4 M UV calls, 0 mismatches |

**Why it gives no FPS by itself.** These functions run on the game's job threads, while the frame waits for command processing, async compute and the GPU. What R5–R7 deliver is an engine whose render path increasingly runs on code the project owns. Using the engine's knowledge to cut command processor work began in R8 (the native G-buffer draw, the CUE snapshot channel). Details: [`NATIVE_ENGINE_RECONSTRUCTION.md`](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-20261023/docs/NATIVE_ENGINE_RECONSTRUCTION.md), [`NATIVE_ENGINE_R7_RESULTS.md`](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-20261023/docs/NATIVE_ENGINE_R7_RESULTS.md).

## Online missions, add-ons and trophies

The game was made for PlayStation Network: online missions, add-ons bought in the store, trophies. SecondSonPC gives all three without PSN and without sending anything anywhere.

**Online: the "Paper Trail" missions**
- The game always sees a player signed in to the network, under the name `t.me/cloudeluxe`. There is nothing to turn on.
- Paper Trail was an online story: six cases, part of them done on a website, part in the game, with rewards (vests). Its server, `api.infamouspapertrail.com`, was shut down by Sony. SecondSonPC answers the game's requests itself: every website step counts as done, and the in-game steps are the game's own progress.
- So the "Paper Trail" missions under **"Evidence"** should open instead of the "Connect to PSN" message.
- That is the only server the game talks to. Everything else stays offline: the game's other network library sees no connection.

**Add-ons (DLC)**
- The pre-order and promotion add-ons were unlock keys: their content is already on the game's disc. SecondSonPC tells the game that every key it asks for is owned.
- Unlocked: **Cole's Legacy** (Cole's jacket and its missions) and the **Penny Arcade, Exist, Jos, Skeleton** and **Handprint** vests.
- **Where to put them on:** pause the game (Esc, or Options on a controller) and open **"Special items"** (Russian: «Особые предметы»), next to the powers on the character page. In the list: *Cole's Jacket* («Куртка Коула»), *Penny Arcade*, *JOS*, *iam8bit*, *Collector's Edition* (the Handprint vest), and the game's own vests. The add-on vests are there from the start; the game's district vests come with the story and with freed districts ("New vest available").

**Trophies**
- All 48 trophies can be earned, the platinum included: it comes on its own with the last one.
- Each trophy shows a notice at the top left of the game for 6 seconds, with its icon from the game's own trophy file.
- Earned trophies are kept in `userdata\home\trophies\CUSA00004.txt`, one line `<number> <time>` each, and the game reads them back at every start. A line `<number> 0` is a trophy added by hand: its notice is shown at the next start.
- The names are the official ones, in Russian or English as the menu is: the game's own text table has every trophy's name and description (`u_trophy_<key>_name`), read from your copy of the game. Grades (bronze, silver, gold) are not shown: they are only in the trophy file's encrypted part.
- The same text table keeps two trophies that were cut: **"Deadliest Catch"** («Крупная рыба», subdue a D.U.P. Commander without killing him) and **"Hard Mode"** (finish the game in Hard Mode), and an earlier description of "Unstoppable": "Finish the game in Challenge Mode".
- Trophies earned with builds before this one were not kept. The game gives a trophy again when its condition is met again.

These are new and not yet played through from start to end: if a Paper Trail or Cole's Legacy mission does not open, or a trophy shows no notice, open an issue with `logs\SecondSonPC.log`.

## Cut content

**[Cut content →](cut-content/README.md)** ([по-русски](cut-content/README.ru.md)) ⚠️ *Story spoilers, endings included.* What the retail game still carries from its development, found while porting it: the Akuran gang that was Russian, Blast Cores, a third ending choice, Hank's daughter, a Surveillance Van activity, an XP system, Story and Challenge modes, cut powers and trophies, two unused USTV news broadcasts, build modes (Demo, Kiosk, Beta, Review) and debug worlds. With the voice lines in English and the official Russian dub, and the unused videos. In English and Russian.

## Launcher

`SecondSonPC.exe` started without arguments opens the launcher; **Играть** (Play) starts the game with what is chosen there. It is in Russian for now.

<p align="center">
  <img src=".github/readme/launcher-home.png" alt="The launcher: Play, the launch profile, the PC's configuration" width="880">
</p>

- **Home**: Play, the launch profile and whether the game's folder is found. *Standard* is `play.cmd`'s preset, *Experimental* is `play_experimental.cmd`'s. Before every start the saves are copied to `userdata\savedata_backups\<date_time>`, as `play.cmd` does.
- **Display, Graphics, Effects, Controls, Audio, Advanced**: the same settings as the in-game window, saved to the same `settings.toml`, so a change in one shows in the other. Settings that need a restart and settings that are unavailable say so; Advanced ends with the closed Experiments section.
- The game's own art (`sce_sys\pic0.png` of your copy) is the home page's background; it is read from the game's folder, not shipped.
- `play.cmd` and any command line with arguments start the game at once, without the launcher.

<p align="center">
  <img src=".github/readme/launcher-graphics.png" alt="The launcher's Graphics page: shadow, reflection and texture quality, anisotropic filtering" width="880">
</p>

<p align="center">
  <img src=".github/readme/launcher-controls.png" alt="The launcher's Controls page: mouse settings" width="880">
</p>

## PC settings

The PC options are in the [launcher](#launcher) and inside the game's own menu: **Esc (Options) → Settings → "Additional settings"**. This is the line that used to read "HDR Rendering — Unavailable"; the string is replaced as it is read, and the game's files are not changed.

- There are eight pages: **Graphics, Display, Controls, Keys, Audio, Game, Advanced, Experiments**.
- **Experiments** lists the runtime's switches by group: the command processor, Vulkan recording, pipelines and memory, GPU queues and frame pacing, processor threads, R10's draws from the engine's data, the system libraries, R12's particles and weather, and R12's stand tools (diagnostics, off). Each says what it does, what it measured and whether `play.cmd` or `play_experimental.cmd` turns it on. Switches that are off by default are listed too, so every one of them can be tried. The launcher's value is the default; what a player changes is saved in their settings file and applies from the next start, because those subsystems read it once at start. "Reset this page" returns to the launcher's values. The log marks every changed one with `EXPERIMENT`.
- The window works with keyboard, mouse and gamepad navigation, with prompts for the last device used.
- Changes apply immediately where the subsystem allows it: upscaler and quality, sharpness, frame generation, Reflex, smoke quality, particle resolution, motion blur and its strength, reflection quality, rain shadows, anisotropic filtering, anti-aliasing, vertical sync, frame-rate limit, mouse and motion settings, volume. Shadow quality and texture quality apply from the next start.
- **Display**:
  - vertical sync is On (FIFO), Off (IMMEDIATE) or Off without tearing (MAILBOX);
  - the frame-rate limit is 30, 40, 50, 60, 90, 120, 144, 165, 240 or unlimited;
  - "Window mode" is windowed or borderless;
  - the monitor list shows each display's desktop size and refresh rate.
- Window mode, resolution and monitor changes ask for confirmation for 15 s and roll back if it is not given.
- Options that need a restart show a badge, and unavailable options say why.
- The window scales with the game window and DPI, from 800 × 600 to 4K and 21:9.

Details: [`PC_SETTINGS_UI.md`](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-20261023/docs/PC_SETTINGS_UI.md).

## Input and button prompts

Keyboard and mouse are handled natively, with PC key bindings. The game draws every button prompt from one icon font, and SecondSonPC redraws the physical-button cells for the active input device. So **all** prompts of the game, in every language, switch between keyboard/mouse, Xbox and PlayStation, without editing strings or files.

<p align="center">
  <img src=".github/readme/prw1.png" alt="Keyboard and mouse prompt adaptations" width="900">
</p>

<p align="center">
  <img src=".github/readme/prw2.png" alt="Original-style controller prompts and alternative button layouts" width="900">
</p>

### Native mouse camera

On the PS4 the camera is turned by the right stick, and an emulator usually turns mouse movement into a stick deflection. That brings the stick's dead zone, response curve, acceleration and top speed with it: slow movements are lost, fast ones are capped. SecondSonPC hands the mouse to the game's camera directly instead.

- The game's camera integrator (eboot `0x14d6f0`) gets the mouse counts as yaw and pitch angles: 0.001 rad per count at `mouseSensitivity = 1.0` (1 000 counts ≈ 57°). Nothing is smoothed or accelerated, so the same hand movement always turns the camera by the same angle, at any frame rate.
- What the game decides stays the game's:
  - its vertical limits for the current camera, the same as the stick's (on foot about 85° up and down);
  - camera locks in cut-scenes and moves;
  - the game's own X / Y inversion options, plus `invertY` in `[Input]`.
- The mouse moves the camera through the same limits as the stick. The automatic camera's narrower range (about ±10°, the one it returns to when you run) does not hold the mouse back.
- Motion mode (G, for graffiti) and the touch pad keys still move the controller and the touch pad, not the camera.
- `SSPC_MOUSE_CAMERA=0` returns to the stick emulation (`mouseDeadzone` applies only there).

## Resolutions and aspect ratios

The game renders at the window's size, or the display's in full screen, not at a fixed 1920 × 1080. Set it in **Settings → Display** or in `SecondSonPC.toml`:

| Key | Values | What it does |
|---|---|---|
| `[Display] resolution` | `"auto"` \| `"WxH"` | The game's output size. `auto` = the window / the display. |
| `[Display] aspectRatio` | `"auto"`, `"4:3"`, `"16:10"`, `"16:9"`, `"21:9"`, `"32:9"` | `auto` follows the window. A window taller than 16:9 (e.g. portrait) gets 16:9 with bars. |
| `[Graphics] renderScale` | `"auto"` \| `"50"`…`"100"` | The 3D scene against the output. `auto` = at most 1080 lines, so 4K output keeps a 1080p scene unless raised. The scene is capped at 2880 × 2160 pixels. |
| `[Display] hudSafeArea` | 85…100 % | The HUD's distance from the edges, like the PS4's Display Area. |
| `[Display] hudArea` | `"full"`, `"21:9"`, `"16:9"` | On very wide pictures, keeps the HUD in the centre. |
| `[Graphics] fov` | 80…130 % | The player camera's field of view; cut-scenes keep theirs. |

- **Hor+.** Wider shapes show more of the world at the sides with the same vertical view. Nothing is stretched.
- **HUD and menus.** The HUD is never stretched. The game's pause and settings screens stay centred on wide pictures, and mouse pointing in them follows that layout.
- **Movies.** On a picture that is not 16:9 the game cut pre-rendered movies above and below (21:9, 32:9) or smeared their edges into the bars (4:3). They are now shown whole, 16:9, with black bars. Subtitles are unchanged.
- **Cost.** At 1920 × 1080 nothing changes (55.7 vs 56.0 FPS in an A/B). A wider picture draws more of the world: about −18 % FPS at 2560 × 1080 and −31 % at 32:9 on an RTX 2060. Render scale and DLSS / FSR help.
- Needs the default GPU backend (`--gpu-backend sspc`).

## Archive

`main` is the playable build and the only branch. Earlier work that is not in it is kept in tags:

| Tag | What | State |
|---|---|---|
| [`archive/feature/native-engine-r8`](../../tree/archive/feature/native-engine-r8) (+ `archive/feature/r8-cp-profile`, `r8-native-cp`, `r8-render-layer`, `r8-recomp`, `r8-verify`) | R8 directions: the native render layer, a CUE audit, a deterministic verification mode | the R8 core, profile, CP channel, native G-buffer draw and emitter batching are in `main`; the rest is research |
| [`archive/feature/fork-port-prep-workers`](../../tree/archive/feature/fork-port-prep-workers) | ideas from the axylpup shadPS4 fork; **potato mode** for weak PCs (`[Graphics] potatoMode` levels 1–4: fewer shadow maps and small objects) | the frontend and emitter threads are in `main`; potato mode only in the tag |
| [`archive/feature/native-missions`](../../tree/archive/feature/native-missions) (on top of [`archive/feature/native-rebirth`](../../tree/archive/feature/native-rebirth)) | **Native Rebirth**: a separate native program that reads the game's packages itself, renders with its own Vulkan renderer, plays Delsin's original animations on the original Havok collision and runs the prologue's graffiti mission | research vertical slice, not the game |
| [`archive/experiment/bbport-gpu`](../../tree/archive/experiment/bbport-gpu) | BBPort's GPU library as the backend | rejected: 25 vs 40 FPS, effects missing |
| [`archive/feature/uncharted-bbport-optimizations`](../../tree/archive/feature/uncharted-bbport-optimizations) | uncharted_pc / bbport techniques: threaded recording, preparation workers, LTO, PGO | measured, nothing taken |
| [`archive/docs-r10-20261024`](../../tree/archive/docs-r10-20261024) | R10 as developed: its six reports in `docs/`, its tools in `tools/r10` | the code is in `main` |
| [`archive/docs-r11-20261025`](../../tree/archive/docs-r11-20261025) | R11 as developed: its six reports in `docs/`, its tools in `tools/r11` | the code is in `main` |
| [`archive/docs-r12-20261026`](../../tree/archive/docs-r12-20261026) | R12 as developed: its five reports in `docs/`, its tools in `tools/r12` | the code is in `main` |

The tag [`archive/dev-tools-20261023`](../../tree/archive/dev-tools-20261023) keeps the removed development scripts: A/B stand, measurement routes, analysis tools, the `play_*.cmd` variants.

## Architecture

```text
Original PS4 game (eboot.bin + modules, unchanged on disk)
        |  x86-64 code runs natively on the PC's CPU
        v
SecondSonPC loader and runtime
        +-- kernel: threads, sync, time, event queues
        +-- virtual / direct memory, exception handling, CPU patcher (fs:, SSE4a)
        +-- file system, system / user services, PlayGo
        +-- save data, WASAPI audio, video decoding
        +-- offline network / NP / trophies
        +-- PC settings window in the game's menu, input, button prompts
        +-- Ajm / ATRAC9, common dialogs, network, libc (R10, on in play.cmd)
        |
        +-- native engine: hash-pinned replacements of engine functions
        |     (hand-written / recompiled with AVX; verify, guarded, native)
        v
SecondSonPC GPU
        +-- PM4 command processor: parse | prepare | record | submit threads
        +-- shader frontend: SRT walk, resource plans, specialization keys
        +-- GCN -> SPIR-V: own translator (bridge recompiler for declined programs)
        +-- engine draw runs (CITY-OPT), native G-buffer draw preparation
        +-- shader + pipeline disk cache, static shader prediction
        +-- Vulkan executor: descriptors, memory import, barriers, write-back
        +-- presenter: frame pacing, DLSS / FSR upscaling, FSR frame generation, Reflex
        v
Vulkan 1.3 driver
```

## Known issues

- **Performance.** Dense scenes are bound by CPU-side command processing, Vulkan recording and the game's async compute: gameplay in the city runs at 48–50 FPS and a heavy save at 33 FPS (42 with R12's experimental switches) on the development PC, far from a locked 60.
- **Intel CPUs** lack SSE4a. In a forced test on AMD, the 73 short `extrq` emulated through faults cost ~10 % FPS at the start of a scene. A real Intel CPU has not been tested ([SSPC-011](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-20261023/docs/KNOWN_ISSUES.md)).
- **Tested hardware.** Only one PC was tested: Ryzen 5 5500, RTX 2060 and Windows 10. AMD and Intel GPUs and other drivers are unknown.
- **Game version.** Only CUSA00004 with update 01.07 (EU) is tested; the engine replacements install only on that executable.
- **From shadPS4.** The build still needs `third_party/shadPS4`: its CMake target, its third-party libraries, and the bridge as the A/B reference (the `bridge` values of the switches). 49 imports that the game does not call in a normal run still belong to the bridge.
- **Memory.** On a PC without a page file, total committed memory can reach the Windows limit in long sessions; keep the page file on. Fixed causes are in [`KNOWN_ISSUES.md`](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-20261023/docs/KNOWN_ISSUES.md).
- **Shader preparation** cannot predict every program: 166 of about 304 000 predicted shader requests are declined by the own translator (for example an `image_sample` whose resource is not known before the game runs). The game translates those the first time it uses them, which can cause one short stutter.
- **Frame generation** has not been reviewed frame by frame, and input latency with it is not measured.
- **GPU loss once with early submission.** In one of three 30-minute runs with `SSPC_R11_EARLY_SUBMIT=256` the driver reset the GPU after 18.5 minutes (`VK_ERROR_DEVICE_LOST`). Its exact repeat, another run and a run without it were clean, and the cause is not known. Such resets also happened on that PC before R11. If it happens to you, turn off "Early batch submission (R11)" on the Experiments page: that is R10's behaviour.
- **Not tested in R11:** flying, cut-scenes, vertical sync switches together with DLSS / FSR frame generation, gamepad navigation of the new rows, two monitors.
- **R12 is still under its final check.** The experimental switches above are off by default; their comparison with R11 over every scene is running. "Rain shadows: Off" has not yet been checked frame by frame. Not tested in R12: flying, cut-scenes, light from Delsin's powers (none on the routes), harbour and village with the particle sort.
- **The launcher and the new graphics settings** are new: the launcher is in Russian only, and the FPS cost of shadow quality, texture quality, anisotropic filtering and anti-aliasing is not measured yet. If something looks wrong, set them back to *as in the game*.
- **Online missions, add-ons and trophies are new** and not yet played through: the Paper Trail missions, the Cole's Legacy missions and each trophy's notice have been built, not tested in the game from start to end. The trophy grades (bronze, silver, gold) are not shown: they are in the encrypted trophy texts.
- There is no full-campaign compatibility guarantee.

## Roadmap

### Runtime and services
- [x] Own loader, kernel, threads, memory, exceptions, CPU patcher
- [x] File system, system services, save data, offline network / trophies
- [x] WASAPI audio output, video decoding
- [x] Ajm / ATRAC9 decoding (R10, LibAtrac9)
- [x] Common dialogs, network and the libc functions the game calls (R10)
- [x] Signed in to an emulated network, Paper Trail answered locally; the add-ons on the disc unlocked; trophies kept on the PC with notices and the platinum
- [ ] The imports the game does not call in a normal run (49 at the bridge)

### Graphics and performance
- [x] Own PM4 command processor and Vulkan backend (default), four threads
- [x] Shader / pipeline disk cache, static prediction, preparation window
- [x] DLSS / FSR 3.1 upscaling, FSR 3.1 frame generation, Reflex
- [x] Free frame pacing
- [x] City: draw runs (CITY-OPT 1–6), trivial waits (R6-P), emitter batching (R8)
- [x] Gameplay: no pipeline-cache freezes (F1), no vertex heap re-imports (F3)
- [x] REA-3 / AQ with even frame pacing in fights and on the owner's save (R10 MAIN_FREE, `play_experimental.cmd`)
- [x] Even frames in Neon Dash without frame generation (R11 early batch submission, `play.cmd`)
- [ ] REA-3 / AQ / MAIN_FREE / AQ_IMAGES into `play.cmd`, after Neon Dash (1 % low and P99 below `play.cmd` now)
- [x] Weather, local lights and reflections measured pass by pass; particle sort 8.5 → 1.5 ms, GPU writes kept in video memory (R12, experimental)
- [ ] R12's GPU-write and particle switches into `play.cmd`, after the final comparison
- [ ] Less command processor work for the remaining draws (the rain shadow maps' ~1 000 depth draws first)
- [ ] Potato mode for weak PCs in `main`

### Native engine
- [x] Function registry, hash-pinned install, verify / guarded / native, fallback
- [x] Static recompiler v2 (AVX, floating point, calls)
- [x] G-buffer and shadow passes, materials, CUE setters and commit, verified in game
- [x] The engine's state used by the command processor (native G-buffer draw, CUE channel)
- [ ] The native render layer submits its draws without the game's command buffer

### PC experience
- [x] PC settings window in the game's menu
- [x] Experiments page: every runtime switch on and off from the menu (R10), also the ones off by default (R11)
- [x] Live vertical sync (FIFO / IMMEDIATE / MAILBOX), more frame limits, window mode with roll-back (R11)
- [x] Motion blur on / off and its strength (R11)
- [x] Reflection quality and rain shadows in Graphics (R12)
- [x] A launcher before the game; shadow quality, texture quality, anisotropic filtering and anti-aliasing in Graphics
- [ ] The launcher in English
- [x] Button prompts for keyboard/mouse, Xbox, PlayStation
- [x] Native mouse camera (no stick emulation)
- [x] Any resolution and aspect ratio (4:3 to 32:9, Hor+, HUD, FOV, movies 16:9)
- [ ] Tested on real 21:9, 32:9 and 4K monitors
- [x] A release package for the game's folder (`build.ps1`, `play.cmd`)
- [ ] Tested on more hardware (AMD / Intel GPUs, Intel CPUs)

### Independence
- [ ] Remove the bridge (`src/bridge/shadps4`)
- [ ] Build without `third_party/shadPS4`

## Reports

Every phase ends with a report: what was measured, what was kept, what was rejected and why. They are not part of the build, so they are not in `main`: the full list, **135 reports** at the tag `archive/docs-20261023` and in the other archive tags, grouped by area, is in **[`REPORTS.md`](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-20261023/docs/REPORTS.md)**. Most are written in Russian, the newer ones partly in English. R10's six reports are at the tag `archive/docs-r10-20261024`, starting with [R10_FINAL_REPORT.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-r10-20261024/docs/R10_FINAL_REPORT.md); R11's six are at the tag `archive/docs-r11-20261025`, starting with [R11_FINAL_REPORT.md](https://github.com/kam0n4ik/inFAMOUS-Second-Son-PC-Edition/blob/archive/docs-r11-20261025/docs/R11_FINAL_REPORT.md).

## Contributing

Reproducible bug reports with `logs\SecondSonPC.log`, traces and documentation fixes are welcome. For performance changes:
- record a baseline;
- change one thing at a time;
- check rendering and Vulkan validation;
- report average FPS and 1 % lows;
- document failed experiments.

Do not submit game dumps or any proprietary game content.

## Legal notice

This is an independent, unofficial project and is not affiliated with, endorsed by, or sponsored by Sony Interactive Entertainment or Sucker Punch Productions. inFAMOUS and inFAMOUS: Second Son are trademarks and/or copyrighted works of their respective owners. Apart from the short research clips in `cut-content\` (unused voice lines and videos, documented there), no original game content is distributed by this repository; users must supply their own legally obtained game files.

Third-party components remain under their own licences. These include shadPS4 (GPL-2.0) and its libraries; the NVIDIA DLSS SDK, Streamline and Reflex; and the AMD FidelityFX SDK, whose licence texts are in `licenses\` of the release package. The Roboto fonts are under the OFL / Apache 2.0.

## Acknowledgements

- The shadPS4 project, whose emulator core the port started from and still builds on.
- The axylpup shadPS4 fork for Second Son, whose split of command processing inspired the fork-port work (`archive/feature/fork-port-prep-workers`).
- The BBPort (Bloodborne PC) and uncharted_pc projects, whose techniques were measured here.
- Ghidra, LLVM and the authors of the open-source Vulkan tools and libraries used here.
- Researchers documenting PlayStation 4 hardware, GCN and executable formats.

## Disclaimer

This is experimental software: features may be incomplete and regressions may occur. Back up your save data; `play.cmd` does it before every start.
