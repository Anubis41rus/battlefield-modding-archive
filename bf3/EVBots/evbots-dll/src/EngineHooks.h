// EngineHooks.h — перехват функций движка BF3.
//
// ── ЧТО ЗДЕСЬ И ЧЕГО НЕ ──────────────────────────────────────────────────
// Механизм перехвата (inline 8 байт + трамплин) — базовая техника x86, а не
// чьё-то решение: без него не сделаешь ни один мод для BF3, включая
// аналог VeniceEXT. Реализация в InlineHook.cpp проверена самопроверкой.
//
// Четыре цели ниже — НЕ поверхность событий EVBots. Это проверка
// механизма на функциях самого теста, плюс заготовка на будущее.
//
// ПОВЕРХНОСТЬ СОБЫТИЙ для Lua-модов берётся из fun-bots: список хуков и
// событий в stand\funbots-api-usage.md и stand\PHASE1-MAP.md.
// Своя поверхность — из VE-API VU (vext\ref\server\hook, vext\ref\server\event).
//
// Пока эти адреса не найдены в движке, слой Lua (LuaRuntime, ModLoader) будет
// работать поверх символьного интерфейса движка, а список событий —
// пополняться по мере нахождения функций.

#pragma once

#include "InlineHook.h"
#include "ModdingInterface.h"

namespace evbots {

// ── проверка механизма ───────────────────────────────────────────────────
// Обходные функции: в них попадает управление из трамплина. Тест зовёт их
// напрямую — так проверяется, что трамплин возвращает управление в
// оригинал.
void OnNewRoundTrampoline();
void OnTickTrampoline();
void OnDamageTrampoline();
void ChatMessageTrampoline();

/// Наши detour-функции: их адреса пишутся в начало функции движка.
///
/// Возвращают то же, что вернул оригинал: на x86 результат функции лежит в
/// EAX, поэтому обходник обязан вернуть его дальше. Если этого не делать,
/// перехваченная функция начинает возвращать мусор — именно так и выглядели
/// первые провалы самопроверки.
///
/// В боевой работе у каждой цели будет своя настоящая сигнатура.
int __fastcall DetourOnNewRound(void* self, void* args);
int __fastcall DetourOnTick(void* self, void* args);
int __fastcall DetourOnDamage(void* self, void* edx, void* arg1);
int __fastcall DetourChatMessage(void* self, void* edx, void* arg1,
                                 void* arg2);

// Счётчики вызовов — доказательство, что перехват сработал.
struct HookCounters {
    unsigned long onNewRound = 0;
    unsigned long tick = 0;
    unsigned long onDamage = 0;
    unsigned long chatMessage = 0;
};

const HookCounters& Counters();
void ResetCounters();

// Трамплин первого хука — для проверки, что оригинальный код сохранён.
void* TestFirstTrampoline();

/// Сколько байт и сколько инструкций украдено первым перехватом.
struct StolenInfo {
    size_t size;
    size_t instructions;
};
StolenInfo TestFirstStolen();

// Номер версии вшитой Lua (0, если Lua ещё не подключена).
int LuaVersionNumber();

namespace EngineHooks {

// Сколько целей в проверочном наборе. Это НЕ число хуков VEXT.
inline constexpr int kHookCount = 4;

// Резолвит цели перехвата. Возвращает false, если движка в процессе нет
// (тестовый хост) — тогда цели берутся из Overrides.
bool ResolveNatives(EngineNatives* out);

// Ставит перехваты. Возвращает, сколько удалось.
int InstallAll(const EngineNatives& natives);

// Цели для проверки механизма. Тест задаёт свои адреса.
struct Overrides {
    void* onNewRound = nullptr;
    void* tick = nullptr;
    void* onDamage = nullptr;
    void* chatMessage = nullptr;

    bool any() const {
        return onNewRound || tick || onDamage || chatMessage;
    }
};

void SetOverrides(const Overrides& o);
const Overrides& GetOverrides();

// Результат последней установки по индексу 0..3.
HookResult LastResult(int index);
void ClearLastResults();

/// Трамплин перехвата по индексу 0..3 — адрес, с которого выполняется
/// украденная часть оригинала. nullptr, если хук не установлен.
///
/// Нужен обходникам, чтобы вызвать исходный код. Без этого перехват
/// «съедает» функцию: она не доходит до тела и возвращает мусор.
void* TrampolineOf(int index);

}  // namespace EngineHooks

}  // namespace evbots