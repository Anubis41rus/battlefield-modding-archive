// VextHooks.h — библиотека VeniceEXT Hooks для EVBots.
//
// Регистрирует глобальную таблицу Hooks (shared) и типы Hook, HookContext —
// строго по документации VU (НЕ по памяти):
//   content\vext\ref\shared\library\hooks.md
//   content\vext\ref\shared\type\hook.md
//   content\vext\ref\shared\type\hookcontext.md
// Подробности — в VextHooks.cpp.
#pragma once

struct lua_State;

namespace evbots {

/// Зарегистрировать Hooks в состоянии Lua.
bool RegisterVextHooks(lua_State* L);

}  // namespace evbots
