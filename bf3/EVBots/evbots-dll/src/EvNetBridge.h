// EvNetBridge.h — NetEvents через канал ev-core.dll (К4).
#pragma once

struct lua_State;

namespace evbots {

/// Lua-функции канала: __evnet_send, __evnet_broadcast, __evnet_available, __evnet_playername. Обе стороны.
bool RegisterEvNetBridge(lua_State* L);

/// Подключить обработчик ev-core; клиент — ещё и начать подключение к серверу. false — ev-core.dll нет.
bool EvNetBridgeStart(bool client);

/// Отдать принятое из сети в Lua (NetEvents._OnNet) и дослать отложенное (NetEvents._Pump). Звать на тике движка.
void EvNetDrain();

}  // namespace evbots
