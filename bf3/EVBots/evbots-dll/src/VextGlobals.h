// VextGlobals.h — библиотека VeniceEXT Globals для EVBots.
#pragma once

struct lua_State;

namespace evbots {

/// Зарегистрировать глобальную таблицу Globals в состоянии Lua.
bool RegisterVextGlobals(lua_State* L);

}  // namespace evbots