// InlineHook.h — перехват функций движка BF3 и правка его памяти через ядро ev-core.dll.
//
// ★ Сборка 237 (26.10.2026): как у VU — код и память движка меняет только ядро (vu-core / ev-core.dll), расширение
// (veniceext / EVBots.dll) ставит свои обработчики через него. Сам механизм (8 байт E9 rel32 + NOP, кража целого числа
// инструкций — X86Length, трамплин, атомарная запись, побайтный откат) — ev-core\src\EvHook.cpp. Здесь — тонкий клиент
// с прежним интерфейсом: места вызова в EVBots.dll не менялись. Нет ev-core.dll — перехваты не ставятся (BadTarget).

#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace evbots {

/// Результат установки хука (коды совпадают с EvHookInstall ядра).
enum class HookResult {
    Ok,                ///< хук установлен
    AlreadyHooked,     ///< эта цель уже перехвачена
    BadTarget,         ///< адрес вне читаемой памяти, не исполняемый или нет ev-core.dll
    PrologueShort,     ///< не читаются украденные байты пролога
    UnknownPrologue,   ///< в первых инструкциях неопознанный код
};

const char* ToString(HookResult r);

class InlineHook {
public:
    InlineHook() = default;
    ~InlineHook();

    InlineHook(const InlineHook&) = delete;
    InlineHook& operator=(const InlineHook&) = delete;

    /// Ставит хук (через ядро). name — для журнала ядра (logs\ev-core.log).
    HookResult Install(void* target, void* detour, const char* name = nullptr);

    /// Снимает хук (ядро восстанавливает пролог побайтно).
    bool Remove();

    bool IsInstalled() const { return installed_; }

    /// Адрес вызова оригинала (трамплин ядра).
    void* Trampoline() const { return trampoline_; }

    void* Target() const { return target_; }
    void* Detour() const { return detour_; }

    /// Сколько байт пролога украдено (целое число инструкций, не меньше 8).
    size_t StolenBytes() const { return stolen_; }

    /// Сколько инструкций ушло в трамплин.
    size_t StolenInstructions() const { return instructions_; }

private:
    void* target_ = nullptr;
    void* detour_ = nullptr;
    void* trampoline_ = nullptr;
    size_t stolen_ = 0;
    size_t instructions_ = 0;
    bool installed_ = false;
};

/// Список установленных хуков EVBots.dll — чтобы RemoveAll работал по всем сразу.
class HookRegistry {
public:
    HookRegistry() {
        InitializeCriticalSection(&gate_);
        std::memset(hooks_, 0, sizeof(hooks_));
    }

    static HookRegistry& Instance();

    void Add(InlineHook* hook);
    void Remove(InlineHook* hook);
    size_t Count() const;
    size_t RemoveAll();

private:
    CRITICAL_SECTION gate_;
    InlineHook* hooks_[128];
    size_t count_ = 0;
};

/// Читается ли память по адресу (ядро).
bool IsExecutable(void* address);

/// Замена указателя в слоте движка (vtable и т.п.) через ядро. expect != nullptr — только если в слоте именно оно.
/// Прежнее значение — в *oldOut. true — заменено.
bool PatchPointer(void* slot, void* value, void* expect, void** oldOut, const char* name);

/// Запись байтов в память движка через ядро. expect != nullptr — только если там эти байты (правило 18).
bool PatchBytes(void* address, const void* expect, const void* bytes, size_t len, const char* name);

/// Есть ли ядро с перехватами (ev-core.dll 5+).
bool CoreHooksAvailable();

/// Экспорт ядра ev-core.dll по имени (nullptr — нет ядра или функции). Ядро грузится так же, как для перехватов.
FARPROC CoreExport(const char* name);

/// Частота движка этого процесса (ядро, EvFreq.cpp): 30, 60 или 120; без ядра — 30.
int CoreFrequencyHz();

/// ★ 238-3 (А1): тик Lua сервера идёт из кадра движка (EVBOTS_SERVER_TICK=frame) и этот поток — поток кадра. Тогда ждать
/// чего-то, что делает тот же поток (хук тика, ответ RCON сервера), нельзя. Определение — ModdingInterface.cpp.
bool EVBotsOnServerFrameThread();
void EVBotsSetServerFrameThread(unsigned long threadId);
/// Тик Lua сервера идёт из кадра движка (перехват кадра поставлен) — тогда никакой поток не должен ждать хук тика: кадр
/// может стоять на локе Lua, который держит ждущий (проверка 238-3: Level:Loaded из своего потока).
bool EVBotsServerFrameTickActive();
void EVBotsSetServerFrameTickActive();

}  // namespace evbots
