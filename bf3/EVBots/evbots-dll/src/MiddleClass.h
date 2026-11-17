// MiddleClass.h — встроенная система классов VeniceEXT (middleclass, MIT).

#pragma once

struct lua_State;

namespace evbots {

/// Загрузить middleclass и объявить глобальный class('Name').
/// Возвращает false, если исходник не собрался (тогда class не будет).
bool RegisterMiddleClass(lua_State* L);

}  // namespace evbots
