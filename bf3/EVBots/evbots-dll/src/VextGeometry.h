// VextGeometry.h — типы-конструкторы VeniceEXT (VU) для EVBots.
//
// Регистрирует глобальные типы Vec2, Vec3, Vec4, LinearTransform,
// QuatTransform, Quat и Guid со свойствами, методами и операторами —
// строго по документации VU (НЕ по памяти):
//   content\vext\ref\shared\type\{vec2,vec3,vec4,lineartransform,
//   quat,quattransform,guid}.md
// (подробности — в VextGeometry.cpp).
#pragma once

struct lua_State;

namespace evbots {

/// Зарегистрировать все типы-конструкторы VU в состоянии Lua.
/// Вызывать после luaL_openlibs, до выполнения __init__.lua модов.
/// Возвращает false, если хоть один Lua-чанк не собрался или не выполнился.
bool RegisterVextGeometry(lua_State* L);

}  // namespace evbots
