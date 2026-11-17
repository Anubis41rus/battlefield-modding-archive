---
name: reverse-engineering
description: Read how a game actually works so a mod can hook it. Decompile .NET/Mono (ILSpy), IL2CPP (Cpp2IL, Il2CppDumper), Java (Vineflower) and native code (Ghidra, IDA, via MCP servers); dump data and asset formats; reverse-engineer a binary file format and prove it with a round trip; read live memory (Cheat Engine, Frida, x64dbg); find render passes (RenderDoc). Use when a mod needs game internals, e.g. "how does the boss AI work", "find the damage function", "what format is this .sld/.pak/.dat", "where is the player position in memory".
---

# Reverse engineering for mods

Rule zero: **read the real thing, don't guess.** Decompiled code, the actual data, a live memory view or a
GPU capture is the spec. Write what you learn into `MODLOG.md` (names, IDs, offsets, formats) as you go.

Keep all decompiled output and extracted assets **outside** the mod repo (e.g. `~/<game>-decomp/`) and never
publish them. Single-player, offline or servers the user runs. Never attach debuggers or scanners to games
with anti-cheat (see `skills/mod-any-game/references/safety.md`).

## Pick the tool by what the code is (`um scan` tells you)

**Managed .NET** (XNA/FNA, Unity Mono, most indie C#)
- `dotnet tool install -g ilspycmd`, then `ilspycmd -p -o ~/<game>-decomp <Game>.exe` (or
  `Assembly-CSharp.dll`). This gives a full C# project you can grep.
- dnSpyEx: step through with a debugger, set breakpoints, edit methods.
- MCP: ILSpy-MCP / dnspy-mcp let an agent query types and methods directly.

**Unity IL2CPP**
- Cpp2IL (or Il2CppDumper) on `GameAssembly.dll` + `global-metadata.dat` gives types, fields, method
  signatures and addresses, plus dummy DLLs for ILSpy.
- Method bodies are native: load `GameAssembly.dll` in Ghidra/IDA and apply the generated script to name
  functions.
- Live: UnityExplorer, or `il2cpp-frida-mcp`.

**Java** (Minecraft, Slay the Spire...)
Vineflower / CFR / Recaf. For Minecraft, use Loom `genSources` with Mojang mappings.

**Native C/C++** (custom engines, Unreal game code, console recomps)
- Ghidra (free) or IDA, driven through MCP so the agent can decompile, rename, retype and follow
  cross-references:
  - Ghidra: GhidraMCP (LaurieWired), pyghidra-mcp (headless, with a run-script tool; code-capable tools beat
    hundreds of tiny ones), ReVa.
  - IDA: the official Hex-Rays IDA MCP (IDA 9.4+ Pro/Home; "code mode" runs IDAPython), or ida-pro-mcp.
  - Binary Ninja and radare2 have MCP servers too.
- Workflow:
  1. Strings, then cross-references, then the function.
  2. Name and type everything you understand. Renames accumulate into a readable program.
  3. Confirm dynamically (below) before building on a guess. Agents can confidently misidentify things.
- Unreal: dump the reflection data first (UE4SS dumper or Dumper-7); it names most gameplay classes and
  properties for free.

## Fast path first: route by file type, budget the calls, label every claim

Adapted from leanrea (aicodeking/leanrea, MIT), a CLI-only alternative to MCP reverse-engineering toolkits.
- **Answer most questions in 2–4 tool calls.** Start with the cheapest tool that fits the target, then open
  Ghidra/IDA only when the cheap pass can't answer. Batch probes into one shell call and cap every output
  (`| head -N`, Grep `head_limit`). A 30 MB decompile is grepped by address or string, never read whole.
- **Route other target types to the right tool:** Electron `app.asar` → `asar extract` and grep the JS;
  Go binary → `go version -m <exe>` for the module list, plus symbols kept in `pclntab` (GoReSym / redress);
  SQLite saves → `sqlite3 <db> .schema`; WASM → `wasm2wat`; firmware or packed blobs → `binwalk -e`.
  When a tool in the row is missing, use the fallback in the same row (strings + grep, `pefile`, capstone).
  Don't install anything, and don't run `npx`/package downloads, without the user's OK. On offline projects
  the answer is no.
- **Search extracted trees with hidden and ignored files included** (`rg -uu`): bundlers and installers put
  code in dot-dirs and paths that `.gitignore`-aware search skips.
- **Work on a copy** in a per-target folder outside the repo (`<re-root>/<target>/`), never on the live
  install the game runs from.
- **Label every claim in a report as seen, inferred or unknown,** with its source (`file:line`, VA, log
  line). "Seen" means you read it in the disassembly, data or log this session. "Inferred" means it follows
  from what you saw but you didn't check it directly; it goes to `STATE.md` as a hypothesis (below). An
  independent read-only checker (a cheap subagent) is worth running on the report before a build.

## Native inline hooks (x86): steal to the instruction boundary, not "5 bytes"

An inline hook replaces the first N bytes of the target with a jump (often `E9 rel32` +
`NOP` padding) and recreates the stolen bytes in a trampoline the detour calls for the
original. N must cover **whole instructions**; if the patch cuts one in half, execution
lands mid-instruction and faults with `0xC0000005` / `ACCESS_VIOLATION`. A 5-byte steal
looks safe but is only correct if bytes 1-5 decode cleanly — a prologue like
`0F AF CA` (3-byte `imul`) plus the start of the next instruction is the classic trap
(the trampoline jumps "onto the last byte of the mov" and dies). Known-good libraries
already encode the right lengths (e.g. the `X86Length.h` length tables used by
MinHook/vTrap line) — reuse the table instead of hard-counting bytes as instructions.

Hook-writing checklist (record all of it in MODLOG.md):
- [ ] stole the minimum whole instructions that cover the detour plus a comfortable margin;
- [ ] decoded the stolen byte range to the instruction boundary (disassembler, not byte counting);
- [ ] calling convention is right (`__thiscall`/`__fastcall`/`__cdecl`) — check it, don't assume:
      ECX read before written ⇒ `__thiscall`, EDX ⇒ `__fastcall`, and the declared stack-arg count
      must match the callee's `RET imm16` (`imm/4` args). A mismatch detour faults or corrupts the stack;
- [ ] trampoline reproduces the relocated bytes exactly (relative operands stay relative);
- [ ] `FlushInstructionCache` / `VirtualProtect` used correctly (RX→RWX then back);
- [ ] an env-flag kill-switch exists so a crashing hook can be turned off without rebuilding.

**Dynamic / live**
- Cheat Engine: value scans → "find out what writes to this address" → struct → owner. CheatEngine MCP
  servers exist.
- x64dbg: breakpoints and tracing (x64dbg-mcp; bind it to 127.0.0.1, since some default to 0.0.0.0).
- Frida (frida-mcp, frida-game-hacking-mcp): hook functions from JavaScript, log arguments.
- ReClass.NET rebuilds structs from live memory.

**Graphics**
- RenderDoc (renderdoc-mcp) captures a frame and shows every draw, the render targets, the constant buffers
  (view/projection matrices) and the depth buffer.
- That's how you find where to inject geometry or effects (mashup-mods), and which texture holds a sprite
  atlas.

## Data files and asset formats
Use the community tool first:
- Unity: UABEA, AssetRipper
- Unreal: FModel, UAssetGUI
  - **Fully compressed UE3 `.upk`** (UELib shows `Exports=0`): unpack a copy with Gildor's `decompress`
    (from UE Viewer), or, for LZO packages, with
    [`unpack_upk.py`](references/ue3-packages/README.md).
- Bethesda: xEdit, BSArch
- GameMaker: UndertaleModTool
- Genie: genieutils
- Source: VRF, Crowbar
- FromSoft: WitchyBND, Smithbox
- Godot: GDRE Tools

Before reversing a format yourself, search the community archives. Most game formats from the 2000s and
2010s were worked out there:
- **XeNTaX** (closed in 2023): the [forum](https://archive.org/details/forum.xentax.com_2023-08-15),
  [wiki](https://archive.org/details/wiki-wikixentaxcom_202305) and
  [attachments](https://archive.org/details/Xentax-forum-attachments-archive) are on archive.org, with a
  [public backup on GitHub](https://github.com/XeNTaXBackup/XeNTaXBackup.github.io);
- **[ZenHAX](https://zenhax.com/index.php.html)**: game file research and QuickBMS scripts (posts up to
  early 2023);
- **[The definitive guide to exploring file formats](https://archive.org/details/definitive-guide-to-exploring-file-formats)**
  (Mr. Mouse, XeNTaX): the classic tutorial on reading an unknown format.

Use them for format knowledge and tools, whatever its origin (betas, leaked SDKs). Don't download game files,
leaked code, SDKs or license keys from them.

For an **undocumented format**:
1. Collect several stock files. Compare sizes, and hex-dump the headers (`xxd | head`). Look for magic
   numbers, counts, offsets and tables of fixed-size records.
2. Form a hypothesis for the header, then the frame/record layout, then the compression. Check it by
   parsing every stock file without errors.
3. Write a reader that decodes to something viewable (PNGs, JSON) and **look at it**.
4. Write the writer, and **prove it with a round trip**: decode → encode → decode, compared against the
   original. The AoE2 SLD sprite writer was accepted only when it round-tripped the stock knight at
   0.9/255 mean error (`examples/aoe2-de-civ/sld.py`).
5. Only then write new files. Test them in game with one asset before batch-converting.

## Make it an oracle
- **Engine logic you port** (for a simulator, a trainer, a reimplementation): record real traces from the
  game (positions, velocities per tick) and replay them against your port. In the Terraria Eye of Cthulhu
  work, the Eye's velocity matched 99.9% once the port used the action applied on frame t+1. Float32
  constants mattered too (`0.2f` ≠ `0.2`). A leftover mismatch was traced to a hidden buff (Happy!, x1.21
  move speed). Replays find what reading the code misses.
- **For long RE runs:**
  - a journal file;
  - small verified steps;
  - cap attempts per problem (about 3 identical failures, then change approach);
  - commit every confirmed fact.

## When the mod crashes the game: isolate the hook, don't guess from the dump

A minidump only helps if it contains the offending code. It often doesn't: a small dump
(around 200 KB, few threads) records almost no code and no usable stack, and a DEP
(Data Execution Prevention) fault at something like `0x3d088889` can point at an
address that isn't even mapped — you cannot name the crashing function from it. Real
example: a dedicated server died ~0.65 s after "level started"; the dump proved nothing
about *which* hook did it.

Binary isolation beats reading dumps:
- Add environment flags to the injected library to disable hooks one at a time:
  `MYMOD_NO_HOOKS=1` skips all hooks, `MYMOD_NO_HOOK_<name>=1` skips one (install the
  rest). Log loudly in the runtime log which hooks were skipped, so runs are
  self-documenting ("хук tick отключён флагом ... (изоляция)", "3 of 4 hooks
  installed").
- Then run the game a few times: no hooks → stable; all hooks except the suspect →
  stable; with the suspect → crash. One bad detour is pinned in minutes without any
  dump forensics.
- Judge success by the **live runtime log and the still-running process**, not the
  dump: a line showing the game kept advancing ("GAME SESSION started", ping counters,
  frames ticking) is the oracle. Only then reopen the crash as a RE question (what does
  the target actually do, why does the detour fault).
- Isolate config-driven failures the same binary way: change one setting per launch
  (e.g. bot count / spawn mode) instead of reasoning from a config back to the crash.

## One engine flag, several consequences: patch branches, not the flag

A single field often drives unrelated features, and the first fix that flips it breaks
something else. Real example (BF3 server, bots as engine players): the player's
"is AI" byte made the engine apply the bot's input (needed for walking) and made the
client draw nameplates over bots. The same byte also deleted the player on death and
suppressed the "enemy killed" message. With the byte cleared, the kill message
returned but the nameplates vanished. The fix: keep the byte set, find every
`cmp byte [player+flag],0 / jcc` on the paths you do not want, and patch only those
branches. Rules for such patches:
- **Disassemble each site from the on-disk binary first** (capstone on the file is
  enough) and compare against the agent's or decompiler's claim: instruction, length,
  target.
- **All or nothing at runtime:** read all sites before writing any; if one byte
  differs (another build, already patched), write nothing and log which site failed.
  Accept an "already patched" site as matching.
- `VirtualProtect` → write → restore → `FlushInstructionCache`, log each site with
  its address and purpose, and add an env flag that disables the whole set.
- If your own code later calls the engine's "delete player", clear the flag first
  when the cleanup branch depends on it.

## Unknown pointer field? Read the live object before reversing its writer

A field that the engine fills only at some event (here: a player's pointer that is
empty until the human presses deploy) can usually be named in a minute. From a
separate process (`ReadProcessMemory`, read-only), dump the object it points to and
look for a string pointer among its first words. Frostbite data containers carry
their asset name at `+0x08`, e.g. `Characters/Soldiers/MpSoldier`. A value that is
identical across runs and teams is static level data. Find it yourself (heap scan by
vtable + name check) instead of waiting for the event that copies it.

## STATE.md beside MODLOG.md: hypotheses are not facts

In a long run the journal accumulates conclusions that later get refuted, and the next
reader — often another agent — takes them as facts. Keep `MODLOG.md` as an append-only
archive and maintain a short `STATE.md` (100–150 lines) next to it: what works, what
doesn't, what is refuted (with the disproving test), hypotheses explicitly marked
"not verified", and the single next step. A conclusion without a test that could falsify
it is written as a hypothesis; the words "finally" and "no alternatives" are banned —
in one project at least four such "final" conclusions were later refuted. When an
outside review lands (a second AI, a forum thread), save it verbatim in its own file,
mark every claim verified/guess, and distill the generalizable parts into the skill
you actually used.

Cheap diagnostics before expensive ones:
- **A crash handler that names the fault** (`SetUnhandledExceptionFilter` +
  `MiniDumpWriteDump`, or WER LocalDumps; log EIP as `module+RVA`) — before rolling
  anything back "one journal commit at a time".
- **Diff two live objects** (a working human player object vs the broken bot one) at
  the same moment. Dump generously past the "documented" size — fields often live
  beyond it. Fields non-zero in one and zero in the other are the candidates. Also
  read the tail of the original handler you bypassed: everything it writes after
  creation is exactly what your object is missing.
- **Bundle read-only probes into one run** (client log, memory diff, heap scan,
  read-back); "one hypothesis per run" applies only to probes that call into the engine.
- **Scan the heap instead of hooking constructors** that crash level load: with no
  ASLR, walk committed RW pages for a known vtable pointer and validate candidate
  fields under SEH — zero hooks, zero engine calls.
- **Automate the backup:** the launch script copies the last working DLL with a
  timestamp before every deploy; a rule that must be remembered manually gets broken
  exactly once.
- **Check the bridge before blaming the mod:** a C binding that reads argument index 2
  while the Lua side calls without `self` (the message lands at index 1) makes log
  lines vanish — a working loader then looks like a stuck state machine. Validate
  every `l_*` argument with `luaL_check*` and log every Lua error with a traceback
  unconditionally. If a mod "silently stalls", grep it for time sources (`os.clock`,
  delta time): a runtime returning 0/nil freezes step budgets and timers invisibly.
