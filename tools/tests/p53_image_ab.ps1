# SecondSonPC P5.3D.2: deterministic image comparison of the game's PM4 path and the native render path.
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\tests\p53_image_ab.ps1 -Tag <name>
#       [-Scene beach] [-Passes 1,9] [-Block 4] [-FirstFlip 3000] [-Flips 32] [-Seconds 125] [-Validate] [-Env "A=b"]
# The scene is frozen with the game's own pause (Options = Return at 70 s): the world keeps being rendered every frame
# with the simulation stopped (the same draw count frame after frame). SSPC_P53_NATIVE_AB=<Block> then alternates
# blocks of flips between the game's PM4 path and the native path (apply mode in -Passes, native state on), and the
# targets those passes wrote are captured where their draws end (SSPC_P53_CAPTURE_PASSES), plus the scanout image at
# each flip's end (SSPC_DIAG_CAPTURE_IMAGE). tools\tests\p53_image_compare.py compares every native capture with the
# PM4 captures of the same target and the PM4 captures with each other (the noise floor), bit for bit and by value.
# Output: logs\p53\<Tag>\<scene>_1\ (the run) and ...\captures\ (images, captures.csv, compare\report.md/.json, heat maps).
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$Scene = "beach",
    [string]$Passes = "1,9",
    [int]$Block = 4,
    [int]$FirstFlip = 3000,
    [int]$Flips = 32,
    [int]$Seconds = 125,
    [switch]$Validate,
    [string]$Env = ""
)
$ErrorActionPreference = "Stop"
$root = "E:\SecondSonPC"
$out = "$root\logs\p53\$Tag"
$cap = "$out\captures"
if (Test-Path $cap) { throw "$cap exists: pick another -Tag" }
New-Item -ItemType Directory -Force $cap | Out-Null
$scanout = "26c568000" # the flip's image (RENDER_ENGINE_PASSES.md: the post context's target)
$press = "space@12,space@22,move:-3000:0@56:0.25,move:-3000:0@58:0.25,move:-3000:0@60:0.25,return@70"
$envs = @(
    "SSPC_P410_PARALLEL_CP=0", # (pass captures: the single-thread processor only)
    "SSPC_P53_NATIVE_DRAW=apply:$Passes",
    "SSPC_P53_NATIVE_STATE=apply",
    "SSPC_P53_NATIVE_AB=$Block",
    "SSPC_P53_CAPTURE_PASSES=$Passes",
    "SSPC_P53_CAPTURE_FLIPS=$FirstFlip+$Flips",
    "SSPC_DIAG_CAPTURE_IMAGE=${scanout}:$FirstFlip+$Flips",
    "SSPC_DIAG_CAPTURE_DIR=$cap"
) -join ';'
if ($Env) { $envs += ";$Env" }
$args46 = @("-ExecutionPolicy", "Bypass", "-File", "$root\tools\tests\p46_runs.ps1", "-Tag", $Tag, "-Scenes", $Scene,
    "-Repeats", "1", "-Seconds", "$Seconds", "-OutRoot", "$root\logs\p53", "-ExtraArgs", "--press $press", "-Env", $envs)
if ($Validate) { $args46 += "-Validate" }
powershell @args46
$names = "$cap\names.json"
@{ "26c568000" = "scanout"; "2fcdf0000" = "lighting (pass 9 -> forward)"; "26b200000" = "main depth";
   "2f9e10000" = "G-buffer 0"; "2fa610000" = "G-buffer 1"; "26bd70000" = "G-buffer 2"; "2fb600000" = "G-buffer 3";
   "2fbe00000" = "G-buffer 4"; "304a20000" = "depth (pass 1)" } | ConvertTo-Json | Out-File $names -Encoding ascii
py -I -X utf8 "$root\tools\tests\p53_image_compare.py" $cap --names $names
