# Build crash-coop.dll into dist\ and copy the launcher files next to it.
# Needs zig on PATH (https://ziglang.org/), or set $env:ZIG to its full path.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$zig = if ($env:ZIG) { $env:ZIG } else { 'zig' }
$dist = Join-Path $here 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
& $zig cc -target x86_64-windows-gnu -shared -O2 (Join-Path $here 'src\crash-coop.c') -o (Join-Path $dist 'crash-coop.dll') -lws2_32 -lwinmm
if ($LASTEXITCODE -ne 0) { throw 'build failed' }
Copy-Item (Join-Path $here 'launcher\*') $dist -Force
# test helper used by tests\suite.ps1 (enters a level with the game's own "loadmap" console command)
New-Item -ItemType Directory -Force (Join-Path $dist 'tests') | Out-Null
& $zig cc -target x86_64-windows-gnu -shared -O2 (Join-Path $here 'tests\loadmap.c') -o (Join-Path $dist 'tests\loadmap.dll')
if ($LASTEXITCODE -ne 0) { throw 'loadmap build failed' }
"Built: $dist"
