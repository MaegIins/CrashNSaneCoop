# Inject a DLL into the TEST copy (instance A/B) with CreateRemoteThread(LoadLibraryW). Refuses any other process.
# A fresh copy of the DLL is made per injection so it can be loaded again later.
param([string]$Inst = 'A', [string]$Dll)
$ErrorActionPreference = 'Stop'
$root = if ($env:CRASHCOOP_LAB) { $env:CRASHCOOP_LAB } else { Join-Path (Split-Path -Parent $PSScriptRoot) 'lab' }; $exe = "$root\test-install\CrashBandicootNSaneTrilogy.exe"
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class Inj {
  [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(int a, bool i, int pid);
  [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr VirtualAllocEx(IntPtr h, IntPtr a, IntPtr n, uint t, uint p);
  [DllImport("kernel32.dll", SetLastError=true)] static extern bool WriteProcessMemory(IntPtr h, IntPtr a, byte[] b, IntPtr n, out IntPtr w);
  [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr CreateRemoteThread(IntPtr h, IntPtr sa, IntPtr ss, IntPtr fn, IntPtr arg, uint f, IntPtr tid);
  [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr h, uint ms);
  [DllImport("kernel32.dll")] static extern bool GetExitCodeThread(IntPtr h, out uint c);
  [DllImport("kernel32.dll", CharSet=CharSet.Ansi)] static extern IntPtr GetProcAddress(IntPtr m, string n);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] static extern IntPtr GetModuleHandle(string n);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  public static string Go(int pid, string path) {
    IntPtr h = OpenProcess(0x0002 | 0x0008 | 0x0010 | 0x0020 | 0x0400, false, pid);
    if (h == IntPtr.Zero) return "OpenProcess failed " + Marshal.GetLastWin32Error();
    byte[] p = Encoding.Unicode.GetBytes(path + "\0");
    IntPtr mem = VirtualAllocEx(h, IntPtr.Zero, (IntPtr)p.Length, 0x3000, 0x04);
    IntPtr w; WriteProcessMemory(h, mem, p, (IntPtr)p.Length, out w);
    IntPtr ll = GetProcAddress(GetModuleHandle("kernel32.dll"), "LoadLibraryW");
    IntPtr t = CreateRemoteThread(h, IntPtr.Zero, IntPtr.Zero, ll, mem, 0, IntPtr.Zero);
    if (t == IntPtr.Zero) return "CreateRemoteThread failed " + Marshal.GetLastWin32Error();
    WaitForSingleObject(t, 10000); uint code; GetExitCodeThread(t, out code);
    CloseHandle(t); CloseHandle(h);
    return code != 0 ? "loaded (module handle low bits 0x" + code.ToString("X") + ")" : "LoadLibraryW returned NULL";
  }
}
'@
$p = Get-Process -Id ([int](Get-Content "$root\pid-$Inst.txt")); if ($p.Path -ne $exe) { throw "instance $Inst is not the test copy" }
$copy = Join-Path (Split-Path $Dll) ("{0}-{1:yyyyMMdd-HHmmss}.dll" -f [IO.Path]::GetFileNameWithoutExtension($Dll), (Get-Date))
Copy-Item $Dll $copy
"injecting $copy into instance $Inst (pid $($p.Id))"
[Inj]::Go($p.Id, $copy)
