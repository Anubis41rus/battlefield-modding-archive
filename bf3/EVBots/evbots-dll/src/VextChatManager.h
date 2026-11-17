// VextChatManager.h — серверная библиотека VeniceEXT ChatManager.

// Подробности — в VextChatManager.cpp.
#pragma once

struct lua_State;

namespace evbots {

/// Зарегистрировать серверный ChatManager в состояние Lua.
/// Возвращает false, если регистрация не удалась.
bool RegisterVextChatManager(lua_State* L);

}  // namespace evbots
