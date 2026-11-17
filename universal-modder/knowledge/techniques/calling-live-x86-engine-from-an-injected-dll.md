---
kind: technique
title: "Calling a live x86 game engine from an injected DLL"
game: "Battlefield 3"
games_also: []
game_version: "1.6.0.0 dedicated server (bf3_server_Final.exe), no patch"
platform: windows
engine: frostbite
route: native-hook
tools: ["MSVC (cl.exe, x86)", "Ghidra headless 12.1.4", "inline hooking"]
anti_cheat: "PunkBuster (offline/LAN only, never touched)"
status: in-progress
agents: ["OpenCode (space-bunny-free)"]
humans: ["Anubis"]
date: 2026-10-11
links: []
tags: [native-hook, inline-hook, reverse-engineering, x86, thiscall, calling-convention, frostbite, player-spawn, embedded-lua]
---

# Calling a live x86 game engine from an injected DLL: what actually breaks

Derived from the BF3 server work (`Legacy\legacyext`), where a Lua mod's bots
had to be created inside the running Frostbite server. Every rule below cost
at least one run, and two of them crashed the user's game client.

**The general shape:** you inject a DLL into a game, find the engine function
that creates a player, and call it yourself to spawn something the game has no
UI for (AI, companions, test objects). It is the most valuable and most
hostile kind of modding, because the engine assumes every object it holds came
through its own door.

## Rule 1 — the calling convention is the first crash

Most engine code is `__thiscall`: `this` in ECX, *all* arguments on the stack.
Declaring the function pointer `__fastcall` puts the first argument in EDX and
shifts everything else — the callee reads garbage, and the epilogue's
`RET 0xN` unwinds a frame it never built.

MSVC only allows `__thiscall` on member functions (C3865), so you cannot just
write it. The workaround is a **dummy parameter**:

```cpp
// thiscall emulation: fake the EDX slot, then declare it __fastcall
int __fastcall Detour(void* self, void* /*edx*/, void* a1, void* a2);
using Tramp = int(__fastcall*)(void*, void*, void*, void*);
result = ((Tramp)trampoline)(self, nullptr, a1, a2);
```

Trampolines (`self, nullptr, ...`) and detours (`self, /*edx*/, ...`) must be
declared consistently or the stack silently drifts.

## Rule 2 — get the argument count from `RET imm`, not from the decompiler

The decompiler's parameter list is a guess. The epilogue is not: `ret 0xC`
means three stack arguments, `ret 0x10` means four. A detour declared with the
wrong count reads arguments from the wrong slots.

Getting this wrong is the same failure mode as rule 1 and looks identical: an
assert or a breakpoint several frames away from the real mistake.

## Rule 3 — decompiler addresses are VAs, your loader math wants RVAs

IDA/Ghidra report addresses with the image base baked in (`0x00E644B0`).
`GetModuleHandle("game.exe")` returns `0x00400000`. Adding the VA to the base
lands you `+0x400000` into the middle of `.text`, which usually looks like a
trampoline of `C3 CC CC` and lands in your own crash report rather than
anywhere useful. **Subtract the image base before adding.**

## Rule 4 — never call engine functions on objects you have not validated

Enumerating candidate objects and calling a member function on each one will
crash the server. Plenty of objects are captured mid-construction and their
vtable slot does not point at what you think.

Score candidates with plain memory reads (`VirtualQuery`, guarded pointer
reads) and make **exactly one** real engine call, on the best candidate, wrapped
in `__try`/`__except` so a single bad object degrades to a logged refusal
instead of taking the process down.

## Rule 5 — the log that stops mid-sentence is the crash site

When a process dies, the last line of its log is the last thing that finished.
If a diagnostic prints four values and stops after the third, the fault is
between the third and the fourth — that is often faster than reasoning about
the code, and it is falsifiable in one run.

Corollary: put one log line at the **very top** of a suspect function. No line
means the crash is in the *caller*, not the callee. That one test splits a
search space in half.

## Rule 6 — capture, never guess

Guessing an argument's meaning produced two wasted sessions here. Hooking the
function and logging the real `this` and arguments on the first genuine call
answers it in a single run:

```
TRACE spawnMgrGet(this=F691EC80 -> F6AD1480)     ← real, captured
```

One attempt guessed that `this` was a spawn-point object; the captured value
showed the function reads a vector at `this+0xCA8` and returns its last
element — which is why the guessed read produced `begin=0x3F800000` (a float,
not a pointer).

## Rule 7 — some engine entry points have no call sites at all

`getReferencesTo(function) == 0` does not mean dead code. It means it is called
**virtually**. Find the pointer in `.rdata` instead, walk backwards to the
vtable start, and count the slot: `vtable + 0xBC` for slot 47. The functions
that write that vtable pointer are the constructors — hooking them enumerates
every instance of that class as the level loads, which is far more reliable
than guessing offsets.

## Rule 8 — transient buffers die the moment the call returns

A buffer passed to a "create object from descriptor" entry point belongs to
the connection that supplied it and is freed as soon as the call returns.
Saving the *pointer* and using it a minute later passes freed memory: it may
still be readable, so the failure looks like nonsense (a name field reading
`NULL`, a pointer that is now a float) rather than an obvious bad access.

Save a **copy** inside the hook while the buffer is live.

## Rule 9 — identity slots must be unique

Anything keyed by an index — player slot, entity handle — must not reuse a live
one. Handing a new object an occupied slot silently displaces the incumbent:
the human player loses control and the manager's array ends up holding only
your object. Compute the slot as (current count + 1) rather than hardcoding.

## Rule 10 — embedded scripting languages break on C escapes

A Lua chunk inside a C++ raw string (`R"LUA(...)LUA"`) is raw text. Writing
`\\"` to escape a quote in the Lua source produces `\\` + `"` to the Lua
parser: the `\\` is an escaped backslash and the `"` *closes the string early*.
The chunk then fails with `unfinished string near '" ..'` and the whole
library silently does not register — the symptom looks like "the mod can't find
our global", not "our C++ has a quoting bug".

If a script-bearing library suddenly fails to register, read the parser error
before suspecting the mod.

## Rule 11 — beware `namespace X` inside a file already in `namespace X`

Wrapping a block in `namespace legacy { ... }` inside a file whose content is
already inside `namespace legacy` yields `legacy::legacy::Foo`. It compiles,
it links within the file, and it fails at link time for anything that declares
the symbol in the outer namespace — or, worse, silently links against a
different overload. Also remember that an unnamed namespace gives internal
linkage: a function declared in the named namespace but defined in the unnamed
one is `LNK2019`, while the same declaration and definition both in the unnamed
namespace is `C5046`-as-error. Pick one and keep the declaration where the
definition is.

## Rule 12 — back up the last known-good binary

Before every rebuild-and-deploy cycle, copy the DLL that currently works into
a dated backup folder. Code can always be reverted by editing; a *behaviourally
good* binary that has been overwritten three times cannot be recovered, and
"revert to the run that worked" stops being an option exactly when you need it.
