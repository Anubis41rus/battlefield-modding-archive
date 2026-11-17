// InlineHook.cpp — клиент перехватов ядра ev-core.dll (сборка 237). Механизм — ev-core\src\EvHook.cpp.

#include "InlineHook.h"

#include "EVBotsLog.h"

namespace evbots {
namespace {

using HookInstallFn = int(__cdecl*)(void*, void*, const char*, void**, unsigned*, unsigned*);
using HookRemoveFn = int(__cdecl*)(void*);
using IsExecFn = int(__cdecl*)(const void*);
using PatchPointerFn = int(__cdecl*)(void**, void*, void*, void**, const char*);
using PatchBytesFn = int(__cdecl*)(void*, const void*, const void*, unsigned, const char*);

struct CoreApi {
    HookInstallFn install = nullptr;
    HookRemoveFn remove = nullptr;
    IsExecFn isExec = nullptr;
    PatchPointerFn patchPointer = nullptr;
    PatchBytesFn patchBytes = nullptr;
};

// ev-core.dll: загрузчик ставит её первой; если нет (тест-хост, client-harness) — из папки EVBots.dll, как lua.dll.
HMODULE CoreModule() {
    HMODULE m = ::GetModuleHandleA("ev-core.dll");
    if (m != nullptr) return m;
    HMODULE self = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&CoreModule), &self)) {
        return nullptr;
    }
    wchar_t path[MAX_PATH] = {};
    DWORD n = ::GetModuleFileNameW(self, path, MAX_PATH);
    while (n > 0 && path[n - 1] != L'\\' && path[n - 1] != L'/') --n;
    if (n == 0 || n + 12 >= MAX_PATH) return nullptr;
    ::wcscpy_s(path + n, MAX_PATH - n, L"ev-core.dll");
    return ::LoadLibraryW(path);
}

const CoreApi& Core() {
    static CoreApi api = [] {
        CoreApi a;
        HMODULE m = CoreModule();
        if (m != nullptr) {
            a.install = reinterpret_cast<HookInstallFn>(::GetProcAddress(m, "EvHookInstall"));
            a.remove = reinterpret_cast<HookRemoveFn>(::GetProcAddress(m, "EvHookRemove"));
            a.isExec = reinterpret_cast<IsExecFn>(::GetProcAddress(m, "EvIsExecutable"));
            a.patchPointer = reinterpret_cast<PatchPointerFn>(::GetProcAddress(m, "EvPatchPointer"));
            a.patchBytes = reinterpret_cast<PatchBytesFn>(::GetProcAddress(m, "EvPatchBytes"));
        }
        if (a.install == nullptr || a.remove == nullptr || a.isExec == nullptr || a.patchPointer == nullptr ||
            a.patchBytes == nullptr) {
            LOG_ERR("EVBots: ev-core.dll с перехватами (версия 5+) не найдена — перехваты и патчи движка НЕ ставятся");
            a = CoreApi{};
        } else {
            LOG_INFO("EVBots: перехваты и правка памяти движка — через ev-core.dll (как VU: vu-core)");
        }
        return a;
    }();
    return api;
}

}  // namespace

const char* ToString(HookResult r) {
    switch (r) {
        case HookResult::Ok: return "installed";
        case HookResult::AlreadyHooked: return "already hooked";
        case HookResult::BadTarget: return "bad target address";
        case HookResult::PrologueShort: return "prologue too short";
        case HookResult::UnknownPrologue: return "unknown instruction in prologue";
    }
    return "unknown";
}

bool CoreHooksAvailable() { return Core().install != nullptr; }

FARPROC CoreExport(const char* name) {
    static HMODULE s_core = CoreModule();
    return (s_core != nullptr && name != nullptr) ? ::GetProcAddress(s_core, name) : nullptr;
}

int CoreFrequencyHz() {
    using FreqFn = int(__cdecl*)();
    static const FreqFn fn = reinterpret_cast<FreqFn>(CoreExport("EvFreqHz"));
    const int hz = fn != nullptr ? fn() : 30;
    return (hz == 60 || hz == 120) ? hz : 30;
}

bool IsExecutable(void* address) {
    return Core().isExec != nullptr && Core().isExec(address) != 0;
}

bool PatchPointer(void* slot, void* value, void* expect, void** oldOut, const char* name) {
    if (oldOut != nullptr) *oldOut = nullptr;
    if (Core().patchPointer == nullptr) return false;
    return Core().patchPointer(static_cast<void**>(slot), value, expect, oldOut, name) != 0;
}

bool PatchBytes(void* address, const void* expect, const void* bytes, size_t len, const char* name) {
    if (Core().patchBytes == nullptr) return false;
    return Core().patchBytes(address, expect, bytes, static_cast<unsigned>(len), name) != 0;
}

InlineHook::~InlineHook() {
    if (installed_) Remove();
}

HookResult InlineHook::Install(void* target, void* detour, const char* name) {
    if (installed_) {
        LOG_WARN("target %p already hooked (detour %p), repeated install skipped", target_, detour_);
        return HookResult::AlreadyHooked;
    }
    if (Core().install == nullptr) return HookResult::BadTarget;
    // ★ 238-3: трамплин ядро пишет прямо в trampoline_ ДО записи перехода — обходник, вызванный потоком игры сразу после
    // установки, уже видит оригинал (раньше — локальная переменная и присваивание после возврата: окно с nullptr).
    unsigned stolen = 0, instr = 0;
    const int r = Core().install(target, detour, name, &trampoline_, &stolen, &instr);
    void* const tramp = trampoline_;
    if (r < 0 || r > static_cast<int>(HookResult::UnknownPrologue)) return HookResult::BadTarget;
    const HookResult res = static_cast<HookResult>(r);
    if (res != HookResult::Ok) {
        LOG_ERR("хук не ставится (ядро): цель %p — %s", target, ToString(res));
        return res;
    }
    target_ = target;
    detour_ = detour;
    stolen_ = stolen;
    instructions_ = instr;
    installed_ = true;
    HookRegistry::Instance().Add(this);
    LOG_INFO("hook installed (ev-core): target %p -> detour %p, trampoline %p; stolen %zu bytes (%zu instr.)", target,
             detour, tramp, stolen_, instructions_);
    return HookResult::Ok;
}

bool InlineHook::Remove() {
    if (!installed_) return true;
    const bool ok = Core().remove != nullptr && Core().remove(target_) != 0;
    if (!ok) {
        LOG_ERR("hook removal failed; active detour and trampoline preserved: %p", target_);
        return false;
    }
    installed_ = false;
    target_ = nullptr;
    detour_ = nullptr;
    trampoline_ = nullptr;
    stolen_ = 0;
    instructions_ = 0;
    HookRegistry::Instance().Remove(this);
    return ok;
}

// ── реестр ────────────────────────────────────────────────────────────────

HookRegistry& HookRegistry::Instance() {
    static HookRegistry instance;
    return instance;
}

void HookRegistry::Add(InlineHook* hook) {
    if (hook == nullptr) return;
    EnterCriticalSection(&gate_);
    if (count_ < sizeof(hooks_) / sizeof(hooks_[0])) hooks_[count_++] = hook;
    LeaveCriticalSection(&gate_);
}

void HookRegistry::Remove(InlineHook* hook) {
    EnterCriticalSection(&gate_);
    for (size_t i = 0; i < count_; i++) {
        if (hooks_[i] == hook) {
            hooks_[i] = hooks_[count_ - 1];
            hooks_[count_ - 1] = nullptr;
            count_--;
            break;
        }
    }
    LeaveCriticalSection(&gate_);
}

size_t HookRegistry::Count() const { return count_; }

size_t HookRegistry::RemoveAll() {
    EnterCriticalSection(&gate_);
    InlineHook* list[128];
    const size_t n = count_;
    for (size_t i = 0; i < n; ++i) list[i] = hooks_[i];
    LeaveCriticalSection(&gate_);
    for (size_t i = 0; i < n; i++) {
        if (list[i] != nullptr) list[i]->Remove();  // Remove сам вычёркивает из реестра
    }
    return n;
}

}  // namespace evbots
