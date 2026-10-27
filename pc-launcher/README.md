# inFAMOUS PC Launcher

One Qt Quick launcher for both games of the shared runtime: inFAMOUS: Second Son and inFAMOUS: First Light PC Edition.
It finds the game from the folder it stands in (`sce_sys\param.sfo`, the product's `.toml` and exe), edits the same
settings file the game reads (`%LOCALAPPDATA%\<Product>\settings.toml`), and starts the game with `--no-launcher`.

In a player's install it lives in `Launcher\` next to `SecondSonPC.exe` / `FirstLightPC.exe`. The game's exe started
without arguments opens it; without `Launcher\` the exe opens its built-in launcher as before (`--launcher` always does).

## Building

Needs MSVC 2022 (14.4x), the Windows SDK, CMake + Ninja and Qt 6.10 for msvc2022_64. `tools\env.ps1` names the paths
used here; each can be overridden by the environment variable of the same name (`LAUNCHER_QT`, `LAUNCHER_MSVC_ROOT`,
`LAUNCHER_WINSDK`, `LAUNCHER_CMAKE_BIN`, `LAUNCHER_NINJA_BIN`).

```powershell
powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Tests   # build\Release\bin\Launcher.exe and the tests
powershell -ExecutionPolicy Bypass -File tools\test.ps1           # the tests (fixtures in %TEMP% only)
powershell -ExecutionPolicy Bypass -File tools\deploy.ps1         # dist\inFAMOUS-PC-Launcher: exe, Qt, MSVC runtime
```

The runtime's `build.ps1` puts `dist\inFAMOUS-PC-Launcher` into its package as `Launcher\` when it exists.

## Licenses

Qt is linked dynamically under the LGPL 3.0 (`licenses\`); the font Roboto Condensed is under the SIL OFL 1.1.
