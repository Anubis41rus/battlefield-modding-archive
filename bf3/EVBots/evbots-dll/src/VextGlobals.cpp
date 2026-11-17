// VextGlobals.cpp — VeniceEXT Globals (заглушка).
//
// Глобальная таблица Globals, используемая fun-bots для обмена данными
// между OnLevelLoaded, SetMaxBotsPerTeam и другими обработчиками.
// Все поля по умолчанию nil или false — реальные значения устанавливаются
// в runtime (OnLevelLoaded, SetMaxBotsPerTeam и т.д.).
//
// Поля соответствуют использованию в fun-bots __init__.lua:
//   Globals.GameMode, Globals.LevelName, Globals.Round
//   Globals.MaxBotsPerTeam, Globals.UsedSpawnMethod, Globals.RespawnDelay
//   Globals.IsInputAllowed, Globals.IsInputRestrictionDisabled
//   Globals.MapHasDynamicJetSpawns
//   Globals.IsTdm, Globals.IsSdm, Globals.IsGm, Globals.IsAirSuperiority

#include "VextGlobals.h"

#include <string>

#include "EVBotsLog.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace evbots {
namespace {

constexpr char kGlobalsSource[] = R"LUA(
local log = ...

-- ═══════════════════════════════════════════════════════════════════════════
-- Globals — глобальная таблица, заполняемая движком и модом fun-bots
-- ═══════════════════════════════════════════════════════════════════════════
_G.Globals = {
    -- Строки, устанавливаются в OnLevelLoaded
    GameMode = nil,
    LevelName = nil,
    Round = nil,

    -- Числа
    MaxBotsPerTeam = nil,
    UsedSpawnMethod = nil,
    RespawnDelay = nil,

    -- Булевы (все false по умолчанию)
    IsInputAllowed = false,
    IsInputRestrictionDisabled = false,
    MapHasDynamicJetSpawns = false,

    -- Режимы игры (булевы)
    IsTdm = false,
    IsSdm = false,
    IsGm = false,
    IsAirSuperiority = false,
}

return _G.Globals
)LUA";

bool RegisterVextGlobalsImpl(lua_State* L) {
    lua_pushcfunction(L, [](lua_State* L_) -> int {
        const char* text = luaL_optstring(L_, 2, "");
        LogPrintf("EVBots: Globals: %s", text);
        return 0;
    });
    const int kSourceLen = static_cast<int>(sizeof(kGlobalsSource) - 1);
    if (luaL_loadbufferx(L, kGlobalsSource, kSourceLen,
                         "=[vextglobals]", "t") != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: Globals не собрался: %s",
                err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }
    lua_pushvalue(L, -2);  // копируем log-функцию как аргумент
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: Globals не выполнился: %s",
                err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }
    if (!lua_istable(L, -1)) {
        LOG_ERR("EVBots: Globals вернул не таблицу");
        lua_pop(L, 1);
        return false;
    }
    lua_setglobal(L, "Globals");
    LOG_INFO("EVBots: Globals подключен (заглушка)");
    return true;
}

}  // namespace

bool RegisterVextGlobals(lua_State* L) {
    return RegisterVextGlobalsImpl(L);
}
}  // namespace evbots