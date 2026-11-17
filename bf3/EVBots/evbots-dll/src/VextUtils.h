// VextUtils.h — глобальные библиотеки VeniceEXT (VU) для EVBots.
//
// Регистрирует две shared-библиотеки VU: MathUtils и SharedUtils.
// Методы и сигнатуры — из документации VU:
//   content\vext\ref\shared\library\mathutils.md
//   content\vext\ref\shared\library\sharedutils.md
// (см. подробности в VextUtils.cpp).
#pragma once

struct lua_State;

namespace evbots {

/// Зарегистрировать глобальные таблицы MathUtils и SharedUtils.
/// serverContext — флаг окружения (1 = сервер, 0 = клиент), от него зависят
/// SharedUtils:IsServerModule/IsClientModule. Вызывать после luaL_openlibs,
/// до выполнения __init__.lua модов. Возвращает false, если Lua-кусок
/// не собрался или не выполнился.
bool RegisterVextUtils(lua_State* L, int serverContext);

}  // namespace evbots
