// VextTeamSquadManager.h — серверная библиотека VeniceEXT TeamSquadManager.

// Подробности — в VextTeamSquadManager.cpp.
#pragma once

struct lua_State;

namespace evbots {

/// Зарегистрировать серверный TeamSquadManager в состояние Lua.
/// Возвращает false, если регистрация не удалась.
bool RegisterVextTeamSquadManager(lua_State* L);

}  // namespace evbots
