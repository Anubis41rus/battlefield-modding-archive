// VextClient.h — клиентские библиотеки VeniceEXT для клиентского режима EVBots.dll (в bf3.exe).
//
// NetEvents (client), PlayerManager (client), InputManager, ClientUtils, RaycastManager (client),
// DebugRenderer, WebUI — строго по VU-Docs content\vext\ref\client\library\*.md (сигнатуры и типы возврата);
// ResourceManager, EntityManager — по shared\library (у VU общие, у нас клиентские до К5).
// К3 (23.10.2026): каркас; движковых вызовов пока нет — подробности в VextClient.cpp.
#pragma once

struct lua_State;

namespace evbots {

/// Зарегистрировать клиентские библиотеки VU в состоянии Lua (только для LuaContext::Client).
/// Возвращает false, если Lua-чанк не собрался или не выполнился.
bool RegisterVextClient(lua_State* L);

}  // namespace evbots
