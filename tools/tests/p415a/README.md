# P4.15A test infrastructure

The authoritative artifacts for the 2026-10-12/13 run are under E:\SecondSonPC_AI\p415a.
These scripts preserve explicit E: paths, TEMP/TMP/TMPDIR, settings and copied savedata.
Do not run them with the game or protected worktree as an output directory.
No script in this directory replaces play.cmd or tools\manual_run.ps1.

- build.ps1: existing offline dependencies, independent E:\SecondSonPC_AI\build\p415a.
- run.ps1: unique run folders, isolated profiles, fingerprints, stdout/stderr and graceful shutdown.
- ab-resume.ps1: three runs per scene/build, resumes only incomplete entries.
- metrics.py: reads gpu_frames.csv and reports whole/steady frames and resource metrics.
- pair.ps1: two independent processes contending for an isolated shader/pipeline cache.
- verify-preservation.ps1: compares protected files, nested working files and task2 against initial backup.
- snapshot-binary.ps1: exact executable and source identity; refuses to replace an existing label.

These copies retain absolute paths to the recorded E: workspace and scripts, intentionally. Review paths before reuse on a different machine. Use the copies in E:\SecondSonPC_AI\p415a for the original run; binaries and baseline manifests there are immutable labels.
The application includes --gpu-stability-test for deterministic API-result fault injection using real Vulkan objects; it requires validation to be enabled, and writes only to configured logs/test-cache paths.
See docs/PHASE4_P415A_STABILITY.md for current test status and limitations. A saved checkpoint is not evidence that its tests passed.

