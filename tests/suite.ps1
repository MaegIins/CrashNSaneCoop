# Two-client automated suite for the Crash N. Sane co-op mod (TEST COPIES ONLY).
# Starts two copies of the game (A left, B right), enters Crash 1 N. Sanity Beach in both, injects the mod (UDP
# between the copies), then drives A and checks what B's game and both mod logs say:
#   1 presence   both puppets spawned, friend visible on both sides
#   2 crate      A jumps on a crate -> A sends it, B breaks its own copy (B's crate component is freed)
#   3 fruit      A's own fruit count goes up, B's counter receives it
#   4 fall       A drops below the level -> B parks the puppet (no "puppet gone"), then shows it again
#   5 level      A quits to the map and comes back -> both games alive, A's puppet spawned again
# Invariants checked at the end: both games alive, no FAULT line in the mod logs, no Windows error dialog.
# Prints one "RESULT: PASS" or "RESULT: FAIL" line (the scorer self-test runs first).
param([string]$Dll = "", [switch]$KeepRunning,
      [string]$Level = 'crash1/l101_nsanitybeach/l101_nsanitybeach',   # any level, entered with the game's "loadmap" console command
      [string]$CrateA = '-8.2,-142.1,-46.2', [string]$CrateB = '-4.9,121.6,-67.2;148,-53.8,-56.8',   # A breaks CrateA; B breaks CrateB crates (x,y,z;x,y,z) until its fruit counter is found
      [string]$LoadmapDll = "")
$ErrorActionPreference = 'Stop'
$root = if ($env:CRASHCOOP_LAB) { $env:CRASHCOOP_LAB } else { Join-Path (Split-Path -Parent $PSScriptRoot) 'lab' }
$g = "$PSScriptRoot\gamectl.ps1"; $modDir = "$root\mod"; if (-not $Dll) { $Dll = "$modDir\crash-coop.dll" }; if (-not $LoadmapDll) { $LoadmapDll = "$modDir\loadmap.dll" }
$results = New-Object System.Collections.ArrayList
function Check($name, [bool]$ok, $detail) { [void]$results.Add([pscustomobject]@{ Name = $name; Ok = $ok; Detail = $detail }); '{0}  {1}  {2}' -f ($(if ($ok) { 'PASS' } else { 'FAIL' })), $name, $detail }

# ---- scorer: the only place that decides PASS/FAIL ----
function Score($rows, [int]$expected) {
    if (-not $rows -or $rows.Count -lt $expected) { return 'FAIL' }   # a check that never ran is a failure
    if (@($rows | Where-Object { -not $_.Ok }).Count) { return 'FAIL' }
    'PASS'
}
# scorer self-test: known bad inputs must FAIL, a known good one must PASS
$self = @(
    (Score @() 1) -eq 'FAIL'
    (Score @([pscustomobject]@{ Ok = $true }) 2) -eq 'FAIL'
    (Score @([pscustomobject]@{ Ok = $true }, [pscustomobject]@{ Ok = $false }) 2) -eq 'FAIL'
    (Score @([pscustomobject]@{ Ok = $true }, [pscustomobject]@{ Ok = $true }) 2) -eq 'PASS'
)
if ($self -contains $false) { 'scorer self-test failed'; 'RESULT: FAIL'; exit 1 }
'scorer self-test ok'

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class ST {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(int a, bool i, int pid);
  [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr h, IntPtr a, byte[] b, IntPtr n, out IntPtr r);
  [DllImport("kernel32.dll")] public static extern bool WriteProcessMemory(IntPtr h, IntPtr a, byte[] b, IntPtr n, out IntPtr w);
  public static long Q(IntPtr h, long a) { var b = new byte[8]; IntPtr r; ReadProcessMemory(h, (IntPtr)a, b, (IntPtr)8, out r); return BitConverter.ToInt64(b, 0); }
  public static int Dialogs(uint pid) { int n = 0; EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid && IsWindowVisible(h)) { var s = new StringBuilder(64); GetClassName(h, s, 64); if (s.ToString() == "#32770") n++; } return true; }, IntPtr.Zero); return n; }
}
'@
function GamePid($i) { [int](Get-Content "$root\pid-$i.txt") }
function Alive($i) { [bool](Get-Process -Id (GamePid $i) -ErrorAction SilentlyContinue) }
function ModLog($i) { Get-Content "$modDir\coop-$i.log" -ErrorAction SilentlyContinue }
function GameLog($i) { Get-Content "$root\logs\game-$i.log" -ErrorAction SilentlyContinue }
function WaitFor($what, [scriptblock]$cond, [int]$sec) { for ($t = 0; $t -lt $sec * 2; $t++) { if (& $cond) { return $true }; Start-Sleep -Milliseconds 500 }; $false }
function Player($i) { $p = Get-Process -Id (GamePid $i); $h = [ST]::OpenProcess(0x438, $false, $p.Id); $b = $p.MainModule.BaseAddress.ToInt64()
    $m = [ST]::Q($h, $b + 0x1A665E8); @{ H = $h; A = [ST]::Q($h, [ST]::Q($h, [ST]::Q($h, [ST]::Q($h, [ST]::Q($h, $m + 0x10) + 0x20)) + 0x10) + 0x28) } }
function Teleport($i, $x, $y, $z) { $pl = Player $i; $w = [IntPtr]0
    $pos = [BitConverter]::GetBytes([single]$x) + [BitConverter]::GetBytes([single]$y) + [BitConverter]::GetBytes([single]$z)
    for ($k = 0; $k -lt 3; $k++) { [ST]::WriteProcessMemory($pl.H, [IntPtr]($pl.A + 0x20), $pos, [IntPtr]12, [ref]$w) | Out-Null
        $f = [BitConverter]::GetBytes(([uint32]([ST]::Q($pl.H, $pl.A + 0x38) -band 0xFFFFFFFF)) -bor 0x400); [ST]::WriteProcessMemory($pl.H, [IntPtr]($pl.A + 0x38), $f, [IntPtr]4, [ref]$w) | Out-Null; Start-Sleep -Milliseconds 20 } }

try {
# ---- start both copies and enter N. Sanity Beach ----
foreach ($i in 'A', 'B') { $env:GAMECTL_INST = $i; & $g stop | Out-Null }
Start-Sleep 5
$env:GAMECTL_INST = 'A'; & $g start | Out-Null; Start-Sleep 25; $env:GAMECTL_INST = 'B'; & $g start | Out-Null; Start-Sleep 30
$short = ($Level -split '/')[1]
$ic = [Globalization.CultureInfo]::InvariantCulture; $ca = @($CrateA -split ',' | % { [double]::Parse($_, $ic) }); $cbs = @($CrateB -split ';' | % { ,@($_ -split ',' | % { [double]::Parse($_, $ic) }) })
function CrateRx($c) { ([string]::Format([Globalization.CultureInfo]::InvariantCulture, '{0:F1} {1:F1} {2:F1}', $c[0], $c[1], $c[2])) -replace '\.', '\.' }   # the mod logs with a dot
function LoadLevel($i) {   # the game's own console command, sent on the game thread by loadmap.dll
    Set-Content (Join-Path (Split-Path $LoadmapDll) 'loadmap.cmd') $Level; & "$PSScriptRoot\inject.ps1" -Inst $i -Dll $LoadmapDll | Out-Null }
function LevelLoads($i) { @(GameLog $i | Select-String "LOADING LEVEL: '$Level'").Count }
foreach ($i in 'A', 'B') {
    $env:GAMECTL_INST = $i
    for ($try = 0; $try -lt 3; $try++) {
        if (-not (Alive $i)) { & $g start | Out-Null; Start-Sleep 30 }   # a copy can die on its own while starting (seen: exe+0x9945FF, no mod loaded)
        try {
        & $g key Enter | Out-Null; Start-Sleep 10; & $g key Enter | Out-Null; Start-Sleep 4
        & $g click 480 475 | Out-Null; Start-Sleep 8                                                    # main menu
        LoadLevel $i; Start-Sleep 25
        } catch { "  setup $i try ${try}: $_"; continue }
        if ((GameLog $i | Select-String "Loaded chunk '${short}:GlobalChunk'")) { break }
        & $g stop | Out-Null; Start-Sleep 5; & $g start | Out-Null; Start-Sleep 30
    }
}
$inBeach = @('A', 'B' | Where-Object { GameLog $_ | Select-String "Loaded chunk '${short}:GlobalChunk'" }).Count -eq 2
Check "setup: both copies in $short" $inBeach '' | Write-Output
if (-not $inBeach) { 'RESULT: FAIL'; exit 1 }

# ---- inject the mod ----
Remove-Item "$modDir\coop.stop", "$modDir\coop.coco", "$modDir\coop.experimental", "$modDir\coop.noevents" -ErrorAction SilentlyContinue
'A udp 127.0.0.1 27901 127.0.0.1 27902' | Set-Content "$modDir\coop-$(GamePid 'A').cfg"
'B udp 127.0.0.1 27902 127.0.0.1 27901' | Set-Content "$modDir\coop-$(GamePid 'B').cfg"
& "$PSScriptRoot\inject.ps1" -Inst A -Dll $Dll | Out-Null; Start-Sleep 2; & "$PSScriptRoot\inject.ps1" -Inst B -Dll $Dll | Out-Null

# 1 presence
$ok = WaitFor 'spawn' { (ModLog 'A' | Select-String 'spawn state 3 puppet 0x[1-9A-F]') -and (ModLog 'B' | Select-String 'spawn state 3 puppet 0x[1-9A-F]') -and (ModLog 'A' | Select-String 'friend visible') -and (ModLog 'B' | Select-String 'friend visible') } 40
Check '1 presence: puppets spawned, friend visible on both' $ok '' | Write-Output
$ok = WaitFor 'scan' { (ModLog 'A' | Select-String 'crate scan: [1-9]') -and (ModLog 'B' | Select-String 'crate scan: [1-9]') } 30
Check '1 presence: crate lists built on both (precondition)' $ok '' | Write-Output

# B picks up fruit first (its mod finds its own wumpa counter on the first change; needed for check 3)
# one pickup can leave two candidates (the crate counter can change with the fruit): B breaks crates until found
$ok = $false
foreach ($cb in $cbs) { & "$PSScriptRoot\jumpon.ps1" -Inst B -X $cb[0] -Y $cb[1] -Z $cb[2]
    $ok = WaitFor 'b counter' { ModLog 'B' | Select-String 'wumpa: counter found' } 15; if ($ok) { break } }
Check '3 fruit: B found its own counter (precondition)' $ok '' | Write-Output

# 2 crate (the first crate of the level)
$rx = CrateRx $ca
& "$PSScriptRoot\jumpon.ps1" -Inst A -X $ca[0] -Y $ca[1] -Z $ca[2]
$sent = WaitFor 'sent' { ModLog 'A' | Select-String "crate broken here at $rx" } 15
Check '2 crate: A detected its own break and sent it' $sent '' | Write-Output
$repl = WaitFor 'replayed' { ModLog 'B' | Select-String "friend broke the crate at ${rx}: breaking ours" } 15
Check '2 crate: B found its copy of the crate and broke it' $repl '' | Write-Output
$gone = WaitFor 'gone' { -not (ModLog 'B' | Select-String "friend broke a crate at $rx") } 1
Check '2 crate: B did not report it missing' $gone '' | Write-Output
$echo = -not (ModLog 'B' | Select-String "crate broken here at $rx")
Check '2 crate: B did not send the crate back as its own' $echo '' | Write-Output

# 3 fruit
$ok = WaitFor 'fruit' { (ModLog 'B' | Select-String 'wumpa: \+\d+ from the friend') } 30
Check '3 fruit: B added the fruit A picked up' $ok (@(ModLog 'B' | Select-String 'wumpa: \+') | Select-Object -Last 1) | Write-Output

# 4 fall (precondition: B shows A before the fall)
$pre = [bool](ModLog 'B' | Select-String 'friend visible')
Teleport 'A' $ca[0] $ca[1] ($ca[2] - 3000)
$park = WaitFor 'park' { ModLog 'B' | Select-String 'friend falling: puppet parked above the level' } 15
Check ('4 fall: B parked the puppet while A fell (precondition friend visible: ' + $pre + ')') ($pre -and $park) '' | Write-Output
$back = WaitFor 'back' { ModLog 'B' | Select-String 'back on their feet' } 40
Check '4 fall: puppet back once A respawned' $back '' | Write-Output
Check '4 fall: puppet never destroyed by the kill plane' (-not (ModLog 'B' | Select-String 'puppet gone')) '' | Write-Output

# 5 level change: A loads the level again with the console command (works the same in Crash 1, 2 and 3)
Start-Sleep 5   # let A finish respawning first
$before = LevelLoads 'A'; LoadLevel 'A'
$hub = WaitFor 'reload' { (LevelLoads 'A') -gt $before } 30
$respawn = WaitFor 'respawn' { @(ModLog 'A' | Select-String 'spawn state 3 puppet 0x[1-9A-F]').Count -ge 2 } 60
Check '5 level: A loaded the level again (precondition)' $hub '' | Write-Output
Check '5 level: A back in the level, puppet spawned again' $respawn '' | Write-Output
# invariants, sampled three times a few seconds apart
$inv = $true
for ($s = 0; $s -lt 3; $s++) {
    foreach ($i in 'A', 'B') {
        if (-not (Alive $i)) { $inv = $false; "  sample ${s}: $i is not running" }
        elseif ([ST]::Dialogs([uint32](GamePid $i))) { $inv = $false; "  sample ${s}: $i shows an error dialog" }
        if (ModLog $i | Select-String 'FAULT') { $inv = $false; "  sample ${s}: FAULT in $i's mod log" }
    }
    Start-Sleep 3
}
Check 'invariants: both games alive, no FAULT, no error dialog' $inv '' | Write-Output

} catch { "error: $_"; Check 'suite ran to the end' $false "$_" | Write-Output }
if (-not $KeepRunning) { New-Item "$modDir\coop.stop" -ItemType File -Force | Out-Null; Start-Sleep 3; Remove-Item "$modDir\coop.stop"; foreach ($i in 'A', 'B') { $env:GAMECTL_INST = $i; & $g stop | Out-Null } }
$expected = 15
$r = Score $results $expected
"{0}/{1} checks passed" -f @($results | Where-Object Ok).Count, $expected
"RESULT: $r"
