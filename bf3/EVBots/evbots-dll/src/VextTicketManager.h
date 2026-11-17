// VextTicketManager.h — библиотека VeniceEXT TicketManager для EVBots.
//
// Регистрирует глобальную таблицу TicketManager (server):
// GetTicketCount(team) — тикеты движка (ServerTicketCounterEntity +0xA0),
// GetCurrentRound, GetRoundCount — пока заглушки (0).
#pragma once

struct lua_State;

namespace evbots {

/// Зарегистрировать TicketManager в состоянии Lua.
bool RegisterVextTicketManager(lua_State* L);

}  // namespace evbots