// LuaDebugConsole.h — глобалы VeniceEXT: Console и перехват print.
//
// Реализовано строго по документации VU:
//   - Console — ref\client\library\console.md и тип ConsoleCommand
//     (ref\client\type\consolecommand.md). Библиотека Console есть только
//     в клиентском API VU, серверного/общего Console нет;
//   - Debug — глобалом VU НЕ является: это собственная таблица мода
//     fun-bots (ext\Shared\Debug.lua), которую мод грузит сам через
//     require('__shared/Debug'). Поэтому здесь под Debug ничего не
//     добавляем — иначе выдуманный API замаскирует реальную проблему;
//   - print модов направляем в лог EVBots (LOG_INFO) — без игровой
//     консоли это единственный способ видеть сообщения модов.

#pragma once

struct lua_State;

namespace evbots {

/// Зарегистрировать глобал Console (методы по документации VU) и print.
/// Возвращает false, если встроенный Lua-модуль не собрался/не выполнился.
/// Вызывать после создания состояния Lua (LuaRuntime::Initialize).
bool RegisterDebugConsole(lua_State* L);

}  // namespace evbots
