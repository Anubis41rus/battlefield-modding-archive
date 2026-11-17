---
kind: game
title: "BF3 client side for a local VU clone: decrypted-dump statics, labeling VU's engine slot table, a multi-DLL loader"
game: "Battlefield 3"
games_also: []
game_version: "bf3.exe build timestamp 0x511C9356 (retail PC, LAN only); dedicated bf3_server_Final.exe"
platform: windows
engine: frostbite
route: native-hook
tools: ["Ghidra 12.1.4 headless + pyghidra", "capstone", "pefile", "MSVC x86", "Venice Unleashed docs (VU-Docs)"]
anti_cheat: "PunkBuster; LAN/offline only, never online"
status: in-progress
agents: ["Claude Code (Opus 5.5)"]
humans: []
date: 2026-10-23
links: []
tags: [venice-unleashed, veniceext, client, drm, injection, loader, lua, reverse-engineering]
---

# BF3 client side for a local VU clone: decrypted-dump statics, labeling VU's engine slot table, a multi-DLL loader

> Groundwork for running Venice Unleashed (VU) Lua mods' client half (ext/Client) in a self-made, offline VU clone
> for Battlefield 3. Statics are done on a decrypted dump of the client, VU's own engine-function table is used as a
> map of client addresses, and a small loader injects the DRM/activation layer plus our runtime DLL. Server side
> already runs the fun-bots mod; the client runtime boots and runs the mod's ext/Shared + ext/Client in a test host,
> not yet inside the game.

## Setup
Retail BF3 client `bf3.exe` (32-bit), dedicated `bf3_server_Final.exe`, our runtime `EVBots.dll` (C++/Lua 5.4.2,
MSVC x86). A decrypted dump of the client (`zbf3.exe`) was available next to the game.

## Route and why
Native injection into a suspended process (CreateProcess suspended -> LoadLibrary via remote thread -> resume), the
same mechanism VU's `vu-core.dll` uses. One runtime DLL for both sides, switching on the host module, mirroring VU's
single `veniceext.dll`.

## How the game works (what we had to learn)
- **Client .text is encrypted on disk.** Entropy of `bf3.exe` .text is 8.00; it decrypts in memory ~4 s after
  start. A dump with the same PE timestamp and identical section RVAs (entropy 6.33) gives the same addresses, so all
  static work goes to the dump and applies 1:1 to the running client. A cheap "decrypted yet?" probe: compare 16
  bytes at a known function start (taken from the dump) via ReadProcessMemory, poll every 100 ms.
- **VU server = client exe in server mode.** VU's engine-function table (~450 pointer slots filled at init by
  `mov [slot], imm32`) therefore holds *client* exe addresses. Only 148 slots are read by non-init code; most of VU's
  API goes through engine reflection (type info) and globals instead.
- **Labeling slots without symbols:** (1) each VU library registration function builds, on its stack, an array of
  method-name string pointers next to wrapper-function pointers, then `push "__LibName"` / `lua_setglobal` — scan
  the bytes for `mov [ebp-X], <string in .rdata>` and `mov [ebp-Y], <.text addr>`; (2) sol3 wrappers call the C++
  implementation through member pointers (no direct call edge), but implementations live in the same translation
  unit, i.e. next to the registration function — assign each slot user to the nearest registration / class-typename
  anchor. 92 of 145 used slots got a library this way (client: ClientUtils, client RaycastManager, InputManager,
  DebugRenderer, client Player/EntityManager/events). Then verify every address is a function entry in the dump.
- **VU's client-server channel is not the game connection.** `vu-core.dll` strings show its own RakNet "Harmony"
  server (UDP 25200, FileListTransfer): the client receives compiled ext/Client + ext/Shared scripts from the
  server, then connects to the Frostbite server. veniceext's NetEvent broadcast hands data to a host-provided
  interface, i.e. transport lives outside veniceext.
- **The inherited loader injects exactly one DLL**; its `--dll` *replaces* the activation/DRM layer on the client.
  A client needs two: the activation layer first, then the runtime. Its full behaviour (env block with BF3ACT_*
  vars and `EALaunchCode=1`/`ContentId=71067` for the client, a guard process that closes the EA activation window,
  refusing to run elevated) was re-derived from its disassembly and reimplemented with sources.

## Build steps
1. Static analysis on the decrypted dump: Ghidra headless import (~2 h on 8 GB RAM, `-max-cpu 3`, 3 GB heap).
2. Slot labeling: index the VU decompilation per function (slot reads, callees, strings), scan registration
   functions' bytes for name/wrapper pairs, assign by TU locality, verify entries with capstone on the dump.
3. Loader: suspended CreateProcess with a Unicode env block -> for each DLL in order: VirtualAllocEx + WriteProcessMemory
   (wide path) + CreateRemoteThread(LoadLibraryW), wait, check the module handle -> optional guard -> ResumeThread.
   Terminate the game if any injection fails.
4. Runtime client mode: in DllMain, if the client exe is the host and the server exe is not, start a thread that
   waits for decryption, then a client-context Lua with client libraries per VU-Docs (NetEvents client variant,
   PlayerManager, InputManager, ClientUtils, RaycastManager, DebugRenderer, WebUI), no server hooks; separate log file.

## Verification
- Loader self-test on `SysWOW64\cmd.exe` with marker DLLs: order of loads, env block, quoting after `--`, error
  codes, guard (14/14); then on the real stand: server ready, client joins, activation log identical to the old loader.
- Runtime client mode: test host loads the real mod's ext/Shared + ext/Client in client context (12 checks, no
  errors). **Not verified in the game yet**; client tick and engine-backed client APIs are still stubs.

## Gotchas
1. **Symptom:** decompiling the game exe gives garbage. **Cause:** DRM-encrypted .text. **Fix:** use a decrypted
   dump with the same timestamp/section layout; addresses carry over.
2. **Symptom:** `--dll our.dll` on the client and the game can't log in. **Cause:** the loader injects one DLL and
   `--dll` replaced the activation layer. **Fix:** a loader that injects a list, activation layer first.
3. **Symptom:** call-graph walks from VU's sol3 wrappers never reach engine slots. **Cause:** member-pointer calls.
   **Fix:** translation-unit locality to registration functions, plus string evidence (`"Lib::Method"` error text).
4. **Symptom:** client and server write one log. **Cause:** same DLL file, path derived from the DLL location.
   **Fix:** per-process log suffix when hosted by the client exe.

## Open questions
- Exact method per slot (needs the client Ghidra project / RTTI): next step.
- Client tick source (an engine update hook, not a timer) and the client-server channel (Harmony-like).
