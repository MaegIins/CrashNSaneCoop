# Automated two-client test

`suite.ps1` runs two copies of the game on one PC, connects them with the mod over UDP (127.0.0.1), drives player A
and checks what player B's game and both mod logs report. Nobody needs to play.

| # | Check |
|---|---|
| setup | both copies reach Crash 1, N. Sanity Beach (a copy that dies while starting is restarted) |
| 1 | both puppets spawned, each side sees the friend; crate lists built (precondition) |
| 2 | A jumps on a crate: A sends it, B breaks its own copy, B does not call it missing, B does not send it back |
| 3 | B picks up fruit first (precondition: its counter is found), then gets the fruit A picks up |
| 4 | A drops below the level: B parks the puppet, shows it again after the respawn, and the puppet is never destroyed |
| 5 | A quits to the map (precondition: the map really loaded) and comes back: the puppet is spawned again |
| invariants | sampled 3 times: both games alive, no FAULT line in the mod logs, no Windows error dialog |

A scorer self-test runs first. A check that never ran counts as a failure. The last line is `RESULT: PASS` or
`RESULT: FAIL`.

## Lab folder

Point `CRASHCOOP_LAB` at a folder (default: `lab\` next to this repository) that holds:

- `test-install\`: a **copy** of the game folder (Steam build 2964270). Never use the real install: the test
  copies get their own `APPDATA` folders (`appdata-A`, `appdata-B`) so the real saves are not touched. Loading a
  save with progress can unlock Steam achievements on the account that runs the copies.
- `mod\crash-coop.dll`: the build to test. The suite writes the UDP configs and the mod logs there.
- `logs\`, `pid-A.txt`, `pid-B.txt`: created by `gamectl.ps1`.

Then run `powershell -ExecutionPolicy Bypass -File tests\suite.ps1` (about 8 minutes). Windows A and B open side
by side on the left and right halves of the screen; leave them visible, because keys and clicks are sent to them.

The menu clicks assume a 1280x720 window and the French menu layout of a save that already reached N. Sanity Beach.
