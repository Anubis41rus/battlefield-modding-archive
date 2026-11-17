// VextConfig.h — библиотека fun-bots Config, SpawnMethod, TeamSwitchModes для EVBots.
#pragma once

struct lua_State;

namespace evbots {

/// Зарегистрировать глобальные таблицы Config, SpawnMethod, TeamSwitchModes.
bool RegisterVextConfig(lua_State* L);

}  // namespace evbots
