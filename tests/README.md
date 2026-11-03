# Automated two-client test

`suite.ps1` runs two copies of the game on one PC, connects them with the mod over UDP (127.0.0.1), drives player A
and checks what player B's game and both mod logs report. Nobody needs to play.

| # | Check |
|---|---|
| setup | both copies enter the chosen level with the game's own `loadmap` console command (a copy that dies while starting is restarted) |
| 1 | both puppets spawned, each side sees the friend; crate lists built (precondition) |
| 2 | A jumps on a crate: A sends it, B breaks its own copy, B does not call it missing, B does not send it back |
| 3 | B breaks crates until its fruit counter is found (precondition), then gets the fruit A picks up |
| 4 | A drops below the level: B parks the puppet, shows it again after the respawn, and the puppet is never destroyed |
| 5 | A loads the level again (precondition: the game logged a new load): the puppet is spawned again |
| 6 | (with `-Checkpoint x,y,z`) A opens a checkpoint crate, B's opens too, B dies and respawns at the checkpoint |
| invariants | sampled 3 times: both games alive, no FAULT line in the mod logs, no Windows error dialog |

A scorer self-test runs first. A check that never ran counts as a failure. The last line is `RESULT: PASS` or
`RESULT: FAIL`.

## Lab folder

Point `CRASHCOOP_LAB` at a folder (default: `lab\` next to this repository) that holds:

- `test-install\`: a **copy** of the game folder (Steam build 2964270). Never use the real install: the test
  copies get their own `APPDATA` folders (`appdata-A`, `appdata-B`) so the real saves are not touched. Loading a
  save with progress can unlock Steam achievements on the account that runs the copies.
- `mod\crash-coop.dll`: the build to test, and `mod\loadmap.dll` (built by `build.ps1` into `dist\tests`). The suite writes the UDP configs and the mod logs there.
- `logs\`, `pid-A.txt`, `pid-B.txt`: created by `gamectl.ps1`.

Then run `powershell -ExecutionPolicy Bypass -File tests\suite.ps1` (about 8 minutes). Other levels:

```
# Crash 2, Turtle Woods
tests\suite.ps1 -Level crash2/l201_turtlewoods/l201_turtlewoods -CrateA '16.1,1114.0,-208.2' -CrateB '464.0,3455.0,460.0;464.0,3455.0,146.0' -Checkpoint none
# Crash 3, Toad Village
tests\suite.ps1 -Level crash3/l301_toadvillage/l301_toadvillage -CrateA '-92.9,16.0,84.9' -CrateB '-171.9,-221.0,84.0;-65.8,1382.3,79.6' -Checkpoint none
```

Last results (v20): Crash 1 N. Sanity Beach 19/19 twice (with the checkpoint check), Crash 2 Turtle Woods 15/15, Crash 3 Toad Village 15/15 (`-Checkpoint none`).

Windows A and B open side
by side on the left and right halves of the screen; leave them visible, because keys and clicks are sent to them.

The only menu click (title screen to main menu) assumes a 1280x720 window.
