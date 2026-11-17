// VextFbTypes.h — классы fb VU как глобальные типы клиентского Lua (сборка 215). Реализация сгенерирована
// tools\gen_fb_types.py в VextFbTypes.cpp.
#pragma once

struct lua_State;

namespace evbots {

/// Зарегистрировать все классы fb из VU-Docs, которых ещё нет в _G (Foo(), Foo(guid), Foo(other)).
/// Возвращает число добавленных, -1 — ошибка (в лог).
int RegisterVextFbTypes(lua_State* L);

}  // namespace evbots
