# Drive the TEST copy: start (isolated APPDATA + stdout log), screenshot, keys (only when the game window has focus).
# Usage:  gamectl.ps1 start [A|B]      gamectl.ps1 shot [name]      gamectl.ps1 key <Name> [holdMs] [repeat]
#         gamectl.ps1 log [lines]       gamectl.ps1 stop
param([string]$cmd, [string]$a1, [string]$a2, [string]$a3)
$ErrorActionPreference = 'Stop'
$root = if ($env:CRASHCOOP_LAB) { $env:CRASHCOOP_LAB } else { Join-Path (Split-Path -Parent $PSScriptRoot) 'lab' }
$exe  = "$root\test-install\CrashBandicootNSaneTrilogy.exe"

Add-Type -AssemblyName System.Drawing, System.Windows.Forms
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class W {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int hh, bool repaint);
  [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, uint data, UIntPtr extra);
}
'@

# Instance selection: env GAMECTL_INST (A/B). Each started instance records its pid in local\pid-<inst>.txt.
$inst = if ($env:GAMECTL_INST) { $env:GAMECTL_INST } else { 'A' }
function Get-Game {
  $f = "$root\pid-$inst.txt"
  if (Test-Path $f) {
    $p = Get-Process -Id ([int](Get-Content $f)) -ErrorAction SilentlyContinue
    if ($p -and $p.Path -eq $exe) { return $p }
  }
  return $null
}
function Focus-Game($p) {
  $h = $p.MainWindowHandle
  if ($h -eq [IntPtr]::Zero) { throw 'game has no window' }
  [W]::ShowWindow($h, 9) | Out-Null; [W]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 300
  $fg = [W]::GetForegroundWindow(); $fp = 0; [W]::GetWindowThreadProcessId($fg, [ref]$fp) | Out-Null
  if ($fp -ne $p.Id) {
    # Windows blocks focus stealing from background processes: attach to the foreground thread, then switch.
    $fgPid = 0; $fgThread = [W]::GetWindowThreadProcessId($fg, [ref]$fgPid); $me = [W]::GetCurrentThreadId()
    [W]::AttachThreadInput($me, $fgThread, $true) | Out-Null
    [W]::BringWindowToTop($h) | Out-Null; [W]::SetForegroundWindow($h) | Out-Null
    [W]::AttachThreadInput($me, $fgThread, $false) | Out-Null
    Start-Sleep -Milliseconds 400
    $fg = [W]::GetForegroundWindow(); $fp = 0; [W]::GetWindowThreadProcessId($fg, [ref]$fp) | Out-Null
  }
  if ($fp -ne $p.Id) { throw "focus is not on the game (foreground pid $fp) - refusing to send keys" }
}
$vk = @{ Enter=0x0D; Esc=0x1B; Space=0x20; Up=0x26; Down=0x28; Left=0x25; Right=0x27; Backspace=0x08; Tab=0x09
         W=0x57; A=0x41; S=0x53; D=0x44; E=0x45; Q=0x51; X=0x58; Z=0x5A; C=0x43; F1=0x70; Ctrl=0xA2; Shift=0xA0; K=0x4B; L=0x4C }

switch ($cmd) {
  'start' {
    if (Get-Game) { 'already running'; break }
    $appdata = "$root\appdata-$inst"; New-Item -ItemType Directory -Force $appdata, "$root\logs" | Out-Null
    $log = "$root\logs\game-$inst.log"
    $before = @(Get-Process CrashBandicootNSaneTrilogy -ErrorAction SilentlyContinue | ForEach-Object Id)
    $c = "/c set `"APPDATA=$appdata`"&& `"$exe`" > `"$log`" 2>&1"
    Start-Process cmd.exe -ArgumentList $c -WorkingDirectory "$root\test-install" -WindowStyle Hidden
    for ($i = 0; $i -lt 40; $i++) {
      Start-Sleep -Milliseconds 250
      $new = Get-Process CrashBandicootNSaneTrilogy -ErrorAction SilentlyContinue | Where-Object { $before -notcontains $_.Id -and $_.Path -eq $exe } | Select-Object -First 1
      if ($new) { Set-Content "$root\pid-$inst.txt" $new.Id; break }
    }
    if (-not $new) { throw 'game process did not appear' }
    "started instance $inst pid $($new.Id) (APPDATA=$appdata, log=$log)"
    # side by side, never overlapping: A on the left half, B on the right half. The game resizes its window while it
    # boots, so a hidden helper places it again a few times during the first minute.
    $side = if ($inst -eq 'B') { 'right' } else { 'left' }
    $me = $MyInvocation.MyCommand.Path
    $cmd = "`$env:GAMECTL_INST='$inst'; foreach (`$t in 8,15,25,40,60) { Start-Sleep 7; try { & '$me' place $side | Out-Null } catch {} }"
    Start-Process powershell -WindowStyle Hidden -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', $cmd | Out-Null
  }
  'shot' {
    $p = Get-Game; if (-not $p) { throw 'game not running' }
    $h = $p.MainWindowHandle; $r = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $hh = $r.B - $r.T
    $bmp = New-Object Drawing.Bitmap $w, $hh; $g = [Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size); $g.Dispose()
    $scale = [Math]::Min(1.0, 960.0 / $w); $small = New-Object Drawing.Bitmap $bmp, ([int]($w*$scale)), ([int]($hh*$scale))
    $name = if ($a1) { $a1 } else { 'shot' }; New-Item -ItemType Directory -Force "$root\shots" | Out-Null
    $f = "$root\shots\$name.png"; $small.Save($f, [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose(); $small.Dispose()
    "window ${w}x${hh} at $($r.L),$($r.T) -> $f"
  }
  'key' {
    $p = Get-Game; if (-not $p) { throw 'game not running' }
    Focus-Game $p
    $code = [byte]$vk[$a1]; if (-not $code) { throw "unknown key $a1" }
    $hold = if ($a2) { [int]$a2 } else { 80 }; $rep = if ($a3) { [int]$a3 } else { 1 }
    $scan = [byte][W]::MapVirtualKey($code, 0)
    for ($i = 0; $i -lt $rep; $i++) {
      [W]::keybd_event($code, $scan, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds $hold
      [W]::keybd_event($code, $scan, 2, [UIntPtr]::Zero); Start-Sleep -Milliseconds 250
    }
    "sent $a1 x$rep (hold ${hold}ms)"
  }
  'click' {
    # click at a position given in screenshot coordinates (960-wide shots); default: window centre
    $p = Get-Game; if (-not $p) { throw 'game not running' }
    Focus-Game $p
    $r = New-Object W+RECT; [W]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $scale = [Math]::Min(1.0, 960.0 / $w)
    $x = if ($a1) { $r.L + [int]([int]$a1 / $scale) } else { $r.L + [int]($w / 2) }
    $y = if ($a2) { $r.T + [int]([int]$a2 / $scale) } else { $r.T + [int](($r.B - $r.T) / 2) }
    [System.Windows.Forms.Cursor]::Position = New-Object Drawing.Point $x, $y
    Start-Sleep -Milliseconds 150
    [W]::mouse_event(0x02, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80; [W]::mouse_event(0x04, 0, 0, 0, [UIntPtr]::Zero)
    "clicked at $x,$y"
  }
  'log' {
    $n = if ($a1) { [int]$a1 } else { 15 }
    Get-Content "$root\logs\game-$inst.log" -Tail $n -ErrorAction SilentlyContinue
  }
  'stop' { $p = Get-Game; if ($p) { Stop-Process -Id $p.Id -Force; "stopped $inst" } else { "$inst not running" } }
  'place' {
    # place window: left or right half of the primary screen
    $p = Get-Game; if (-not $p) { throw 'game not running' }
    $scr = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
    $w = [int]($scr.Width / 2); $hh = [int]($w * 9 / 16) + 40
    $x = if ($a1 -eq 'right') { $scr.X + $w } else { $scr.X }
    [W]::MoveWindow($p.MainWindowHandle, $x, $scr.Y + 60, $w, $hh, $true) | Out-Null
    "placed $inst at $x,$($scr.Y + 60) size ${w}x$hh (screen $($scr.Width)x$($scr.Height))"
  }
  default { 'usage: start|shot|key|log|stop' }
}
