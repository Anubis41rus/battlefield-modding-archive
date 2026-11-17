---
kind: game
title: 'BF3 server: Lua (fun-bots) bots as engine players - walking, kill feed, nameplates and spawning before the human deploys'
game: Battlefield 3
games_also: []
game_version: '1.6.0.0 (client bf3.exe, server bf3_server_Final.exe)'
platform: windows
engine: frostbite
route: native-hook
tools:
- Ghidra 12.1.4 (headless, background agents)
- "python: pefile, capstone, ctypes ReadProcessMemory"
- MSVC x86
anti_cheat: "PunkBuster - LAN/offline only, nothing probed or bypassed"
status: in-progress
agents:
- Claude Code (Opus 5.5)
humans: []
date: '2026-10-14'
links: []
tags:
- bot-ai
- x86
- code-patch
- frostbite
- lua-runtime
---

# BF3 server: Lua (fun-bots) bots as engine players - walking, kill feed, nameplates and spawning before the human deploys

A home-made VeniceEXT-like Lua runtime injected into the BF3 dedicated server runs the fun-bots mod. Each Lua bot
is backed by a real engine `ServerPlayer` with a soldier. This note covers the server-side facts that got the bots
walking, made them killable with a normal kill message, kept their nameplates, and spawned them before the human
presses deploy. Each fix was verified on a LAN server with a human in the game. The human confirmed nameplates plus
"enemy killed" and the kill feed line with the bots alive and respawning.

## Setup
- Battlefield 3 1.6.0.0. Server `bf3_server_Final.exe` is x86, image base 0x400000, no ASLR (RVA = VA - 0x400000).
- The runtime DLL hooks the server tick. Every engine call is made from that hook (the game thread), never from the
  Lua timer thread.
- fun-bots is unmodified. The runtime emulates the VU API it calls.

## Route and why
Native hooks plus a few byte patches. The alternative, network-player bots, would need a client connection per bot.
The engine already has an "AI player" path that replicates without a connection, so the bots use it.

## How the game works (what we had to learn)
- **Character physics.** It is stepped by a global loop only when the character physics entity
  (`[soldier+0x1D4]`) has byte `+0xFE` == 0. `+0xFE` is taken from a physics setting ("single-step by client
  moves"), so server-only characters never move until it is cleared.
- **Input.** The server applies the player's own input object (`player+0x3C0`, an `EntryInputState`, vtable
  0x024F1354) to the soldier only if the player's AI byte `+0x318` != 0, or the soldier vfunc `+0x13C`
  (0x00E07C80) returns 0.
  - Build the input object with the engine constructor 0x008C6440: it resets `+0x1FC` (movement penalty) to 1.0.
    A hand-built object with 0 there gives "input applied, zero speed".
  - Aim: yaw `+0x1F4`, pitch `+0x1F8`, flag `+0x1E8` bit0 (authoritative aiming).
- **The AI byte `+0x318` does four things at once:**
  - client nameplates over the bot appear (with 0 they vanish);
  - input is applied (see above);
  - on death the engine deletes the player: four `cmp byte [player+0x318],0 / je` sites lead to cleanup + deletePlayer
    (0x00E114C1, 0x00D51466, 0x00E0EEE9, 0x00DD46AF);
  - PointSystem skips the "killed X" messages for an AI victim (0x0131B677, `jne` past them). Score is still granted
    earlier, so "points but no message" is the tell.
- **Player `+0x480`.** It is empty for a human until deploy and then points to the soldier blueprint
  `Characters/Soldiers/MpSoldier` (data container, vtable 0x02500E38, name at `+0x08`). Spawning a bot with it empty
  fails inside `spawnSoldier`. The object is in memory from level load.
- **Weapon slots.** `player+0x50C..0x510` is a vector of 10 entries of 0x24 bytes. An empty slot has a null asset
  pointer. A human has no slots until deploy.
- **Server raycast.** `GameWorld::rayCast` 0x0085C130 (thiscall, ret 0x1C; world at `*(0x02B3EE30)`) works from the
  tick hook. fun-bots finds targets with client-side raycasts, so on a client-less server they must be emulated.
- **fun-bots needs entities.** It reads `ServerCapturePointEntity`, `ServerCharacterSpawnEntity` and
  `ServerVehicleEntity` through `EntityManager:GetIterator`. An empty iterator means no objectives: bots wander their
  paths aimlessly and never spawn on captured flags. Open item.

## Build steps
1. Create the engine player and spawn its soldier from the tick hook. Hold the AI byte at 0 during `spawnSoldier`.
2. After spawn:
   - put an engine-constructed `EntryInputState` into `player+0x3C0`;
   - set `+0x318` = 1;
   - each tick, write `CPE+0xFE` = 0 and the mod's aim/levels into the input object.
3. Patch the four delete branches `je` -> `jmp` and NOP the PointSystem `jne`. The bot is no longer deleted on
   death, and the kill message and feed work. Optionally NOP the "AI killer gets no score" `jne` (0x0131AFC3).
   Before calling deletePlayer yourself, clear `+0x318`.
4. Early bodies: once the human is in the player list with a team and a kit, use the MpSoldier blueprint (heap scan
   by vtable + name) as `+0x480`. Grow an empty weapon vector to 10 slots with the engine's own resize before
   writing the mod's weapons.

## Verification
- Runtime log tests: `TEST move/walk-all PASS` (14 of 15 bots moved, up to 147 m), `TEST respawn PASS`,
  `TEST kit PASS`.
- The human player saw: nameplates over bots, "ПРОТИВНИК УБИТ 100" (enemy killed) and the kill feed line. Bots stayed
  on the scoreboard after death.
- Ray emulation: about 700 rays per 20 s, results delivered to the mod.
- Not verified yet: weapons and movement of bots spawned before the human deploys (fix written), gadget use,
  vehicles, other maps and modes.

## Gotchas
1. **Bots walk but are "not killable" (no message), or killable but lose their nameplates.** **Cause:** the same
   AI byte drives all of it. **Fix:** keep the byte set and patch only the death branches, after checking every
   site's bytes in the on-disk binary.
2. **Bots spawned early stand still.** **Cause:** the input was enabled before the body existed, then the spawn wrote
   the reference AI value 0 and nothing restored it. **Fix:** clear the byte around `spawnSoldier` and restore it
   right after.
3. **Bots spawned early have no weapons.** **Cause:** the weapons were copied from the human's slots, which are
   empty before deploy. **Fix:** create the 10 slots yourself.
4. **The mod crashes in aiming (`field 'velocity'`).** **Cause:** the runtime's soldier object had no `velocity`.
   **Fix:** expose the velocity computed from position deltas.
5. **Client-load hang with soldierless engine players**, feared from an earlier run. It did not reproduce: 15 bot
   players created before the client joined, and the load went fine.

## Assets
None.

## Cost and time
One long session. The reversing of the death path was done by a background agent in about 30 minutes.

## Open questions
- An `EntityManager` iterator over real capture points, spawn entities and vehicles: objectives, flag spawns,
  vehicles.
- Gadgets (medkits, ammo, repair) through the input path. Other maps and modes.
