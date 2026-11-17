// VextSql.h — серверная библиотека VeniceEXT SQL для EVBots.
//
// Регистрирует глобальную таблицу SQL (server) со всеми методами — строго
// по документации VU (НЕ по памяти):
//   content\vext\ref\server\library\sql.md
// Под капотом — настоящий SQLite 3.54.0 (amalgamation, статически в DLL).
// Подробности — в VextSql.cpp.
#pragma once

struct lua_State;

namespace evbots {

/// Зарегистрировать серверные SQL в состоянии Lua.
/// Возвращает false, если регистрация не удалась.
bool RegisterVextSql(lua_State* L);

}  // namespace evbots
