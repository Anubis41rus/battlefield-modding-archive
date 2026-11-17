// VextNetCodec.h — упаковка NetEvents для канала клиент↔сервер (К4).
#pragma once

struct lua_State;

namespace evbots {

/// Регистрирует глобальную таблицу EVNetCodec (EncodeHello, EncodeEvent, Decode). Обе стороны.
bool RegisterNetCodec(lua_State* L);

}  // namespace evbots
