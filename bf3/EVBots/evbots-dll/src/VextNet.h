// VextNet.h — библиотека VeniceEXT Net для EVBots.
//
// Регистрирует глобальную таблицу Net со всеми методами — строго по
// документации VU (НЕ по памяти):
//   content\vext\ref\shared\library\net.md
// Подробности — в VextNet.cpp.
#pragma once

struct lua_State;

namespace evbots {

/// Зарегистрировать Net в состоянии Lua.
bool RegisterVextNet(lua_State* L);

}  // namespace evbots
