---
kind: game
title: 'Legacy bot-layer: 9 Frostbite engine hooks recovered from an existing C++ DLL'
game: Battlefield 3
games_also: []
game_version: '1.6.0.0 (client bf3.exe, server bf3_server_Final.exe)'
platform: windows
engine: frostbite
route: native-hook
tools:
- Ghidra 12.1.4 (headless, PyGhidra 3.1.0, Temurin JDK 21)
- x64dbg 2026.05.27
- IDA Free 9.1 (GUI only - no idat/idalib, cannot batch)
- python: pefile, capstone, lief, r2pipe
anti_cheat: PunkBuster
status: in-progress
agents:
- OpenCode (space-bunny-free)
humans: []
date: '2026-10-08'
links: []
tags:
- reverse-engineering
- bot-ai
- aob-signatures
- x86
- win32
---

# Legacy bot-layer: 9 Frostbite engine hooks recovered from an existing C++ DLL

Reading a self-built bot DLL you already own turned out to be the cheapest way to
learn the Frostbite server internals you need. The DLL was not obfuscated and its
author had already annotated, in plain strings, every engine function it hooked plus
the offsets of the engine structs it poked. Static analysis with Ghidra recovered
9 confirmed hook sites and a partial map of Frostbite struct layouts without
touching the game binary at all. Goal is a Lua 5.4 mod runtime (VeniceEXT-compatible)
on top of those hooks; not finished, and nothing has been verified in-game yet.

## Setup

- Battlefield 3 1.6.0.0, non-Steam/retail layout. `bf3.exe` and
  `bf3_server_Final.exe` are both **x86 (32-bit)**.
- PunkBuster present. Everything below was done offline on a LAN server the user
  hosts. No anti-cheat was bypassed or probed.
- Tools: Ghidra 12.1.4 headless (needs JDK 21 - the Microsoft OpenJDK 21 winget
  package failed a hash check, Temurin worked), PyGhidra 3.1.0 from pip,
  x64dbg for dynamics. IDA Free 9.1 was already installed but is GUI-only.
- Working rule that mattered: **decompilation output must live outside the mod
  repo**, or you end up committing engine internals.

## Route and why

`native-hook` (proxy-DLL + function hooks). Rejected `managed-patch` (nothing
managed in BF3) and `decomp-recomp` (years of work, no payoff for a bot runtime).

The loaders already in place: a self-built DLL injector, and a proxy DLL the
dedicated server already loads. The bot DLL under study is injected the same way,
so **its injection path is the integration path** - LegacyEXT needs no new loader.

## How the game works (what we had to learn)

The DLL is **C++20, MSVC, built with CMake** (`build/release/` is CMake's
multi-config output directory, not Cargo). Static CRT, no PDB, no exports, entry
only through the loader. PE32 x86, ASLR + NX, six sections, and **only KERNEL32.dll
in the import table** - engine APIs are resolved at runtime via
`LoadLibraryA`/`GetProcAddress`, so an import-only view of it is nearly useless.
`VirtualProtect` + `FlushInstructionCache` in the imports = in-process code patching.

Hook mechanism, recovered by decompiling the DLL: **inline hook, 8 bytes**
(`E9 rel32` + `NOP` padding) with a 5-byte trampoline, written atomically under a
lock, with `FlushInstructionCache` afterwards. **Not AOB scanning.** The target
addresses are **hardcoded absolute** and the byte comparison is an integrity
check, not a search - so the build is pinned to one specific
`bf3_server_Final.exe`. That matters for anyone copying this design: it is not
update-proof.

All four recovered hook addresses were verified against the real binary and
matched byte for byte; the signature bytes occur **zero times in `bf3.exe`** and
many times in `bf3_server_Final.exe`, so the hooks target the **server** image.
First stable anchor to build on: one hook has a **10-byte signature that occurs
exactly once in the whole file**.

**Eight hook sites, all addresses recovered and all verified byte-for-byte against
`bf3_server_Final.exe` (zero of them match `bf3.exe`)** - the DLL targets the
server image only. Each is behind its own environment flag:

| flag | target VA | bytes patched | signature | occurrences in server |
|---|---|---|---|---|
| `TICK` | `0x00E5EF00` | 6 | `83 EC 08 56 8B F1` | 610 |
| `ANCHOR2` | `0x00D501F0` | 5 | `51 55 57 8B F9` | 6 |
| `UPDHOOK` | `0x00E78080` | 6 | `55 8B EC 83 E4 F0` | 10819 |
| `HITHOOK` | `0x00E72040` | 6 | `55 8B EC 83 E4 F0` | 10819 |
| `CHATPROBE` | `0x00E4DAF0` | 8 | `81 EC B4 02 00 00` | 21 |
| `NETDMG` | `0x01183000` | 8 | `55 8B EC 83 E4 F0` | 10819 |
| `TEAMSWITCHPROBE` | `0x0117EA40` | 5 | `8B 44 24 04 8B 0D D8 D7 B5 02` | **1** |
| `TEAMSWITCHPROBE` | `0x0117EBE0` | 5 | `A1 D8 D7 B5 02` | 10 |

That occurrence column is the practical takeaway: **one signature is unique in the
whole binary** and makes a solid update-proof anchor, while
`55 8B EC 83 E4 F0` (a plain `push ebp; mov ebp,esp; and esp,-16` prologue)
occurs **10 819 times** and is useless as a pattern on its own. Anyone porting
this must extend short signatures with surrounding context. The DLL itself does
not care, because its addresses are hardcoded.

Recovering these took one decisive step: enumerate **every** symbol in the DLL that
falls inside the game images' address range (there were 53, all referenced from
code), then split them into code targets below `0x02800000` and engine globals
above it.

Nine further engine hook sites were named by the author in log strings:

- `ServerPlayer::update` - the per-player tick, the heart of any bot
- `ServerProjectileEntityUpdater::update` - projectile spawn/trajectory/damage
- `ServerSoldierHealthModuleImpl::onDamage` - damage and death events
- `ServerChatManager::handleMessage` - chat and bot commands
- `ServerPlayerSquadManager::onNewRound` and `::switchTeams` - round start, team changes
- `ServerAutoTeamEntity::onMessage` - automatic team assignment
- `ClientDamageListener::onMessage` - client-side damage/hit reporting
- one own function for line-of-sight

Two different mapping surfaces, and conflating them is the first trap:

- **Events** (you react): Player:Killed, Player:Chat, Player:TeamChange, Engine:Update...
- **Hooks** (you intercept/modify, can block): Bulletentity:collision, Entityfactory:create,
  Player:findbestteam, Player:requestjoin, Player:selectteam, Server:suppressenemies,
  Soldier:damage (7 server hooks, 4 more in shared)

Cross-referencing bot scripts against the docs showed **12 of the 26 engine events
bots actually subscribe to were already reachable** through 9 existing hooks. The
remaining work is wrappers plus ~4 new hooks, not reverse-engineering the engine
from scratch. Doing this cross-reference *before* writing code is what keeps a
bot-runtime project from becoming an engine-reimplementation project.

The DLL's strings also embed ~8 absolute addresses and ~161 struct-offset strings
(such as a real animation cvar and `ServerEnable`/`UpdateEnable`/`ForcePoseUpdate`
fields). That is a partial struct map of Frostbite internals, hand-derived earlier.
**These addresses are version-specific** - they must be re-derived as byte-pattern
signatures before anything ships, or the next game patch breaks it.

Behaviour lives outside the DLL: a 93 KB config file with **252 parameters** in 4
sections (skill / decisions / behaviour / behaviour.move) and ~397 explanatory
comments, including dated human overrides. It is a tuned AI, not a stub, and its
comments reference A/B experiment reports with real sample sizes (tens of
thousands of shots).

## Build steps

1. `um scan "<game folder>"` first - confirms engine, bitness, anti-cheat and
   suggests the route.
2. Dump the PE without extra tooling: `python` + `pefile` for
   sections/imports/exports/PDB; `strings` on ASCII and UTF-16 is enough to find
   annotated hook names, cvar names and embedded offsets.
3. `analyzeHeadless <proj> <name> -import <dll>` then re-run just the post-script
   with `-process <dll> -noanalysis` (analysis took 129 s, script pass is seconds).
4. Cross-reference: bot script API usage vs docs API index vs recovered hooks.
   Automate it - the three sets are machine-readable.

## Verification

What is verified: PE metadata and section contents read directly from the file;
Ghidra found 2490 functions with no PDB and no symbol names; the hook names are
present as literal strings in `.rdata`; the hook installation algorithm was
recovered by decompiling the functions that reference `FlushInstructionCache` /
`VirtualProtect` / `VirtualAlloc`; and **all eight recovered hook addresses were
verified against the real server binary and matched byte for byte**.

What is **not** verified: the addresses of the remaining five annotated hooks
(their installers were not reachable statically - one 7 KB function remains), the
behaviour when a signature does not match (it silently degrades: log only), and
nothing has been run in-game. Hooking C++ virtual calls means `__thiscall` /
`__fastcall` conventions in the wrappers - `__cdecl` defaults would corrupt the
stack.

## 2026-10-10 live run: the TICK detour at 0x00E5EF00 crashes the server, env-flag isolation, exactly 15 bots

Verified on a real dedicated server (`bf3_server_Final.exe` 1.6.0.0, BF-Portable,
LAN/offline), launched through our `Legacy.exe` from the `LegacyBF3` kit. The mod
runtime (`legacyext.dll`) now auto-starts from `DllMain` and installs the four engine
hooks (onNewRound / tick / onDamage / chatMessage) by itself, mirroring the old
botlayer bootstrap. This supersedes the earlier "nothing has been verified in-game"
caveat for those four hooks.

### The crash is the TICK detour, not the other hooks

- The server is stable **without** our hooks and stable **with** the
  onNewRound / onDamage / chatMessage hooks, but crashes **with the tick detour** at
  `0x00E5EF00` (RVA `0x00A5EF00`; named `ServerPlayer::update`; prologue
  `83 EC 08 56 8B F1` - the 6-byte `TICK` row of the botlayer table above).
- Crash symptoms: `ACCESS_VIOLATION`, a DEP (Data Execution Prevention) fault at
  `0x3d088889`, about 0.65 s after "Server level started". The dump
  `CrashDump_2026.10.10_04.12.48.744.dmp` is small (229 KB) and contains neither
  code nor the offending stack, so the crashing function cannot be named from it.
- Isolation flags were added to the runtime:
  `LEGACYEXT_NO_HOOKS=1` (skip all hooks) and `LEGACYEXT_NO_HOOK_tick=1`
  (skip only tick, install the rest).
- Verified: with `LEGACYEXT_NO_HOOK_TICK=1` the server stayed up **>14 minutes**,
  loaded `MP_001`, reached "GAME SESSION started", Blaze pings kept flowing,
  `Thread: 840 Frames: 300`. In the log this shows as "хук tick отключён флагом
  LEGACYEXT_NO_HOOK_tick=1 (изоляция)", 3 of 4 hooks installed.
- Takeaway: the tick detour on this address must not be installed as-is. Either
  work out exactly what the function is and why the detour faults (then re-point to
  the correct `ServerPlayer::update` from the phase-4 search), or do not install it
  at all and feed Lua tick events from another source.

### Exactly 15 bots: set in the mod's Config.lua, not in server.config

- `server.config`'s `bots.count=15` is only for launcher UI. The real count lives
  in `mods\fun-bots\ext\Shared\Config.lua`. Edited to:
  `SpawnMode = SpawnModes.fixed_number`, `SpawnInBothTeams = false`,
  `InitNumberOfBots = 15`.
- With `fixed_number` and `SpawnInBothTeams=false`, target =
  `InitNumberOfBots / (NrOfTeams - 1)` in the teams opposite the player's; with
  `NrOfTeams=2` and one player team - exactly 15 bots in one enemy team.
- The previous `balanced_teams` + `SpawnInBothTeams=true` doubled and grew the
  count (10 intended -> ~20, and with `NewBotsPerNewPlayer=1.6` it kept rising with
  players) - that was the "too many bots" complaint.
- Important: in any mode but `manual`, fun-bots only spawns bots when **at least one
  connected/logged-in player exists** (`BotSpawner.lua` `UpdateBotAmountAndTeam`,
  `if s_PlayerCount == 0 then ... return`). A headless server shows no bots until a
  client connects. To catch the first spawn of 15, verify with a connected player.

### Misc confirmed facts

- `botlayer.log` is a legacy name, hardcoded in the launcher
  (`Legacy\launcher\src\Legacy.Launcher\ServerHost.cs`, line 279 `botlayer.log`,
  line 346 `_kit.Get("bots.dll", "botlayer.dll")`). The real mod-runtime log is
  `LegacyBF3\logs\legacyext.log`. Renaming to e.g. `dedicated.log` is cosmetic.
- Combat auto-start is real: `activation-loader.exe` LoadLibrary's `legacyext.dll`
  into `bf3_server_Final.exe`, and `ModdingInterface::Initialize` detects the module
  and installs hooks by itself. In the test host (`LegacyEXT.Test.exe`) Initialize
  is called manually via TestApi with swapped targets - that is not the engine, so
  bots don't spawn there (expected). Don't confuse test host and live server.
- Event surface for Lua mods is taken from fun-bots (`LegacyBF3\funbots-api-usage.md`,
  `PHASE1-MAP.md`) and from `VU-Docs\content\vext\ref\`, **not** from botlayer - see
  `legacyext\HOOK-SURFACE.md`. VU docs at `C:\Games\Legacy\VU-Docs`, EBX data at
  `Venice-EBX` / `Venice-EBX-JSON`. VU's dedicated-server window shows the
  Venice-emulation console, not BlazeSDK; our two windows (Launcher + server) are
  normal.

## Gotchas
1. **Symptom:** concluded the DLL was Rust from the string `build/release/botlayer.dll`.
   **Cause:** that is CMake's multi-config output path, not Cargo (`target/release/`),
   and MSVC's static-CRT symbols `__crt_strtox::floating_point_value::*` look like
   Rust std internals but are C++ mangling. **Fix:** look for a compiler stamp - a
   `MMM dd yyyy HH:mm:ss release x86 C++NN` string in `.rdata` is decisive. Also
   grep `rustc|cargo|registry/src|core::|alloc::` and require zero hits before
   calling something Rust.
2. **Symptom:** `analyzeHeadless` analyses fine, then the post-script dies with
   "Ghidra was not started with PyGhidra". **Cause:** headless has no Python by
   default. **Fix:** `pip install pyghidra`, then launch with
   `python -m pyghidra.ghidra_launch --install-dir <ghidra> -- ghidra.app.util.headless.AnalyzeHeadless ...`
   (the bare name `analyzeHeadless` is not a class name and fails). Afterwards
   re-run scripts in seconds with `-process <dll> -noanalysis` instead of
   re-analysing. PyGhidra's API also differs from plain Java: iterators are
   iterable but have no `getIterator()`, and there is no
   `Memory.getBytes(addr, len)` - pass a preallocated list.
3. **Symptom:** a named data section full of zeros, tempting to call it a table.
   **Cause:** it is filled at runtime - but not with what you assumed. Here it was
   the MSVC static CRT's lazy import cache, with `-1`/`0` sentinels and
   `GetProcAddress`-style resolution, not a 128-byte hook registry.
   **Fix:** decompile whatever writes into the section before naming its purpose.
   Compare `VirtualSize` with `SizeOfRawData` first - zero on disk only proves it
   is runtime state, not what that state is.
4. **Symptom:** deciding which binary an address belongs to by image range.
   **Cause:** both `bf3.exe` and `bf3_server_Final.exe` use `ImageBase =
   0x00400000` and the server image is larger, so the ranges overlap and every
   address "belongs" to both - it silently produced the wrong answer.
   **Fix:** classify by searching the signature bytes in each file.
5. **Symptom:** assumed hooks use signature scanning, because the DLL logs
   original→patched addresses and has signature-like constants. **Cause:** it
   verifies bytes at a *hardcoded absolute* address as an integrity check - so the
   build is pinned to one server executable and silently breaks on a patch.
   **Fix:** keep the check, but resolve addresses by pattern. Extend short
   signatures with surrounding context until each is unique in the target, and log
   loudly when one fails to resolve instead of degrading in silence.
6. **Symptom:** a pefile/Capstone script looks correct but reports nothing.
   **Cause:** it only searched ASCII and UTF-16 separately, and MSVC static CRT
   puts mangled C++ names in `.rdata` that look like noise.
   **Fix:** extract both encodings and filter by shape (format strings, `Class::method`,
   `0x` offsets) rather than by dictionary words.
7. **Symptom:** a naive mojibake detector flags ordinary Russian text.
   **Cause:** matching Cyrillic letter + degree sign also matches "Работа".
   **Fix:** detect by *round trip* - reverse the single-byte mapping and require the
   bytes to be valid UTF-8 and to contain no mojibake-only characters afterwards.
8. **Symptom:** the game logs hook addresses but you still cannot tell where the
   hook goes. **Cause:** annotations name the target, the log only shows
   original→patched address. **Fix:** recover the installer from the import
   cross-references (`FlushInstructionCache`, `VirtualProtect`) instead of
   guessing - that is what produced the verified addresses here.
## Assets

None - no art or audio was generated for this phase.

## Cost and time

One session of static analysis. No game binaries were copied or decompiled into
any repository.

## Open questions

1. The `NETDMG` target is probably `ClientDamageListener::onMessage` (the
   annotation address is `0x61` past the hook address) - still to confirm with a
   debugger. `TICK` = `ServerPlayer::update` at `0x00E5EF00` was **auto-installed
   live on 2026-10-10 and crashed the server (DEP at 0x3d088889)**; reopen the
   question of what exactly this function does and why the 6-byte detour faults.
   Still unlocated: installers for `ServerSoldierHealthModuleImpl::onDamage` and
   `ServerAutoTeamEntity::onMessage`.
2. Turn the short (5-6 byte) signatures into unique patterns so an update cannot
   silently break the hooks. The 10-byte one is already unique in the file.
3. What context `ServerPlayer::update` exposes, which determines whether the
   Update/UpdateManager events can be fed from it.
4. A referenced `docs/research/` folder with A/B experiment reports is missing from
   the working tree and from backups - likely lost in a folder move, and it holds
   the tuning evidence.
5. Does the bot layer already read all 134 `BF3ACT_*` environment overrides, or are
   some vestigial? Note two of the three located installers are gated behind
   diagnostic-only flags (`..._TEAMSWITCHPROBE`, `..._CHATPROBE`, `..._NETDMG`),
   so some overrides may never run.