// EVBots.TestApi.cpp — тестовые точки входа в DLL.
//
// Нужны тест-хосту: без них он не может задать цели хуков и прочитать
// счётчики. В боевой работе они не используются и не влияют на мод-систему.

#include "LuaRuntime.h"
#include "EngineHooks.h"
#include "EVBotsLog.h"
#include "ModdingInterface.h"

#include <mutex>
#include <string>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

extern "C" {

/// Задать цели хуков вместо адресов движка BF3.
/// Структура из 4 указателей на функции — чтобы тест мог подсунуть свои.
__declspec(dllexport) void __cdecl EVBotsTestSetHooks(const void* targets) {
    if (targets == nullptr) {
        evbots::EngineHooks::Overrides empty{};
        evbots::EngineHooks::SetOverrides(empty);
        return;
    }

    struct Four {
        void* a;
        void* b;
        void* c;
        void* d;
    };

    Four f;
    std::memcpy(&f, targets, sizeof(f));

    evbots::EngineHooks::Overrides o{};
    o.onNewRound = f.a;
    o.tick = f.b;
    o.onDamage = f.c;
    o.chatMessage = f.d;
    evbots::EngineHooks::SetOverrides(o);
}

/// Результат установки хука по индексу 0..3.
/// 0 = установлен, 1 = уже перехвачен, 2 = неверный адрес, 3 = мало байт.
__declspec(dllexport) int __cdecl EVBotsTestLastHookResult(int index) {
    return static_cast<int>(evbots::EngineHooks::LastResult(index));
}

/// Указатель на массив из 4 счётчиков:
/// onNewRound, tick, onDamage, chatMessage.
__declspec(dllexport) unsigned long* __cdecl EVBotsTestCounters() {
    static unsigned long counters[4];
    const auto& c = evbots::Counters();
    counters[0] = c.onNewRound;
    counters[1] = c.tick;
    counters[2] = c.onDamage;
    counters[3] = c.chatMessage;
    return counters;
}

/// Трамплин первого хука (onNewRound) — по нему видно, что оригинальный
/// код сохранён и доступен.
__declspec(dllexport) void* __cdecl EVBotsTestTrampoline() {
    // g_hookOnNewRound живёт в EngineHooks.cpp; здесь берём его адрес
    // через публичную обёртку.
    return evbots::TestFirstTrampoline();
}

/// Адрес нашего обходника первого хука.
///
/// Нужен самопроверке, чтобы сверить: патч обязан прыгать РОВНО сюда.
/// Если числа расходятся — ошибка в расчёте относительного смещения.
__declspec(dllexport) void* __cdecl EVBotsTestDetour() {
    return (void*)(ULONGLONG)&evbots::DetourOnNewRound;
}

/// Состояние мод-системы: [тиков, раундов, уровень загружен].
///
/// Через vtable этого не прочитать: EngineState объявлена внутри DLL.
/// Экспорт даёт самопроверке проверить, что вызовы методов через vtable
/// действительно доходят до реализации, а не в мусор.
__declspec(dllexport) void __cdecl EVBotsTestState(unsigned long* out) {
    if (out == nullptr) {
        return;
    }

    evbots::EngineState state{};
    evbots::GetInstance()->QueryState(&state);

    out[0] = state.tickCount;
    out[1] = state.roundCount;
    out[2] = state.levelLoaded ? 1UL : 0UL;
}

/// Число байт, украденных первым перехватом, и число инструкций.
__declspec(dllexport) unsigned long __cdecl EVBotsTestStolenBytes() {
    return static_cast<unsigned long>(evbots::TestFirstStolen().size);
}

/// Сбросить счётчики вызовов.
__declspec(dllexport) void __cdecl EVBotsTestResetCounters() {
    evbots::ResetCounters();
}

/// Версия Lua, вшитая в DLL (0, если Lua ещё не подключена).
__declspec(dllexport) int __cdecl EVBotsTestLuaVersion() {
    return evbots::LuaVersionNumber();
}

/// Эмитировать события загрузки уровня для модов (у тест-хоста движка нет).
/// После Initialize рассылает Level:Loaded и NodeCollection:FinishedLoading,
/// чтобы fun-bots прошёл путь до спавна ботов. Не part боевой поверхности —
/// только для теста/отладки.
__declspec(dllexport) void __cdecl EVBotsTestSimulateLevel(const char* level,
                                                           const char* mode,
                                                           int round,
                                                           int roundsPerMap) {
    auto& lua = evbots::LuaRuntime::Instance();
    if (!lua.ready()) {
        return;
    }
    evbots::LuaRuntime::EngineEventArg levelName =
        evbots::LuaRuntime::EngineEventArg::Str(level != nullptr ? level : "Levels/MP_001/MP_001");
    evbots::LuaRuntime::EngineEventArg gameMode =
        evbots::LuaRuntime::EngineEventArg::Str(mode != nullptr ? mode : "ConquestLarge0");
    evbots::LuaRuntime::EngineEventArg r =
        evbots::LuaRuntime::EngineEventArg::Num(round);
    evbots::LuaRuntime::EngineEventArg rpm =
        evbots::LuaRuntime::EngineEventArg::Num(roundsPerMap);
    lua.DispatchEngineEvent(
        "Level:Loaded",
        { levelName, gameMode, r, rpm });
    lua.DispatchEngineEvent("NodeCollection:FinishedLoading");
}

/// Просимулировать тики движка для модов: рассылает Engine:Update N раз.
/// fun-bots спавнит ботов в OnEngineUpdate после FIRST_SPAWN_DELAY (5.0),
/// поэтому цикл должен покрыть этот интервал (например 200 тиков по 0.03).
/// Только для теста/отладки, не боевая поверхность.
__declspec(dllexport) void __cdecl EVBotsTestSimulateTicks(int ticks,
                                                           double delta) {
    auto& lua = evbots::LuaRuntime::Instance();
    if (!lua.ready() || ticks < 0) {
        return;
    }
    evbots::LuaRuntime::EngineEventArg dt =
        evbots::LuaRuntime::EngineEventArg::Num(delta);
    evbots::LuaRuntime::EngineEventArg simDt =
        evbots::LuaRuntime::EngineEventArg::Num(delta);
    for (int i = 0; i < ticks; ++i) {
        lua.DispatchEngineEvent("Engine:Update", { dt, simDt });
    }
}

/// ★ Сборка 215 (К3): клиентский режим в тест-хосте. Перед вызовом серверный рантайм должен быть остановлен
/// (Shutdown через vtable). modsRoot — папка Admin\Mods (nullptr или пусто — без модов). 1 — Lua поднялся.
__declspec(dllexport) int __cdecl EVBotsTestClientMode(const wchar_t* modsRoot) {
    return evbots::StartClientMode(modsRoot != nullptr ? std::wstring(modsRoot) : std::wstring()) ? 1 : 0;
}

/// Остановить клиентский режим (Extension:Unloading, закрыть Lua).
__declspec(dllexport) void __cdecl EVBotsTestStopClientMode() {
    evbots::StopClientMode();
}

/// ★ Сборка 228: «кадр клиента» без движка (только при EVBOTS_WEBUI_TEST=1): WebUI, ввод, Client:UpdateInput.
__declspec(dllexport) void __cdecl EVBotsTestClientFrame(float dt) {
    evbots::ClientFrameForTest(dt);
}

/// Выполнить в текущем Lua «return <expr>»: 1 — истина, 0 — ложь/nil, -1 — ошибка (текст в лог), -2 — Lua не поднят.
__declspec(dllexport) int __cdecl EVBotsTestLuaTrue(const char* expr) {
    auto& lua = evbots::LuaRuntime::Instance();
    if (!lua.ready() || lua.state() == nullptr || expr == nullptr) return -2;
    std::lock_guard<std::recursive_mutex> gate(lua.TickGate());
    lua_State* L = lua.state();
    const std::string code = std::string("return ") + expr;
    if (luaL_loadbufferx(L, code.data(), code.size(), "=[test]", "t") != LUA_OK || lua_pcall(L, 0, 1, 0) != LUA_OK) {
        const char* m = lua_tostring(L, -1);
        LOG_ERR("EVBots: тест: Lua «%s»: %s", expr, m != nullptr ? m : "?");
        lua_pop(L, 1);
        return -1;
    }
    const int r = lua_toboolean(L, -1) ? 1 : 0;
    lua_pop(L, 1);
    return r;
}

/// Добавить тестового «живого» игрока в реестр PlayerManager (для теста).
__declspec(dllexport) void __cdecl EVBotsTestAddHumanPlayer(const char* name,
                                                            int team,
                                                            int squad) {
    auto& lua = evbots::LuaRuntime::Instance();
    if (!lua.ready()) {
        return;
    }
    lua.AddTestHumanPlayer(name != nullptr ? name : "TestPlayer", team, squad);
}

}  // extern "C"