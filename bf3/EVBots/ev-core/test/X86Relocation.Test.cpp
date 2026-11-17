#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../src/X86Length.h"

namespace {
int passed = 0, failed = 0;
void Check(bool value, const char* name) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", name);
    value ? ++passed : ++failed;
}
using Function = int(__cdecl*)();
Function original = nullptr;
int __cdecl Detour() { return original() + 5; }
uint32_t CallTarget(const uint8_t* code, uint32_t base, size_t offset) {
    uint32_t relative = 0; std::memcpy(&relative, code + offset + 1, 4);
    return base + static_cast<uint32_t>(offset + 5) + relative;
}
void Rejected(const uint8_t* code, size_t size, const char* label) {
    uint8_t output[32]; std::memset(output, 0xCC, sizeof(output));
    Check(!evcore::RelocatePrologue(code, size, 0x1000, 0x4000, output) && output[0] == 0xCC, label);
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) { std::puts("Usage: X86Relocation.Test.exe core-dll"); return 2; }
    const uint8_t visual[] = {0x51,0x8D,0x0C,0x24,0xE8,0xD7,0x03,0x37,0xFF};
    uint8_t output[32] = {};
    size_t span = 0, count = 0;
    Check(!evcore::StolenSpan(visual, sizeof(visual), 8, &span, &count), "default span still rejects relative calls");
    Check(evcore::StolenSpan(visual, sizeof(visual), 8, &span, &count, true) && span == 9 && count == 3,
          "actual VisualTerrainPostLoad prologue spans three complete instructions");
    Check(evcore::RelocatePrologue(visual, sizeof(visual), 0x010BA540, 0x70001000, output) &&
          CallTarget(output, 0x70001000, 4) == 0x0042A920, "backward native CALL keeps its exact target");
    const uint8_t forward[] = {0xE8,0xF0,0x00,0x00,0x00,0x50,0x59,0x90};
    Check(evcore::RelocatePrologue(forward, sizeof(forward), 0x2000, 0x5000, output) &&
          CallTarget(output, 0x5000, 0) == 0x20F5, "forward CALL keeps its exact target");
    Check(evcore::RelocatePrologue(forward, sizeof(forward), 0xFFFFFFF0, 0x70000000, output) &&
          CallTarget(output, 0x70000000, 0) == 0xE5, "x86 address wrap is explicit modulo 32 bits");
    const uint8_t internal[] = {0xE8,0xFC,0xFF,0xFF,0xFF,0x90,0x90,0x90};
    const uint8_t callNext[] = {0xE8,0,0,0,0,0x58,0x90,0x90};
    const uint8_t call16[] = {0x66,0xE8,0x10,0,0x90,0x90,0x90,0x90};
    const uint8_t branch[] = {0x75,0x12,0x90,0x90,0x90,0x90,0x90,0x90};
    Rejected(internal, sizeof(internal), "calls into replaced bytes are rejected without writing output");
    Rejected(callNext, sizeof(callNext), "CALL-next get-PC idiom is rejected");
    Rejected(call16, sizeof(call16), "prefixed 16-bit CALL is rejected");
    Rejected(branch, sizeof(branch), "conditional branch remains rejected");
    Rejected(visual, sizeof(visual) - 1, "truncated rel32 is rejected");
    const uint8_t spm[] = {0x83,0xEC,0x08,0x56,0x8B,0xF1,0x80,0xBE,0xF1,0,0,0,0};
    Check(evcore::StolenSpan(spm, sizeof(spm), 8, &span, &count) && span == 13 && count == 4,
          "actual ServerPlayerManager prologue decodes without relative flow");

    HMODULE core = LoadLibraryW(argv[1]);
    if (!core) { Check(false, "load test core"); return 1; }
    using Install = int(__cdecl*)(void*,void*,const char*,void**,unsigned*,unsigned*);
    using Remove = int(__cdecl*)(void*);
    auto install = reinterpret_cast<Install>(GetProcAddress(core, "EvHookInstall"));
    auto remove = reinterpret_cast<Remove>(GetProcAddress(core, "EvHookRemove"));
    if (!install || !remove) { Check(false, "core hook exports"); return 1; }
    auto* memory = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!memory) return 1;
    // Execute only our own tiny fixture, never any game code. Same first three
    // instructions as the native terrain prologue, with the callee in this page.
    const uint8_t callee[] = {0xB8,7,0,0,0,0xC3};
    std::memcpy(memory, visual, sizeof(visual));
    const uint32_t displacement = 64 - 9;
    std::memcpy(memory + 5, &displacement, 4);
    memory[9] = 0x59; memory[10] = 0xC3;
    std::memcpy(memory + 64, callee, sizeof(callee));
    FlushInstructionCache(GetCurrentProcess(), memory, 128);
    uint8_t before[16]; std::memcpy(before, memory, sizeof(before));
    auto function = reinterpret_cast<Function>(memory);
    Check(function() == 7, "fixture before hook");
    void* trampoline = nullptr; unsigned stolen = 0, instructions = 0;
    SetEnvironmentVariableA("EVBOTS_HOOK_REL32", "0");
    Check(install(memory, reinterpret_cast<void*>(&Detour), "rel32 disabled fixture", &trampoline, &stolen, &instructions) == 4 &&
          trampoline == nullptr && std::memcmp(before, memory, sizeof(before)) == 0, "env switch refuses CALL without mutation");
    SetEnvironmentVariableA("EVBOTS_HOOK_REL32", nullptr);
    const int installed = install(memory, reinterpret_cast<void*>(&Detour), "rel32 executable fixture", &trampoline, &stolen, &instructions);
    Check(installed == 0 && trampoline && stolen == 9 && instructions == 3, "actual core installs relocated trampoline");
    if (installed == 0 && trampoline) {
        original = reinterpret_cast<Function>(trampoline);
        Check(original() == 7, "trampoline executes relocated CALL and returns with balanced stack");
        Check(function() == 12, "detour calls original trampoline");
        Check(remove(memory) == 1 && std::memcmp(before, memory, sizeof(before)) == 0 && function() == 7,
              "removal restores exact bytes and original behavior");
    }
    std::memcpy(memory + 128, callNext, sizeof(callNext));
    Check(install(memory + 128, reinterpret_cast<void*>(&Detour), "unsafe get-PC fixture", &trampoline, &stolen, &instructions) == 4 &&
          trampoline == nullptr && std::memcmp(memory + 128, callNext, sizeof(callNext)) == 0,
          "actual core rejects unsafe call before patching");
    VirtualFree(memory, 0, MEM_RELEASE);
    std::printf("RESULT pass=%d fail=%d\n", passed, failed);
    return failed ? 1 : 0;
}
