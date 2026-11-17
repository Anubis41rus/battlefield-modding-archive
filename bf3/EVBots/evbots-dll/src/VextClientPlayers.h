// VextClientPlayers.h — игроки клиента из движка bf3.exe (К5 шаг 4).
#pragma once

struct lua_State;

namespace evbots {

/// __clp_local, __clp_ids, __clp_info, __clp_transform — для PlayerManager клиента (VextClient.cpp). Только клиент.
bool RegisterClientPlayers(lua_State* L);

}  // namespace evbots
