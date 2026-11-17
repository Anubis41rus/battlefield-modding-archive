// VextRcon.h — серверная библиотека VeniceEXT RCON для EVBots.
//
// Регистрирует глобальную таблицу RCON (server) со всеми методами — строго
// по документации VU (НЕ по памяти):
//   content\vext\ref\server\library\rcon.md
// Подробности — в VextRcon.cpp.
#pragma once

#include <string>
#include <vector>

struct lua_State;

namespace evbots {

/// Зарегистрировать серверные RCON в состоянии Lua.
bool RegisterVextRcon(lua_State* L);

/// Сборка 198: команда RCON сервера из C++ (слова → слова ответа). false — RCON недоступен.
bool EVBotsRconCommand(const std::vector<std::string>& words, std::vector<std::string>& out, int timeoutMs);

/// ★ Сборка 209: команды модов (RCON:RegisterCommand) через RCON сервера, как VU. Перехват диспетчера входящих команд
/// 0x00DB7F40; ответ — EVBotsRconDrainReplies() из хука тика движка (главный поток).
void EVBotsRconInstallDispatchHookOnce();
void EVBotsRconDrainReplies();

}  // namespace evbots
