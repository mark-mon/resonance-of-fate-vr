# ROF (Resonance of Fate 4K/HD Edition) VR test toolkit. Dot-source from agent commands:
#   . D:\vrconversions\resonanceoffate\agent\tools\rof.ps1
# Adapted from the Star Ocean The Divine Force kit (D:\vrconversions\staroceandivineforce\agent\tools\so6.ps1).
# Everything here only touches: the game folder, the mod build folder, the agent folder, and (for the
# null-headset switch) SteamVR's steamvr.vrsettings, which is backed up first and can be restored.

function Get-ROFAppId {
    # Reads the Steam app id from the library's appmanifest (so it never has to be guessed).
    $lib = "D:\SteamLibrary\steamapps"
    foreach ($m in Get-ChildItem $lib -Filter "appmanifest_*.acf" -ErrorAction SilentlyContinue) {
        $t = Get-Content $m.FullName -Raw
        if ($t -match '"installdir"\s+"Resonance of Fate"' -and $t -match '"appid"\s+"(\d+)"') { return [int]$Matches[1] }
    }
    return 0
}
$Global:ROF = @{
    Game     = "D:\SteamLibrary\steamapps\common\Resonance of Fate"
    Build    = "D:\vrconversions\resonanceoffate\rofvr\build"
    Agent    = "D:\vrconversions\resonanceoffate\agent"
    Captures = "D:\vrconversions\resonanceoffate\agent\captures"
    AppId    = (Get-ROFAppId)
    Exe      = "Resonance of Fate"
    Proxy    = "xinput1_3.dll"      # imported statically (ordinals 2,3,4); the mod is a proxy for it
    Log      = "rofvr.log"
}
New-Item -ItemType Directory -Force $ROF.Captures | Out-Null

Add-Type -AssemblyName System.Drawing, System.Windows.Forms
if (-not ("ROFNative" -as [type])) {
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class ROFNative {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref System.Drawing.Point p);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint mapType);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public KEYBDINPUT ki; public long pad; }
    [DllImport("user32.dll", SetLastError = true)] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
    public static void Key(ushort vk, bool up) {
        ushort scan = (ushort)MapVirtualKey(vk, 0);
        INPUT[] i = new INPUT[1];
        i[0].type = 1; // keyboard
        i[0].ki.wVk = 0; i[0].ki.wScan = scan;
        i[0].ki.dwFlags = 0x0008u | (up ? 0x0002u : 0u); // KEYEVENTF_SCANCODE | KEYUP
        SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }
}
"@ -ReferencedAssemblies System.Drawing
}

# ---------------------------------------------------------------- game control
function Get-ROFProcess { Get-Process $ROF.Exe -ErrorAction SilentlyContinue }

function Stop-ROF {
    # Ask the game to close first (lets Steam see a normal exit), then force it if needed.
    $p = Get-ROFProcess
    if (-not $p) { return "game not running" }
    $p | ForEach-Object { $_.CloseMainWindow() | Out-Null }
    for ($i = 0; $i -lt 15 -and (Get-ROFProcess); $i++) { Start-Sleep 1 }
    if (Get-ROFProcess) { Get-ROFProcess | Stop-Process -Force; Start-Sleep 3; "game force-stopped" } else { "game closed" }
    Start-Sleep 3
}

function Install-ROFMod {
    # Copies the current mod build (DLL, PDB, OpenXR loader) into the game folder.
    foreach ($n in $ROF.Proxy, ($ROF.Proxy -replace "\.dll$", ".pdb"), "openxr_loader.dll") {
        $src = Join-Path $ROF.Build $n
        if (Test-Path $src) { Copy-Item $src (Join-Path $ROF.Game $n) -Force; "installed $n $((Get-Item $src).Length) bytes" }
    }
}

function Set-ROFConfig([string[]]$Set) {
    # Set-ROFConfig "RenderMode = Mono","Reprojection = Mod"  (adds or replaces keys in config.txt)
    $cfgPath = Join-Path $ROF.Game "config.txt"
    $cfg = @(if (Test-Path $cfgPath) { Get-Content $cfgPath } else { @() })
    foreach ($kv in $Set) {
        $k = $kv.Split('=')[0].Trim()
        $found = $false
        $cfg = @($cfg | ForEach-Object { if ($_ -match "^\s*$([regex]::Escape($k))\s*=") { $found = $true; $kv } else { $_ } })
        if (-not $found) { $cfg += $kv }
    }
    $cfg | Set-Content $cfgPath
    "config: $($Set -join '; ')"
}

function Start-ROF([int]$WaitSec = 0, [switch]$Direct) {
    # Launch through Steam (default) or run ROF.exe directly with the Steam app id set (Steam must be running).
    # If the Steam launch shows no window within 45 s, falls back to a direct start.
    $Global:ROFClicked = $false
    $t0 = Get-Date
    function Wait-Window([int]$sec) {
        $t1 = Get-Date
        while (((Get-Date) - $t1).TotalSeconds -lt $sec) {
            $p = Get-ROFProcess
            if ($p -and $p.MainWindowHandle -ne 0) { return $p }
            Start-Sleep 1
        }
        return $null
    }
    if (-not $Direct) {
        Start-Process "steam://rungameid/$($ROF.AppId)"
        "launch requested via Steam"
        if ($WaitSec -le 0) { return }
        $p = Wait-Window ([Math]::Min(45, $WaitSec))
        if ($p) { "game window up after $([int]((Get-Date)-$t0).TotalSeconds)s (pid $($p.Id))"; return }
        if (Get-ROFProcess) { $p = Wait-Window $WaitSec; if ($p) { "game window up after $([int]((Get-Date)-$t0).TotalSeconds)s (pid $($p.Id))" } else { "WARNING: game process running but no window" }; return }
        "Steam launch did not start the game; starting ROF.exe directly"
    }
    $env:SteamAppId = "$($ROF.AppId)"; $env:SteamGameId = "$($ROF.AppId)"
    Start-Process -FilePath (Join-Path $ROF.Game "$($ROF.Exe).exe") -WorkingDirectory $ROF.Game
    "direct start requested"
    if ($WaitSec -gt 0) {
        $p = Wait-Window $WaitSec
        if ($p) { "game window up after $([int]((Get-Date)-$t0).TotalSeconds)s (pid $($p.Id))" } else { "WARNING: game window not seen within $WaitSec s" }
    }
}

function Get-ROFLog([int]$Tail = 0, [string]$Pattern = "") {
    $log = Join-Path $ROF.Game $ROF.Log
    if (-not (Test-Path $log)) { return "no $($ROF.Log)" }
    # The game keeps the log open for writing, so open it with shared read/write access.
    $fs = [System.IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
    try { $sr = New-Object System.IO.StreamReader($fs); $text = $sr.ReadToEnd() } finally { $fs.Close() }
    $lines = $text -split "`r?`n"
    if ($Pattern) { $lines = $lines | Select-String -Pattern $Pattern | ForEach-Object { $_.Line } }
    if ($Tail -gt 0) { $lines | Select-Object -Last $Tail } else { $lines }
}

function Save-ROFLog([string]$Name) {
    $log = Join-Path $ROF.Game $ROF.Log
    $dst = Join-Path $ROF.Captures "$Name.log"
    if (Test-Path $log) { (Get-ROFLog) | Set-Content -Path $dst -Encoding UTF8; "log saved to $dst" } else { "no log" }
}

function Uninstall-ROFMod {
    # Removes the mod DLLs from the game folder (leaves config and logs).
    foreach ($n in $ROF.Proxy, ($ROF.Proxy -replace "\.dll$", ".pdb"), "openxr_loader.dll") {
        $p = Join-Path $ROF.Game $n; if (Test-Path $p) { Remove-Item $p -Force; "removed $n" }
    }
}

# ---------------------------------------------------------------- input
$Global:ROFKeys = @{
    Enter = 0x0D; Esc = 0x1B; Space = 0x20; Up = 0x26; Down = 0x28; Left = 0x25; Right = 0x27; Tab = 0x09
    W = 0x57; A = 0x41; S = 0x53; D = 0x44; E = 0x45; Q = 0x51; F = 0x46; R = 0x52; Backspace = 0x08
    F8 = 0x77; F9 = 0x78; F10 = 0x79; Insert = 0x2D; Delete = 0x2E; PageUp = 0x21; PageDown = 0x22
    F2 = 0x71; F3 = 0x72; F5 = 0x74; F6 = 0x75; F7 = 0x76; F11 = 0x7A; F12 = 0x7B
    NumPad0 = 0x60; NumPad1 = 0x61; NumPad2 = 0x62; NumPad3 = 0x63; NumPad4 = 0x64; NumPad5 = 0x65
    NumPad6 = 0x66; NumPad7 = 0x67; NumPad8 = 0x68; NumPad9 = 0x69
}
# Fake gamepad (mod config DebugFakePad = 2; the mod forces the game's pad object connected):
#   F2 Start, F3 Back, NumPad1 A, NumPad0 B, NumPad3 X, NumPad5 Y, F5 RB, F6 LB, F7 RT,
#   NumPad8/2/4/6 left stick, NumPad7/9 right stick X, I/K/J/L D-pad.  Send with Send-ROFVKey.
function Pad([string]$Button, [int]$HoldMs = 150, [int]$Count = 1, [int]$GapMs = 400) {
    $map = @{ Start = "F2"; Back = "F3"; A = "NumPad1"; B = "NumPad0"; X = "NumPad3"; Y = "NumPad5"; RB = "F5"; LB = "F6"; RT = "F7"
              Up = "NumPad8"; Down = "NumPad2"; Left = "NumPad4"; Right = "NumPad6" }
    $k = $map[$Button]; if (-not $k) { return "unknown pad button $Button" }
    Focus-ROF | Out-Null
    Send-ROFVKey $k -HoldMs $HoldMs -Count $Count -GapMs $GapMs
}
if (-not ('ROFMouse' -as [type])) { Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public static class ROFMouse { [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y); [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e); public static void Click(bool right) { mouse_event(right ? 8u : 2u, 0, 0, 0, UIntPtr.Zero); System.Threading.Thread.Sleep(80); mouse_event(right ? 16u : 4u, 0, 0, 0, UIntPtr.Zero); } }' }
function Focus-ROF([switch]$NoClick) {
    # SetForegroundWindow alone does not always give the game keyboard focus (then key presses are lost), so
    # also click the window's title bar (non-client area: no game input).
    $p = Get-ROFProcess
    if (-not $p -or $p.MainWindowHandle -eq 0) { return "game window not found" }
    [ROFNative]::ShowWindow($p.MainWindowHandle, 9) | Out-Null
    [ROFNative]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
    if (-not $NoClick -and [ROFNative]::GetForegroundWindow() -ne $p.MainWindowHandle -or -not $Global:ROFClicked) {
        $r = New-Object ROFNative+RECT; [ROFNative]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
        [ROFMouse]::SetCursorPos([int](($r.Left + $r.Right) / 2), $r.Top + 12) | Out-Null
        [ROFMouse]::Click($false); $Global:ROFClicked = $true
    }
    Start-Sleep -Milliseconds 300
    "focused game window"
}
function Send-ROFKey([string]$Key, [int]$HoldMs = 80, [int]$Count = 1, [int]$GapMs = 400) {
    # Sends scan-code key presses to the foreground window (focus the game first).
    $vk = $ROFKeys[$Key]
    if (-not $vk) { if ($Key.Length -eq 1) { $vk = [int][char]$Key.ToUpper() } else { return "unknown key $Key" } }
    for ($i = 0; $i -lt $Count; $i++) {
        [ROFNative]::Key([uint16]$vk, $false); Start-Sleep -Milliseconds $HoldMs
        [ROFNative]::Key([uint16]$vk, $true);  Start-Sleep -Milliseconds $GapMs
    }
    "sent $Key x$Count"
}

if (-not ('ROFVKey' -as [type])) {
Add-Type -TypeDefinition @"
using System; using System.Runtime.InteropServices;
public static class ROFVKey {
    [StructLayout(LayoutKind.Sequential)] struct KI { public ushort vk; public ushort scan; public uint flags; public uint time; public IntPtr extra; }
    [StructLayout(LayoutKind.Explicit)] struct IN { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KI ki; [FieldOffset(8)] public long pad0; [FieldOffset(16)] public long pad1; [FieldOffset(24)] public long pad2; [FieldOffset(32)] public long pad3; }
    [DllImport("user32.dll")] static extern uint SendInput(uint n, IN[] i, int size);
    [DllImport("user32.dll")] static extern uint MapVirtualKey(uint code, uint type);
    public static void Key(ushort vk, bool up, bool ext) {
        IN[] i = new IN[1];
        i[0].type = 1; i[0].ki.vk = vk; i[0].ki.scan = (ushort)MapVirtualKey(vk, 0);
        i[0].ki.flags = (ext ? 0x0001u : 0u) | (up ? 0x0002u : 0u);
        SendInput(1, i, Marshal.SizeOf(typeof(IN)));
    }
}
"@
}
function Send-ROFVKey([string]$Key, [int]$HoldMs = 80, [int]$Count = 1, [int]$GapMs = 300) {
    # Virtual-key presses (for code that polls GetAsyncKeyState, e.g. the mod's menu). Navigation keys are
    # sent as extended keys so they are not mistaken for the numeric keypad.
    $vk = $ROFKeys[$Key]
    if (-not $vk) { return "unknown key $Key" }
    $ext = @(0x21,0x22,0x25,0x26,0x27,0x28,0x2D,0x2E) -contains $vk
    for ($i = 0; $i -lt $Count; $i++) {
        [ROFVKey]::Key([uint16]$vk, $false, $ext); Start-Sleep -Milliseconds $HoldMs
        [ROFVKey]::Key([uint16]$vk, $true, $ext);  Start-Sleep -Milliseconds $GapMs
    }
    "sent vkey $Key x$Count"
}

function Test-ROFLoadScreen([string]$Png) {
    # The LOAD screen has an orange "LOAD" header at the top left (window coordinates ~80-310, 90-135).
    $b = [System.Drawing.Bitmap]::FromFile($Png); $n = 0
    for ($y = 90; $y -lt 136 -and $y -lt $b.Height; $y += 3) { for ($x = 80; $x -lt 310 -and $x -lt $b.Width; $x += 3) {
        $c = $b.GetPixel($x, $y); if ($c.R -gt 190 -and $c.G -gt 100 -and $c.G -lt 200 -and $c.B -lt 90) { $n++ } } }
    $b.Dispose(); return ($n -gt 60)
}
function Enter-ROFGame([int]$LoadWaitSec = 40) {
    # From a fresh start to gameplay with save slot Data 1 (tests 010-015):
    #   Enter skips the intro movie / logo; the title takes Enter only after a while (not consistent), so Enter
    #   (virtual key) is repeated until the LOAD screen is seen; Data 1 -> "Load save data?" (cursor on No) -> Up, Enter.
    $Global:ROFClicked = $false
    Start-Sleep 18
    Focus-ROF | Out-Null; Send-ROFKey Enter | Out-Null; Start-Sleep 4      # skip intro movie
    Focus-ROF | Out-Null; Send-ROFKey Enter | Out-Null; Start-Sleep 6      # logo -> title
    $ok = $false; $tmp = Join-Path $ROF.Captures "_nav.png"
    # Known to work (tests 010, 013): click + right-click in the client area, long Enter, long Space, then
    # Enter as a virtual key. (Enter alone at the title is ignored, even 20 times - test 015.)
    $p = Get-ROFProcess; $r = New-Object ROFNative+RECT; [ROFNative]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
    $cx = [int](($r.Left + $r.Right) / 2); $cy = [int](($r.Top + $r.Bottom) / 2)
    for ($i = 0; $i -lt 6 -and -not $ok; $i++) {
        Focus-ROF | Out-Null
        [ROFMouse]::SetCursorPos($cx, $cy) | Out-Null; [ROFMouse]::Click($false); Start-Sleep -Milliseconds 1500
        [ROFMouse]::Click($true); Start-Sleep -Milliseconds 1500
        Send-ROFKey Enter -HoldMs 600 | Out-Null; Start-Sleep -Milliseconds 1500
        Send-ROFKey Space -HoldMs 600 | Out-Null; Start-Sleep -Milliseconds 1500
        Send-ROFVKey Enter -HoldMs 200 | Out-Null; Start-Sleep 3
        Save-ROFShot "_nav" | Out-Null
        if (Test-Path $tmp) { $ok = Test-ROFLoadScreen $tmp }
    }
    if (-not $ok) { return "Enter-ROFGame: LOAD screen NOT reached" }
    "LOAD screen after $i tries"
    # Click = confirm: Data 1 (already selected) -> "Load save data?" (cursor on No) -> W -> click.
    Focus-ROF | Out-Null
    [ROFMouse]::SetCursorPos($r.Left + 650, $r.Top + 215) | Out-Null; Start-Sleep -Milliseconds 400; [ROFMouse]::Click($false); Start-Sleep 2
    Save-ROFShot "_nav_dialog" | Out-Null
    # A left click acts as "confirm" (A) wherever it lands; WASD are the directions (W = up).
    Focus-ROF | Out-Null; Send-ROFKey W -HoldMs 150 | Out-Null; Start-Sleep 1          # No -> Yes
    Save-ROFShot "_nav_yes" | Out-Null
    [ROFMouse]::Click($false); Start-Sleep 8                                            # loading
    Focus-ROF | Out-Null; [ROFMouse]::Click($false); Start-Sleep 1                      # "Load complete."
    [ROFMouse]::SetCursorPos($r.Left + 30, $r.Top + 12) | Out-Null   # park the cursor on the title bar
    Start-Sleep $LoadWaitSec
    "Enter-ROFGame done"
}

# ---------------------------------------------------------------- screenshots
function Save-Screen([string]$Name) {
    # Whole primary screen (shows whatever is on top, e.g. the SteamVR mirror window).
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
    $path = Join-Path $ROF.Captures "$Name.png"
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
    "screen saved to $path"
}
function Save-WindowShot([IntPtr]$Hwnd, [string]$Name, [int]$MaxWidth = 1920) {
    # Captures one window even if covered (PrintWindow with PW_RENDERFULLCONTENT for DX content).
    $r = New-Object ROFNative+RECT
    [ROFNative]::GetWindowRect($Hwnd, [ref]$r) | Out-Null
    $w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
    if ($w -le 0 -or $h -le 0) { return "window has no size" }
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc(); [ROFNative]::PrintWindow($Hwnd, $hdc, 2) | Out-Null; $g.ReleaseHdc($hdc); $g.Dispose()
    if ($w -gt $MaxWidth) {
        $nh = [int]($h * $MaxWidth / $w)
        $small = New-Object System.Drawing.Bitmap $bmp, $MaxWidth, $nh
        $bmp.Dispose(); $bmp = $small
    }
    $path = Join-Path $ROF.Captures "$Name.png"
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    "window saved to $path"
}
function Save-ROFShot([string]$Name) {
    $p = Get-ROFProcess
    if (-not $p -or $p.MainWindowHandle -eq 0) { return "game window not found" }
    Save-WindowShot $p.MainWindowHandle $Name
}
function Get-Windows([string]$Like = "*") {
    Get-Process | Where-Object { $_.MainWindowHandle -ne 0 -and $_.MainWindowTitle -like $Like } |
        Select-Object Id, ProcessName, MainWindowTitle, MainWindowHandle | Format-Table -AutoSize | Out-String -Width 300
}

# ---------------------------------------------------------------- SteamVR null headset
function Get-SteamPath { (Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction SilentlyContinue).SteamPath -replace '/', '\' }
function Get-VrSettingsPath { Join-Path (Get-SteamPath) "config\steamvr.vrsettings" }

function Enable-NullHmd([int]$RenderW = 1832, [int]$RenderH = 1920, [int]$Hz = 90) {
    # Switches SteamVR to its built-in "null" headset (no real HMD needed). Backs up the settings first.
    Get-Process vrserver, vrcompositor, vrmonitor -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 2
    $path = Get-VrSettingsPath
    $bak = "$path.rofbak"
    if (-not (Test-Path $bak)) { Copy-Item $path $bak; "backed up $path -> $bak" }
    $j = Get-Content $path -Raw | ConvertFrom-Json
    if (-not $j.steamvr) { $j | Add-Member -NotePropertyName steamvr -NotePropertyValue ([pscustomobject]@{}) }
    foreach ($kv in @{ requireHmd = $false; forcedDriver = "null"; activateMultipleDrivers = $true; enableHomeApp = $false }.GetEnumerator()) {
        $j.steamvr | Add-Member -NotePropertyName $kv.Key -NotePropertyValue $kv.Value -Force
    }
    $null_ = [pscustomobject]@{
        enable = $true; serialNumber = "Null 4711"; modelNumber = "Null Model"
        windowX = 0; windowY = 0; windowWidth = 1600; windowHeight = 900
        renderWidth = $RenderW; renderHeight = $RenderH; secondsFromVsyncToPhotons = 0.011; displayFrequency = $Hz
    }
    $j | Add-Member -NotePropertyName driver_null -NotePropertyValue $null_ -Force
    # No SteamVR dashboard: with no controllers it pops up over the game and hides its layers.
    if (-not $j.dashboard) { $j | Add-Member -NotePropertyName dashboard -NotePropertyValue ([pscustomobject]@{}) }
    $j.dashboard | Add-Member -NotePropertyName enableDashboard -NotePropertyValue $false -Force
    $j.dashboard | Add-Member -NotePropertyName autoShowGameTheater -NotePropertyValue $false -Force
    $j | ConvertTo-Json -Depth 20 | Set-Content $path -Encoding UTF8
    "SteamVR set to null headset ($RenderW x $RenderH per eye, $Hz Hz). Restore with Restore-RealHmd."
}

function Restore-RealHmd {
    # Puts back the user's original steamvr.vrsettings (Pimax setup).
    Get-Process vrserver, vrcompositor, vrmonitor -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 2
    $path = Get-VrSettingsPath
    $bak = "$path.rofbak"
    if (Test-Path $bak) { Copy-Item $bak $path -Force; Remove-Item $bak; "restored original $path" } else { "no backup found; nothing to restore" }
}

function Get-SteamVrState {
    $procs = Get-Process vrserver, vrcompositor, vrmonitor -ErrorAction SilentlyContinue | Select-Object ProcessName, Id
    $j = Get-Content (Get-VrSettingsPath) -Raw | ConvertFrom-Json
    "forcedDriver=$($j.steamvr.forcedDriver) requireHmd=$($j.steamvr.requireHmd) null.enable=$($j.driver_null.enable) backup=$(Test-Path ((Get-VrSettingsPath) + '.rofbak'))"
    $procs | Format-Table -AutoSize | Out-String
}

"rof toolkit loaded (game: $($ROF.Game))"
