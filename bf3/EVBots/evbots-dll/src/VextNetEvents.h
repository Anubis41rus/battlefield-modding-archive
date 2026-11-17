// VextNetEvents.h — серверная библиотека VeniceEXT NetEvents для EVBots.
//
// Регистрирует глобальную таблицу NetEvents (server) со всеми методами —
// строго по документации VU (НЕ по памяти):
//   content\vext\ref\server\library\netevents.md
// Подробности — в VextNetEvents.cpp.
#pragma once

struct lua_State;

namespace evbots {

/// Зарегистрировать серверные NetEvents в состоянии Lua.
/// Возвращает false, если Lua-чанк не собрался или не выполнился.
bool RegisterVextNetEvents(lua_State* L);

}  // namespace evbots
