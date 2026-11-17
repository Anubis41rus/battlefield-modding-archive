// VextResourceManager.h — библиотека VeniceEXT ResourceManager для EVBots.
//
// Регистрирует глобальную таблицу ResourceManager (shared) с методами по
// документации VU (НЕ по памяти):
//   content\vext\ref\shared\library\resourcemanager.md,
//   content\vext\ref\shared\type\containercallback.md.
// Подробности — в VextResourceManager.cpp.
#pragma once

struct lua_State;

namespace evbots {

/// Зарегистрировать ResourceManager в состоянии Lua.
bool RegisterVextResourceManager(lua_State* L);

/// ★ Сборка 161: load-хендлеры VU. Start — после загрузки модов (ставит перехват
/// onBundlePartition и вызывает хендлеры уже загруженных инстансов первого уровня);
/// NewRound — на новом раунде (догоняющий вызов для того, что перехват не поймал);
/// DrainDeferred — разделы, чьи хендлеры не успели взять лок Lua в потоке загрузки.
void ResourceLoadHandlersStart();
// Real server frame only: startup/reload once settings and native TLS exist.
bool ResourceEnsureNativeStartup();
void ResourceLoadHandlersNewRound();
void ResourceDrainDeferred();

/// ★ Сборка 234 (клиент, реестр В8): слой ресурсов движка по адресам zbf3, событие Partition:Loaded (очередь из
/// перехвата загрузки) и load-хендлеры. Start — после загрузки модов клиента; Drain — раз в кадр движка (под TickGate).
void ResourceClientStart();
void ResourcePartitionEventsDrain();
/// Synchronous loading boundary: false if the bounded queue could not be fully
/// delivered or any Partition:Loaded was lost since Level:LoadResources.
bool ResourceFlushPartitionEvents();

/// Retain while the caller already owns a live native pointer (e.g. a physics
/// callback). Release is queued; never invokes a native allocator on that thread.
bool ResourceRetainLive(unsigned long object);
void ResourceQueueRelease(unsigned long object);
/// Transfers one retained reference to Lua, including on push failure (nil).
void ResourcePushOwnedLive(lua_State* L, unsigned long object);
// Genuine native value userdata only; never an arbitrary table/typeInfo name.
const char* ResourceGeometryType(lua_State* L, int index);

/// Ассет по имени через базу данных движка (0 — нет или слой выключен).
unsigned long LiveFindAssetByName(const char* name);
/// ★ Сборка 168: база ресурсов движка готова (прологи сверены, слой не выключен).
bool LiveEngineReady();

/// ★ Сборка 163: GetSettings(«SyncedGameSettings») через SettingsManager совпал с глобалом движка, а поле
/// AllowClientSideDamageArbitration — по ожидаемому смещению. Условие для хука VU BulletEntity:Collision.
bool LiveSyncedSettingsCheck(unsigned long expectObj, unsigned expectOffset);

}  // namespace evbots
