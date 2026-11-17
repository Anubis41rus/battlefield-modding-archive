// VextTeamSquadManager.cpp — серверная библиотека VeniceEXT TeamSquadManager
// для EVBots.
//
// Строго по документации VU (НЕ по памяти):
//   content\vext\ref\server\library\teamsquadmanager.md
//
// Методы:
//   GetSquadPlayerCount(team: TeamId, squad: SquadId) -> int
//   GetTeamPlayerCount(team: TeamId) -> int
//
// С 14.10 (сборка 78) счётчики настоящие: отряд и команду каждого игрока читает
// мост из движка (VextPlayerManager.cpp, EVBotsCountSquadPlayers). Без движка
// (тест-хост) — 0, как раньше.

#include "VextTeamSquadManager.h"

#include <string>

#include "EVBotsLog.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace evbots {
int EVBotsCountSquadHumans(unsigned int team, unsigned int squad);
int EVBotsCountTeamHumans(unsigned int team);
namespace {

constexpr char kTeamSquadManagerSource[] = R"LUA(
local get_squad_humans, get_team_humans = ...

-- TeamSquadManager — число игроков в отряде/команде.
-- Боты — по Player.squadId/teamId из PlayerManager (выбор мода действует сразу, как в VU),
-- люди (их нет в списке PlayerManager) — из движка.
local TeamSquadManager = {}

local function countBots(team, squad)
    local n = 0
    if PlayerManager == nil then return 0 end
    local list = PlayerManager:GetPlayers()
    for i = 1, #list do
        local p = list[i]
        if p.onlineId == 0 and p.teamId == team and (squad == nil or p.squadId == squad) then
            n = n + 1
        end
    end
    return n
end

function TeamSquadManager:GetSquadPlayerCount(team, squad)
    return countBots(team, squad) + get_squad_humans(team, squad)
end

function TeamSquadManager:GetTeamPlayerCount(team)
    return countBots(team, nil) + get_team_humans(team)
end

_G.TeamSquadManager = TeamSquadManager
return TeamSquadManager
)LUA";

bool RegisterVextTeamSquadManagerImpl(lua_State* L) {
    const int kSourceLen = static_cast<int>(sizeof(kTeamSquadManagerSource) - 1);
    if (luaL_loadbufferx(L, kTeamSquadManagerSource, kSourceLen,
                         "=[vextteamsquadmanager]", "t") != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: TeamSquadManager не собрался: %s",
                err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }
    // Стек: СНАЧАЛА функция (chunk), ПОТОМ аргументы — иначе pcall вызвал бы
    // cfunction-аргумент вместо чанка (и «вернул не таблицу»).
    lua_pushcfunction(
        L, [](lua_State* L_) -> int {
            const lua_Integer team = luaL_optinteger(L_, 1, 0);
            const lua_Integer squad = luaL_optinteger(L_, 2, 0);
            lua_pushinteger(L_, (lua_Integer)EVBotsCountSquadHumans(
                                    (unsigned int)team, (unsigned int)squad));
            return 1;
        });
    lua_pushcfunction(
        L, [](lua_State* L_) -> int {
            const lua_Integer team = luaL_optinteger(L_, 1, 0);
            lua_pushinteger(L_, (lua_Integer)EVBotsCountTeamHumans((unsigned int)team));
            return 1;
        });
    if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: TeamSquadManager не выполнился: %s",
                err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }
    if (!lua_istable(L, -1)) {
        LOG_ERR("EVBots: TeamSquadManager вернул не таблицу");
        lua_pop(L, 1);
        return false;
    }
    LOG_INFO("EVBots: TeamSquadManager подключен (server)");
    lua_setglobal(L, "TeamSquadManager");
    return true;
}

}  // namespace

bool RegisterVextTeamSquadManager(lua_State* L) {
    return RegisterVextTeamSquadManagerImpl(L);
}
}  // namespace evbots
