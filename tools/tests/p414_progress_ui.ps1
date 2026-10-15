# SecondSonPC P4.14: the start-up precompiler's progress panel and its skip key, through the OPERATING SYSTEM's input
# (no --press: an automated run has no panel). A warm shader cache copy, the driver's pipeline cache refused
# (SSPC_DIAG_PIPELINE_CACHE_DRIVER=1) and the driver's own cache empty: the precompilation takes seconds, the panel is
# seen; screenshots of the window's screen area (the panel is a layered window over it), Esc while it is up, the log's
# "skipped by the user" and the background workers checked; WM_CLOSE at the end.
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\tests\p414_progress_ui.ps1 -Out <dir> -Cache <warm cache>
#       [-Scene beach] [-EscAt 5] [-Seconds 40] [-NoEsc]
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [Parameter(Mandatory = $true)][string]$Cache,
    [string]$Scene = "beach",
    [double]$EscAt = 5,
    [int]$Seconds = 40,
    [switch]$NoEsc,
    [string]$Exe = "E:\SecondSonPC\build\bin\SecondSonPC.exe"
)
$ErrorActionPreference = "Stop"
$root = "E:\SecondSonPC"
Import-Module "$root\tools\game_process.psm1" -Force
Add-Type -AssemblyName System.Drawing
if (-not ('SspcSendInput' -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class SspcSendInput {
    [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT { public int dx, dy; public uint data, flags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort vk, scan; public uint flags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Explicit)] struct UNION { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
    [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public UNION u; }
    [DllImport("user32.dll", SetLastError = true)] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    const uint KEYEVENTF_KEYUP = 2, KEYEVENTF_SCANCODE = 8;
    public static uint Key(ushort scan, bool up) {
        var i = new INPUT[1];
        i[0].type = 1;
        i[0].u.ki.scan = scan;
        i[0].u.ki.flags = KEYEVENTF_SCANCODE | (up ? KEYEVENTF_KEYUP : 0);
        return SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }
    public static bool Focus(IntPtr hwnd) {
        if (GetForegroundWindow() == hwnd) return true;
        uint pid;
        uint fg = GetWindowThreadProcessId(GetForegroundWindow(), out pid);
        uint me = GetCurrentThreadId();
        bool attached = fg != 0 && fg != me && AttachThreadInput(me, fg, true);
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        if (attached) AttachThreadInput(me, fg, false);
        return GetForegroundWindow() == hwnd;
    }
}
"@
}

New-Item -ItemType Directory -Force $Out | Out-Null
$actions = "$Out\actions.txt"
"p414_progress_ui: scene $Scene, cache $Cache, Esc at $EscAt s (off: $($NoEsc.IsPresent)) ($(Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))" |
    Out-File $actions -Encoding utf8
$sources = @{
    beach    = "$root\logs\performance\p43_ab\snapshot\home"
    early    = "$root\userdata\savedata_backups\2026-10-10_03-44-58_before_manual_sspc"
    user1022 = "$root\logs\p32\scene_user_1022\home"
}
$profileDir = "$root\logs\p414\ui_profile"
if (Test-Path $profileDir) { Remove-Item $profileDir -Recurse -Force }
New-Item -ItemType Directory -Force $profileDir | Out-Null
Copy-Item $sources[$Scene] "$profileDir\home" -Recurse
$cacheCopy = "$Out\cache"
if (Test-Path $cacheCopy) { Remove-Item $cacheCopy -Recurse -Force }
Copy-Item $Cache $cacheCopy -Recurse
$nv = "$Out\nv"
if (Test-Path $nv) { Remove-Item $nv -Recurse -Force }
New-Item -ItemType Directory -Force $nv | Out-Null
foreach ($f in "gpu_native.txt", "gpu_frames.csv", "gpu_device_lost.txt") { Remove-Item "$root\logs\$f" -ErrorAction SilentlyContinue }
if (Get-Process -Name SecondSonPC -ErrorAction SilentlyContinue) {
    Add-Content -Path $actions -Value "SecondSonPC.exe is already running - stop" -Encoding utf8
    exit 2
}
$envs = @{ "__GL_SHADER_DISK_CACHE_PATH" = $nv; "SSPC_DIAG_PIPELINE_CACHE_DRIVER" = "1"; "SSPC_SHADER_STALL_TRACE" = "1" }
$saved = @{}
foreach ($n in $envs.Keys) { $saved[$n] = [Environment]::GetEnvironmentVariable($n); [Environment]::SetEnvironmentVariable($n, $envs[$n]) }
$settings = "$Out\settings.toml" # (a settings file of its own: the defaults - precompile and the panel on)
$argList = @("--loader", "sspc", "--gpu-backend", "sspc", "--user-data", $profileDir, "--settings", $settings,
    "--shader-cache", $cacheCopy)
try {
    $p = Start-GameProcess -FilePath $Exe -ArgumentList $argList -WorkingDirectory $root
} finally {
    foreach ($n in $saved.Keys) { [Environment]::SetEnvironmentVariable($n, $saved[$n]) }
}
$t0 = Get-Date
function Log([string]$s) {
    $line = "{0,6:N1} s  {1}" -f ((Get-Date) - $t0).TotalSeconds, $s
    Add-Content -Path $actions -Value $line -Encoding utf8
    $line | Out-Host
}
function WaitUntil([double]$sec) { while (((Get-Date) - $t0).TotalSeconds -lt $sec -and -not $p.HasExited) { Start-Sleep -Milliseconds 50 } }
$script:hwnd = [IntPtr]::Zero
function Window {
    if ($script:hwnd -eq [IntPtr]::Zero) {
        $sel = Select-GameWindow -Candidates @(Get-GameWindowCandidates -Process $p)
        if ($sel.Window) { $script:hwnd = $sel.Window.Hwnd; Log ("game window 0x{0:X} ({1})" -f $script:hwnd.ToInt64(), $sel.Reason) }
    }
    $script:hwnd
}
$script:shot = 0
function Shot([string]$what) {
    $h = Window
    if ($h -eq [IntPtr]::Zero) { Log "no window yet for '$what'"; return }
    $r = New-Object SspcGameWindows+RECT
    [void][SspcGameWindows]::GetWindowRect($h, [ref]$r)
    $wd = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
    if ($wd -le 0 -or $ht -le 0) { return }
    $script:shot++
    $bmp = New-Object System.Drawing.Bitmap $wd, $ht
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
    $g.Dispose()
    $file = "$Out\shot_{0:D2}_{1}.png" -f $script:shot, $what
    $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    Log "screenshot $file ($wd x $ht)"
}
Log "started pid $($p.Id): $($argList -join ' ')"
WaitUntil 1.5; [void](Window); if ($script:hwnd -ne [IntPtr]::Zero) { Log "foreground: $([SspcSendInput]::Focus($script:hwnd))" }
WaitUntil 2.5; Shot "panel_early"
WaitUntil 4.0; Shot "panel_later"
if (-not $NoEsc) {
    WaitUntil $EscAt
    if ([SspcSendInput]::Focus((Window))) {
        [void][SspcSendInput]::Key(0x01, $false); Start-Sleep -Milliseconds 120; [void][SspcSendInput]::Key(0x01, $true)
        Log "key Esc"
    } else { Log "SKIPPED Esc (not in the foreground)" }
    Start-Sleep -Milliseconds 1500; Shot "after_esc"
}
WaitUntil $Seconds
Log "closing (WM_CLOSE)"
$stop = Stop-GameProcess -Process $p -CloseTimeoutMs 15000
Log "ended: how=$($stop.how) exit=$($stop.exit_code)"
foreach ($f in "SecondSonPC.log", "gpu_native.txt", "gpu_frames.csv") {
    if (Test-Path "$root\logs\$f") { Copy-Item "$root\logs\$f" $Out -Force }
}
Select-String -Path "$Out\SecondSonPC.log" -Pattern "shader precompiler|pipeline cache \(driver\)" -ErrorAction SilentlyContinue |
    ForEach-Object { Log ("  " + $_.Line) }
Select-String -Path "$Out\gpu_native.txt" -Pattern "start-up precompiler|pipelines on demand" -ErrorAction SilentlyContinue |
    ForEach-Object { Log ("  " + $_.Line) }
