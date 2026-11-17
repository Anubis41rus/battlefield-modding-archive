// LuaEvents.h — библиотека Events VeniceEXT: подписки на события и рассылка.
//
// Реализовано строго по документации VU, без добавлений:
//   content\vext\ref\shared\library\events.md — методы библиотеки;
//   content\vext\ref\shared\type\event.md    — объект Event и его Unsubscribe;
//   content\vext\guides\events.md            — подписки, user data,
//                                               Dispatch/DispatchLocal,
//                                               регистрозависимость имён.
//
// Методы (все вызываются с двоеточием, как в документации):
//   Events:Subscribe(eventName, callback)          -> Event
//   Events:Subscribe(eventName, context, callback) -> Event
//   event:Unsubscribe()
//   Events:Dispatch(eventName, ...)
//   Events:DispatchLocal(eventName, ...)
//   Events:Unsubscribe(eventName)
//   Events:Unsubscribe()

#pragma once

struct lua_State;

namespace evbots {

/// Зарегистрировать глобальную таблицу Events в переданном состоянии Lua.
/// Возвращает false, если встроенный исходник не собрался или не выполнился
/// (тогда Events в модах не появится).
bool RegisterLuaEvents(lua_State* L);

}  // namespace evbots