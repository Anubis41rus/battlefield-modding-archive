// VextEntityManager.h — библиотека VeniceEXT EntityManager для EVBots.
#pragma once

struct lua_State;

namespace evbots {

/// Зарегистрировать EntityManager в состоянии Lua.
bool RegisterVextEntityManager(lua_State* L);

// Accept only this module's verified server factory detour and intact trampoline.
bool EVBotsIsOwnEntityFactoryHook();

/// Пехотный спавнер HQ своей команды по живым сущностям уровня (0 — ещё не найден).
unsigned long EVBotsFindTeamBaseSpawn(unsigned int team);

}  // namespace evbots
