# Crash N. Sane Trilogy - co-op visibility mod (experimental).
# Each player: start the game from Steam, enter a Crash 1 level, then run:
#   powershell -ExecutionPolicy Bypass -File coop.ps1 -Peer <friend's SteamID64>
# Stop:  powershell -ExecutionPolicy Bypass -File coop.ps1 -Stop
# The DLL lives only in the game's memory: nothing is written to the game folder or to the saves.
# It only accepts packets from the SteamID given with -Peer. Logs: coop-me.log next to this script.
param([string]$Peer, [switch]$Stop)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$stopFile = Join-Path $here 'coop.stop'
if ($Stop) { New-Item -ItemType File $stopFile -Force | Out-Null; 'Mod stopped.'; return }
if ($Peer -notmatch '^7656119\d{10}$') { throw 'Usage: coop.ps1 -Peer <SteamID64 of your friend, 17 digits starting with 7656119>' }

# only the game installed by Steam (the mod supports the Steam build only; copies elsewhere, e.g. test copies, are ignored)
$p = Get-Process CrashBandicootNSaneTrilogy -ErrorAction SilentlyContinue | Where-Object { $_.Path -match '\\steamapps\\' } | Select-Object -First 1
if (-not $p) { throw 'Start Crash Bandicoot N. Sane Trilogy from Steam first.' }
Remove-Item $stopFile -ErrorAction SilentlyContinue
Set-Content (Join-Path $here "coop-$($p.Id).cfg") "me steam $Peer" -Encoding ascii

# a fresh copy per injection, so the mod can be stopped and started again in the same game session
if ($here -match 'OneDrive') { Write-Warning "This folder is inside OneDrive. If loading fails, move the files to a plain folder such as C:\CrashCoop." }
Get-ChildItem $here -Filter '*.dll' | Unblock-File -ErrorAction SilentlyContinue   # files downloaded from the internet are tagged; untag them
$dll = Join-Path $here ("crash-coop-{0:yyyyMMdd-HHmmss}.dll" -f (Get-Date))
Copy-Item (Join-Path $here 'crash-coop.dll') $dll

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class CoopInj {
  [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(int a, bool i, int pid);
  [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr VirtualAllocEx(IntPtr h, IntPtr a, IntPtr n, uint t, uint p);
  [DllImport("kernel32.dll", SetLastError=true)] static extern bool WriteProcessMemory(IntPtr h, IntPtr a, byte[] b, IntPtr n, out IntPtr w);
  [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr CreateRemoteThread(IntPtr h, IntPtr sa, IntPtr ss, IntPtr fn, IntPtr arg, uint f, IntPtr tid);
  [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr h, uint ms);
  [DllImport("kernel32.dll")] static extern bool GetExitCodeThread(IntPtr h, out uint c);
  [DllImport("kernel32.dll", CharSet=CharSet.Ansi)] static extern IntPtr GetProcAddress(IntPtr m, string n);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] static extern IntPtr GetModuleHandle(string n);
  public static string Go(int pid, string path) {
    IntPtr h = OpenProcess(0x043A, false, pid); if (h == IntPtr.Zero) return "OpenProcess failed, error " + Marshal.GetLastWin32Error();
    byte[] p = Encoding.Unicode.GetBytes(path + "\0");
    IntPtr mem = VirtualAllocEx(h, IntPtr.Zero, (IntPtr)p.Length, 0x3000, 0x04); IntPtr w;
    if (mem == IntPtr.Zero) return "VirtualAllocEx failed, error " + Marshal.GetLastWin32Error();
    if (!WriteProcessMemory(h, mem, p, (IntPtr)p.Length, out w)) return "WriteProcessMemory failed, error " + Marshal.GetLastWin32Error();
    IntPtr t = CreateRemoteThread(h, IntPtr.Zero, IntPtr.Zero, GetProcAddress(GetModuleHandle("kernel32.dll"), "LoadLibraryW"), mem, 0, IntPtr.Zero);
    if (t == IntPtr.Zero) return "CreateRemoteThread failed, error " + Marshal.GetLastWin32Error() + " (antivirus?)";
    WaitForSingleObject(t, 10000); uint code; GetExitCodeThread(t, out code);
    return code != 0 ? "OK" : "LoadLibrary returned NULL inside the game (DLL blocked by antivirus, or not readable: OneDrive folder?)";
  }
}
'@
if (-not (Test-Path $dll)) { throw "The DLL copy disappeared right away: the antivirus probably quarantined it ($dll)." }
$r = [CoopInj]::Go($p.Id, $dll)
if ($r -eq 'OK') { "Mod loaded. Waiting for $Peer ... (status: $here\coop-me.log)" } else { throw "Injection failed: $r" }
