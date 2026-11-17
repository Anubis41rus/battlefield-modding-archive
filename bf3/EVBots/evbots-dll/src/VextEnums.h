// VextEnums.h — глобальные таблицы-перечисления VeniceEXT (VU) для EVBots.
//
// Регистрирует глобальные Lua-таблицы вида InputDeviceKeys, TeamId, Realm и т.п.,
// которые VU-моды используют как перечисления (напр. InputDeviceKeys.IDK_LeftAlt).
// Данные взяты из документации VU, см. VextEnums.cpp (список таблиц там же).
#pragma once

struct lua_State;

namespace evbots {

/// Зарегистрировать все таблицы-перечисления VU в состоянии Lua.
/// Вызывать после luaL_openlibs, до выполнения __init__.lua модов.
/// Возвращает false, если хоть один кусок Lua не собрался или не выполнился.
bool RegisterVextEnums(lua_State* L);

}  // namespace evbots
