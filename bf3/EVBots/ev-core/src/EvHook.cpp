// EvHook.cpp — перехват функций движка и правка его памяти. Как у VU: в процессе игры код и память движка меняет
// только ядро (vu-core / ev-core), расширение (veniceext / EVBots.dll) лишь регистрирует свои обработчики.
// Перенесено из EVBots.dll (InlineHook.cpp) в сборке 237, 26.10.2026 — решение пользователя «всё сразу».
//
// Перехват: ровно 8 байт (E9 rel32 + 3 NOP), атомарно под LOCK; крадётся ЦЕЛОЕ число инструкций не меньше 8 байт
// (X86Length.cpp); трамплин — украденные инструкции, затем JMP назад; снятие — побайтный откат сохранённой копии.
// Замена указателя (слот vtable и т.п.) — InterlockedExchange; патч байтов — со сверкой ожидаемых (правило 18).
// Каждая операция — строкой в logs\ev-core.log с именем, которое передал вызывающий.

#include <Windows.h>

#include <cstdint>
#include <cstring>

#include "EvCore.h"
#include "X86Length.h"

namespace {

constexpr SIZE_T kPatchSize = 8;
constexpr SIZE_T kMinStolen = kPatchSize;  // меньше нельзя: трамплин вернулся бы на затёртый NOP-ами байт
constexpr int kMaxHooks = 128;

// Коды возврата EvHookInstall — совпадают с evbots::HookResult.
constexpr int kOk = 0, kAlready = 1, kBadTarget = 2, kPrologueShort = 3, kUnknownPrologue = 4;

struct HookRec {
    void* target;
    void* detour;
    void* trampoline;
    unsigned stolen;
    unsigned instructions;
    uint8_t original[kPatchSize];
    char name[48];
    bool used;
};

HookRec g_hooks[kMaxHooks];
SRWLOCK g_hooksLock = SRWLOCK_INIT;

const char* NameOr(const char* name) { return (name != nullptr && name[0] != '\0') ? name : "?"; }

bool Write8Atomic(void* address, uint64_t value, DWORD* exceptionCode) {
    *exceptionCode = 0;
    __try {
        volatile LONG64* slot = static_cast<volatile LONG64*>(address);
        const LONG64 desired = static_cast<LONG64>(value);
        for (;;) {
            const LONG64 assumed = *slot;
            if (InterlockedCompareExchange64(slot, desired, assumed) == assumed) return true;
        }
    } __except (*exceptionCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Запись с временным снятием защиты страницы; 8 байт — одной атомарной операцией.
bool WriteMem(void* address, const void* data, SIZE_T size, const char* name) {
    if (address == nullptr || data == nullptr || size == 0) return false;
    DWORD oldProtect = 0;
    if (!::VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        FileLog("[hook] %s: не снять защиту с %p, ошибка %lu", NameOr(name), address, ::GetLastError());
        return false;
    }
    bool ok = false;
    // 8 байт — всегда lock cmpxchg8b: на x86 инструкция с LOCK атомарна и по невыровненному адресу (split lock — медленно,
    // но атомарно), а обычная запись дала бы полузаписанный переход. Прологи функций выровнены по 16 (237: все 29 целей).
    if (size == 8) {
        uint64_t value = 0;
        std::memcpy(&value, data, 8);
        DWORD code = 0;
        ok = Write8Atomic(address, value, &code);
        if (!ok) FileLog("[hook] %s: атомарная запись по %p — исключение 0x%08lX", NameOr(name), address, code);
    } else {
        __try {
            std::memcpy(address, data, size);
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
    }
    DWORD ignored = 0;
    ::VirtualProtect(address, size, oldProtect, &ignored);
    if (ok) ::FlushInstructionCache(::GetCurrentProcess(), address, size);
    return ok;
}

bool IsExecutableMem(const void* address) {
    if (address == nullptr) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (::VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    return (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE | PAGE_EXECUTE_READ |
                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

bool CopyPrologue(const void* target, uint8_t* stolen, size_t stolenBytes, uint8_t* original) {
    __try {
        std::memcpy(stolen, target, stolenBytes);
        std::memcpy(original, target, kPatchSize);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

HookRec* FindByTarget(void* target) {
    for (HookRec& h : g_hooks) {
        if (h.used && h.target == target) return &h;
    }
    return nullptr;
}

bool RemoveRec(HookRec& h) {
    bool ok = true;
    if (h.trampoline != nullptr) {
        ok = WriteMem(h.target, h.original, kPatchSize, h.name);
        ::VirtualFree(h.trampoline, 0, MEM_RELEASE);
    }
    FileLog("[hook] %s: снят %p (%s)", h.name, h.target, ok ? "пролог восстановлен" : "ОТКАТ НЕ УДАЛСЯ");
    std::memset(&h, 0, sizeof(h));
    return ok;
}

}  // namespace

/// Перехват target → detour. Возврат: 0 поставлен, 1 цель уже перехвачена, 2 плохой адрес, 3 пролог не читается,
/// 4 неизвестная инструкция в прологе. trampolineOut — адрес вызова оригинала.
extern "C" __declspec(dllexport) int __cdecl EvHookInstall(void* target, void* detour, const char* name,
                                                         void** trampolineOut, unsigned* stolenOut,
                                                         unsigned* instructionsOut) {
    if (trampolineOut != nullptr) *trampolineOut = nullptr;
    if (target == nullptr || detour == nullptr || !IsExecutableMem(target)) {
        FileLog("[hook] %s: не ставится — цель %p / обходник %p", NameOr(name), target, detour);
        return kBadTarget;
    }
    ::AcquireSRWLockExclusive(&g_hooksLock);
    struct Unlock { ~Unlock() { ::ReleaseSRWLockExclusive(&g_hooksLock); } } unlock;

    if (HookRec* prev = FindByTarget(target)) {
        FileLog("[hook] %s: цель %p уже перехвачена (%s) — повтор пропущен", NameOr(name), target, prev->name);
        return kAlready;
    }
    HookRec* rec = nullptr;
    for (HookRec& h : g_hooks) {
        if (!h.used) { rec = &h; break; }
    }
    if (rec == nullptr) {
        FileLog("[hook] %s: таблица перехватов полна (%d)", NameOr(name), kMaxHooks);
        return kBadTarget;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    size_t readable = 32;
    if (::VirtualQuery(target, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        readable = static_cast<size_t>(static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize - static_cast<uint8_t*>(target));
    }
    size_t stolenBytes = 0, instructionCount = 0;
    char relocationFlag[8] = {};
    const bool allowRel32Calls = !(GetEnvironmentVariableA("EVBOTS_HOOK_REL32", relocationFlag, sizeof(relocationFlag)) == 1 && relocationFlag[0] == '0');
    if (!evcore::StolenSpan(static_cast<const uint8_t*>(target), readable, kMinStolen, &stolenBytes, &instructionCount, allowRel32Calls)) {
        FileLog("[hook] %s: не ставится — в прологе %p неизвестная инструкция или относительный переход/вызов",
                NameOr(name), target);
        return kUnknownPrologue;
    }
    uint8_t stolen[32] = {};
    uint8_t original[kPatchSize] = {};
    if (stolenBytes > sizeof(stolen)) return kUnknownPrologue;
    if (!CopyPrologue(target, stolen, stolenBytes, original)) {
        FileLog("[hook] %s: не ставится — не читаются %zu байт пролога %p", NameOr(name), stolenBytes, target);
        return kPrologueShort;
    }
    // Трамплин: сначала украденные инструкции, ПОТОМ JMP назад (наоборот — украденное не выполнится никогда).
    auto* tramp = static_cast<uint8_t*>(::VirtualAlloc(nullptr, stolenBytes + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (tramp == nullptr) {
        FileLog("[hook] %s: не ставится — VirtualAlloc %lu", NameOr(name), ::GetLastError());
        return kBadTarget;
    }
    if (!evcore::RelocatePrologue(stolen, stolenBytes, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(target)),
                                  static_cast<uint32_t>(reinterpret_cast<uintptr_t>(tramp)), tramp)) {
        ::VirtualFree(tramp, 0, MEM_RELEASE);
        FileLog("[hook] %s: unsafe relative call in prologue %p; target unchanged", NameOr(name), target);
        return kUnknownPrologue;
    }
    tramp[stolenBytes] = 0xE9;
    const int32_t backRel = static_cast<int32_t>((intptr_t)target - (intptr_t)tramp - 5);
    std::memcpy(tramp + stolenBytes + 1, &backRel, 4);
    ::FlushInstructionCache(::GetCurrentProcess(), tramp, stolenBytes + 5);

    uint8_t patch[kPatchSize] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    const int32_t detourRel = static_cast<int32_t>((intptr_t)detour - (intptr_t)target - 5);
    std::memcpy(patch + 1, &detourRel, 4);
    // ★ 238-3 (проверка 26.10): трамплин готов — отдаём его ДО записи перехода. Раньше он отдавался после записи и строки
    // журнала: поток игры, вошедший в обходник в этом окне, звал оригинал по nullptr. lock cmpxchg8b в WriteMem — барьер.
    if (trampolineOut != nullptr) *trampolineOut = tramp;
    if (!WriteMem(target, patch, kPatchSize, name)) {
        if (trampolineOut != nullptr) *trampolineOut = nullptr;
        ::VirtualFree(tramp, 0, MEM_RELEASE);
        FileLog("[hook] %s: не ставится — запись 8 байт по %p не удалась", NameOr(name), target);
        return kBadTarget;
    }
    rec->target = target;
    rec->detour = detour;
    rec->trampoline = tramp;
    rec->stolen = static_cast<unsigned>(stolenBytes);
    rec->instructions = static_cast<unsigned>(instructionCount);
    std::memcpy(rec->original, original, kPatchSize);
    ::strncpy_s(rec->name, sizeof(rec->name), NameOr(name), _TRUNCATE);
    rec->used = true;
    FileLog("[hook] %s: поставлен %p -> %p, трамплин %p, украдено %zu байт (%zu инстр.): %s", rec->name, target, detour,
            tramp, stolenBytes, instructionCount, evcore::HexBytes(stolen, stolenBytes).c_str());
    if (stolenOut != nullptr) *stolenOut = rec->stolen;
    if (instructionsOut != nullptr) *instructionsOut = rec->instructions;
    return kOk;
}

/// Снять перехват по адресу цели. 1 — снят (или не стоял), 0 — откат пролога не удался.
extern "C" __declspec(dllexport) int __cdecl EvHookRemove(void* target) {
    ::AcquireSRWLockExclusive(&g_hooksLock);
    HookRec* rec = FindByTarget(target);
    const bool ok = rec == nullptr || RemoveRec(*rec);
    ::ReleaseSRWLockExclusive(&g_hooksLock);
    return ok ? 1 : 0;
}

/// Снять все перехваты. Возврат — сколько было.
extern "C" __declspec(dllexport) int __cdecl EvHookRemoveAll() {
    ::AcquireSRWLockExclusive(&g_hooksLock);
    int n = 0;
    for (HookRec& h : g_hooks) {
        if (h.used) { RemoveRec(h); ++n; }
    }
    ::ReleaseSRWLockExclusive(&g_hooksLock);
    return n;
}

extern "C" __declspec(dllexport) int __cdecl EvHookCount() {
    ::AcquireSRWLockShared(&g_hooksLock);
    int n = 0;
    for (const HookRec& h : g_hooks) n += h.used ? 1 : 0;
    ::ReleaseSRWLockShared(&g_hooksLock);
    return n;
}

extern "C" __declspec(dllexport) int __cdecl EvIsExecutable(const void* address) { return IsExecutableMem(address) ? 1 : 0; }

/// Замена указателя в слоте (vtable, таблица переходов): *slot = value атомарно, прежнее значение — в *oldOut.
/// expect != nullptr — замена только если в слоте именно оно (правило 18). 1 — заменено.
extern "C" __declspec(dllexport) int __cdecl EvPatchPointer(void** slot, void* value, void* expect, void** oldOut,
                                                          const char* name) {
    if (oldOut != nullptr) *oldOut = nullptr;
    if (slot == nullptr) return 0;
    DWORD oldProtect = 0;
    if (!::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        FileLog("[patch] %s: слот %p — VirtualProtect %lu", NameOr(name), (void*)slot, ::GetLastError());
        return 0;
    }
    void* prev = nullptr;
    bool ok = false;
    __try {
        if (expect != nullptr) {
            prev = ::InterlockedCompareExchangePointer(slot, value, expect);
            ok = prev == expect;
        } else {
            prev = ::InterlockedExchangePointer(slot, value);
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DWORD ignored = 0;
    ::VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
    ::FlushInstructionCache(::GetCurrentProcess(), slot, sizeof(void*));
    if (oldOut != nullptr) *oldOut = prev;
    FileLog("[patch] %s: слот %p: %p -> %p %s", NameOr(name), (void*)slot, prev, value,
            ok ? "заменён" : "НЕ заменён (в слоте не то, что ждали)");
    return ok ? 1 : 0;
}

/// Запись байтов в память движка (код или данные). expect != nullptr — запись только если там именно эти байты
/// (правило 18: не совпало — не пишем). 1 — записано.
extern "C" __declspec(dllexport) int __cdecl EvPatchBytes(void* address, const void* expect, const void* bytes,
                                                        unsigned len, const char* name) {
    if (address == nullptr || bytes == nullptr || len == 0) return 0;
    if (expect != nullptr) {
        bool same = false;
        __try {
            same = std::memcmp(address, expect, len) == 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            same = false;
        }
        if (!same) {
            FileLog("[patch] %s: %p — байты не те, НЕ пишу", NameOr(name), address);
            return 0;
        }
    }
    const bool ok = WriteMem(address, bytes, len, name);
    if (len > 8 || expect != nullptr) {  // частые записи данных (зонды) в журнал не льём
        FileLog("[patch] %s: %p, %u байт — %s", NameOr(name), address, len, ok ? "записано" : "НЕ записано");
    }
    return ok ? 1 : 0;
}
