# Crash N. Sane Coop

Experimental two-player online co-op for **Crash Bandicoot N. Sane Trilogy** (PC, Steam). Work in progress.

Each player plays their own game. The mod shows the other player in your level as a second Crash (or Coco),
replays their moves and animations, and keeps both games in step for crates and wumpa fruit. It talks over Steam
peer-to-peer through the game's own Steam connection: no server, no port to open.

*Version française plus bas.*

## Status

| Feature | State |
|---|---|
| See your friend in your level (position, rotation) | works, played over Steam |
| Friend's animations (run, jump, spin) | works in Crash 1 and Crash 3; characters still slide a little |
| Crates: a crate one player breaks also breaks for the other (with its fruit, Aku Aku, crate count) | works in a two-copy lab test (not yet over Steam) |
| Shared wumpa counter: fruit one player picks up is added for the other | works in a two-copy lab test (not yet over Steam) |
| Both players on the same hero (Crash/Crash or Coco/Coco): the friend appears with that hero and all animations | works (Coco/Coco: Crash 1 lab test) |
| Friend on another hero than you (e.g. they play Coco, you play Crash) | shown with **your** hero, moved by their buttons only (loading the other hero crashed the game on level changes, v14/v15) |
| Crash 2 | the friend appears, but some animations do not map |
| Deaths and falls | the friend's puppet hides while they fall into a pit and comes back when they respawn; crates restored by a death are not re-sent |
| Checkpoints, level end | not synced (each player has their own) |
| More than 2 players | no |

QA level: **manual** (real two-player sessions over Steam for presence and animations; lab tests with two copies
of the game on one PC for crates, fruit and Coco). No automated test suite yet.

Known issues:
- Characters slide a bit while running.
- If you and your friend play different heroes, they appear as your hero without their animation events. Loading the other hero's pack (v14/v15) made the game crash on the next level change, so it is disabled.
- The game was seen crashing once while the friend's puppet was removed below the level; v14 parks it above the
  level instead (not yet confirmed in a real session).

## Requirements

- Crash Bandicoot N. Sane Trilogy on Steam, build 2964270 (exe SHA-256
  `170c61d37fb5277dd94b3e41f7a918f227b76bc67aa338f5c97f63ac317b6af3`). Other versions are not supported: the mod
  uses fixed addresses inside the exe.
- Windows, both players running the same mod version.

## Install and play

1. Build `crash-coop.dll` (below), or get it from a release, and put it in one folder with the files of `launcher/`.
   Use a normal folder (not OneDrive).
2. Double-click `Lancer Crash Coop.cmd`.
3. Paste your friend's SteamID64 (or their Steam profile link). The launcher shows your own SteamID: send it to them.
4. Click **1. Start the game**, go to the game's main menu or a level. The mod switches on by itself (or click
   **2. Enable the mod**). Your friend does the same.
5. Play the same level: you see each other.

Nothing is written to the game folder or the saves: the DLL only lives in the game's memory while it runs. It only
accepts packets from the SteamID you entered. Logs: `coop-me.log` in the mod folder.

## Build

Any C compiler for x86-64 Windows works. With [Zig](https://ziglang.org/):

```
zig cc -target x86_64-windows-gnu -shared -O2 src/crash-coop.c -o crash-coop.dll -lws2_32 -lwinmm
```

or run `build.ps1`, which builds into `dist/` and copies the launcher next to it.

## How it works (short)

- `coop.ps1` injects the DLL into the running game (LoadLibrary through CreateRemoteThread).
- The DLL spawns a copy of the game's own Crash (or Coco) prototype as a "puppet", writes the friend's position,
  rotation and buttons into it every frame, and replays the friend's Havok behavior events on it from the game
  thread (hardware breakpoints on the engine's event and behavior-sync functions).
- Crates: a broken crate's component disappears from memory; its position is sent, and the other game hits its own
  crate there with the game's damage function, so the game itself handles fruit and counters.
- Fruit: the real wumpa counter is a game variable; each side sends what it picked up, the other side adds it.
- Heroes: each side sends the hero it plays. The puppet is cloned from the local hero's prototype; events are replayed only when both heroes match.

All the reverse-engineering notes (addresses, what was tried, what failed) are in
[docs/RESEARCH-NOTES.md](docs/RESEARCH-NOTES.md).

License: MIT (see [LICENSE](LICENSE)).

This project started from the [coop-anything](https://github.com/cranebeaker/coop-anything) playbook. No game files
are included in this repository.

---

## Français

Co-op en ligne à deux, expérimental, pour **Crash Bandicoot N. Sane Trilogy** (PC, Steam). En cours de développement.

Chacun joue dans sa propre partie. Le mod affiche ton ami dans ton niveau (en Crash ou en Coco), rejoue ses
mouvements, et synchronise les caisses cassées et les fruits Wumpa. Tout passe par le Steam du jeu, sans serveur.

**Jouer :** mets `crash-coop.dll` et les fichiers de `launcher/` dans un même dossier (pas OneDrive), lance
`Lancer Crash Coop.cmd`, colle le SteamID64 de ton ami, clique « 1. Lancer le jeu ». Le mod s'active tout seul.
La case « Voir mon ami en Coco » est désactivée pour l'instant.

**Limites connues :** les persos glissent un peu ; si vous jouez le même héros, ton ami a toutes ses animations ; sinon il apparaît avec ton héros, sans ses animations (charger l'autre héros faisait planter aux changements de niveau) ; morts et checkpoints
ne sont pas gérés ; Crash 2 incomplet ; 2 joueurs maximum. Version du jeu supportée : Steam, build 2964270.
