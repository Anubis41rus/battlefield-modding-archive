// VextPlayerManager.h — библиотека VeniceEXT PlayerManager для EVBots.
//
// Регистрирует глобальную таблицу PlayerManager (server) и типы Player,
// SoldierEntity, EntryInput — строго по документации VU (НЕ по памяти):
//   content\vext\ref\server\library\playermanager.md
//   content\vext\ref\server\type\player.md
//   content\vext\ref\shared\type\entryinput.md
// Подробности — в VextPlayerManager.cpp.
#pragma once

#include <cstddef>

struct lua_State;

namespace evbots {

/// Зарегистрировать серверный PlayerManager в состоянии Lua.
bool RegisterVextPlayerManager(lua_State* L);

/// ★ Сборка 160: сбросить кэши ассетов скана (внешность/перки/промахи/hint) — вызывается
/// при смене уровня (ModdingInterface::OnNewRound): адреса объектов прошлого уровня не те.
void ResetAssetCaches();

/// ★ Сборка 185: имя уровня («Levels/MP_012/MP_012») и режим («TeamDeathMatch0») для Level:Loaded — из движка,
/// как у VU; флаги EVBOTS_LEVEL_NAME/GAMEMODE — явная подмена; не прочитано — Levels/MP_001/MP_001, ConquestSmall0.
void EngineLevelNameAndMode(char* name, size_t nameCap, char* mode, size_t modeCap);
/// Имя и GameMode уровня прочитаны; без режима не разрешаем спавн и Level:Loaded.
bool EngineLevelReady();
/// Сборка 185: число сущностей уровня в движке (точки спавна + флаги) — Level:Loaded ждёт, пока оно > 0 и не меняется.
int EVBotsEngineLevelEntityCount();
/// Сборка 186: перехват RCON onMessage заранее (до загрузки уровня) и счётчик сигналов движка server.onLevelLoaded.
void EVBotsRconHookEarly();
long EVBotsLevelLoadedSeq();
void EVBotsSetStartupLevelSeq(long seq);  // сборка 211: номер сигнала, по которому ушёл стартовый Level:Loaded
bool EVBotsRconHookOn();

}  // namespace evbots
