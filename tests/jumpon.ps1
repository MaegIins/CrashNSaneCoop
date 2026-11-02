# TEST COPY ONLY: drop the player 120 units above a point (z up) and press jump once landed
param([string]$Inst = 'A', [double]$X, [double]$Y, [double]$Z)
$root = if ($env:CRASHCOOP_LAB) { $env:CRASHCOOP_LAB } else { Join-Path (Split-Path -Parent $PSScriptRoot) 'lab' }; $exe = "$root\test-install\CrashBandicootNSaneTrilogy.exe"
Add-Type -TypeDefinition 'using System;using System.Runtime.InteropServices;public static class JO{[DllImport("kernel32.dll")]public static extern IntPtr OpenProcess(int a,bool i,int p);[DllImport("kernel32.dll")]public static extern bool ReadProcessMemory(IntPtr h,IntPtr a,byte[] b,IntPtr n,out IntPtr r);[DllImport("kernel32.dll")]public static extern bool WriteProcessMemory(IntPtr h,IntPtr a,byte[] b,IntPtr n,out IntPtr w);public static long Q(IntPtr h,long a){var b=new byte[8];IntPtr r;ReadProcessMemory(h,(IntPtr)a,b,(IntPtr)8,out r);return BitConverter.ToInt64(b,0);}}'
$p = Get-Process -Id ([int](Get-Content "$root\pid-$Inst.txt")); if ($p.Path -ne $exe) { throw 'not the test copy' }
$h = [JO]::OpenProcess(0x438, $false, $p.Id); $b = $p.MainModule.BaseAddress.ToInt64()
$mgr = [JO]::Q($h, $b + 0x1A665E8); $act = [JO]::Q($h, [JO]::Q($h, [JO]::Q($h, [JO]::Q($h, [JO]::Q($h, $mgr + 0x10) + 0x20)) + 0x10) + 0x28)
$pos = [BitConverter]::GetBytes([single]$X) + [BitConverter]::GetBytes([single]$Y) + [BitConverter]::GetBytes([single]($Z + 120)); $w = [IntPtr]0
for ($i = 0; $i -lt 3; $i++) { [JO]::WriteProcessMemory($h, [IntPtr]($act + 0x20), $pos, [IntPtr]12, [ref]$w) | Out-Null
  $f = [BitConverter]::GetBytes(([uint32]([JO]::Q($h, $act + 0x38) -band 0xFFFFFFFF)) -bor 0x400); [JO]::WriteProcessMemory($h, [IntPtr]($act + 0x38), $f, [IntPtr]4, [ref]$w) | Out-Null; Start-Sleep -Milliseconds 20 }
Start-Sleep 2; $env:GAMECTL_INST = $Inst; & "$root\scripts\gamectl.ps1" key Space 120 1 | Out-Null
