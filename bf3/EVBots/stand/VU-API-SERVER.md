# VEXT API — серверная часть (Legacy)

> Источник правды: `Legacy\VU-Docs\content\vext\ref\server\*` (refs v1.0). Клиентская часть — отдельный tree `vext\ref\client\` (для фаз 3–4). `shared` — общий для client+server (список в §5).
> Схема модов: `Admin/Mods\<name>/ext/{client,server,shared}/__init__.lua` (see `Legacy\VU-Docs\content\modding\structure.md`). `client` — только на клиенте, `server` — только на сервере, `shared` — оба.
> Готовые боты VU `BF-Portable\Battlefield_3_Server\Instance\Admin\Mods\fun-bots` (Joe91/fun-bots, с 16.10) вызывают API этого же набора. Сопоставление с конкретными Lua-вызовами — раздел 7 (заполняется в фазе 4).

## 1. Types (server) — 21 тип

| Тип | Статус для готовых ботов | Назначение / ключевые поля |
|---|---|---|
| Player | V | Игрок: `id, onlineId, guid, accountGuid, ip, ping, alive, isSquadLeader, teamId, squadId, score, kills, deaths`, `soldier`, `corpse`, `weapons[]`, `input`, `selectedKit`, `weapons[]`. Методы: `Kick/Ban/BanTemporarily/BanByIp/Spawn/SelectWeapon/SelectUnlockAssets/EnterVehicle/ExitVehicle`. |
| SoldierEntity | V | Игрок в игре: состояние, оружия `weapons[]`, контейнеры (customization), хиты. События: soldier:HealthAction/ManDown/PrePhysicsUpdate. |
| ControllableEntity | V | Управляемый объект (ТС, довод, физемобджект): `position, rotation, velocity, alive, health` и пр.; AI-контроль. Для ботов-водителей — ключевой тип. События vehicle:* для снарядов? no — Vehicle:* events are separate. |
| SpawnEntity | H | Объект-спавн: `locationNameSid, spawnCount, spawnDelay, teamId, enabled, canSpawnMore, IsAreaOccupiedByEnemies(position, team), spawnedControllables`. Безопасные зоны, зоны захвата, базы, боссы. |
| GameEntity | B | Суперкласс: `position, rotation, velocity, lifeTime, creationTime, entityGuid`, собственные жизни/принадлежность. |
| DamageInfo | H | Инфо урона: `position, direction, origin, damage, damagedMaterial, collidingMaterial, boneIndex, isBulletDamage, isExplosionDamage, isClientDamage, empTime, stamina, collisionSpeed`. |
| DamageGiverInfo | H | Кто нанёс урон: `entity`, `player?` (из событий повреждений и hook BulletEntity:Collision). |
| CollisionInfo | H | Результат столкновения: `entity, material, ownMaterial, position, normal, speed, part, isInstantlyDestructible`. |
| ExplosionEntity | R | Взрыв: позиция, очки навеса (редко используются в боте). |
| KillCounterEntity / LifeCounterEntity / TicketCounterEntity | R | Счётчики убийств/жизней/билетов (для квестов и рандомизированных режимов). |
| CapturePointEntity | R | Точка захвата: `teamId, isCaptured, captureCount` и пр. |
| NetEvent | R | Дескриптор подписки от `NetEvents:Subscribe` → `Unsubscribe()`. |
| SQLBlob | R | BLOB-данные (в SQL:Query). |
| ServerPlayerEvent / ServerDoublePlayerEvent | R | События от клиента (конструкторы с фильтрами sendToPlayerOnly/clientsOnly.../team). База для всех "Server:*"-событий. |
| SoldierWeapon | R | Оружие: `name, primaryAmmo, secondaryAmmo, weaponModifier, weaponFiring, fireLogic`. |
| SoldierWeaponsComponent | R | Компонент оружия солдата. |
| VoipChannel | R | Канал раскастки: `name, defaultEmitterType, players`, методы `AddPlayer/RemovePlayer/Close`. |

Статусы: V — встречается в боте, H — часто, R — редко/только как референс. ▲ В серверном API **нет отдельных типов Vehicle/Controller** — ТС = ControllableEntity + события vehicle:* (Damage/Destroyed/...).

## 2. Libraries (server) — 10

| Библиотека | Главные методы (используемые в боте) | Назначение |
|---|---|---|
| ChatManager | `SendMessage(msg[, team\|squad\|player])`, `Yell(msg, dur[, team\|squad\|player])` | Текст/голос в чат; `Yell` — до 255 символов. |
| NetEvents | `Subscribe(name, cb)`, `Broadcast(name, ...)`, `SendTo/SendToLocal`, `BroadcastLocal`, `SendUnreliable*` (To/Broadcast), `Unsubscribe()` | Сетевые события — основной транспорт для кастомики (turn-based, RP, управление ботом). |
| PlayerManager | `GetPlayers()`, `GetPlayerById/ByOnlineId/ByGuid/ByName`, `GetPlayersByTeam/Squad`, `GetSquadLeader`, `CreatePlayer`, `DeletePlayer`, `SetSpectatorEnabled` | Поиск игроков, управление спектаторами, динамическое создание игроков. |
| RCON | `SendCommand(cmd[, args])`, `RegisterCommand(name, flags, cb)`, `DeregisterCommand(handle)`, `GetServerGuid()`, `TriggerEvent(event[, args])` | Администрирование сервера, RCON-соединения, триггеры сторонним клиентам. |
| RaycastManager | `DetailedRaycast(from, to, maxHits, materialFlags, flags)`, `CollisionRaycast(...)` | Сетевые лучи: стрельба, обход, проверка проходимости. |
| SQL | `Open()/Close()`, `Query(sql, ...args)`, `Escape(text)`, `Blob(data)`, `Error()`, `AffectedRows()`, `LastInsertId()` | SQLite-ходмейкер для хранения данных бота. |
| ServerUtils | `SetCustomMapName/GetCustomMapName`, `SetCustomGameModeName/GetCustomGameModeName` | Отображение в серверном списке зума. |
| TeamSquadManager | `GetSquadPlayerCount(team, squad)`, `GetTeamPlayerCount(team)` | Счётчик слотов/игроков. |
| TicketManager | `SetTicketCount(team, n)`, `GetTicketCount(team)`, `GetCurrentRound()`, `GetRoundCount()` | Тайминг-механика раундов (DeathMatch). |
| Voip | `CreateChannel(name, VoipEmitterType)`, `GetChannels()` | Каналы раскастки (все каналы реплицируются клиентам). |

## 3. Events (server)

> 51 событие (список `VU-Docs/content/vext/ref/server/event/*.md`). Статусы: V = подписался в функции, R = реально работает в Legacy.

### 3.1 Events (server) - полный список

| Событие (VEXT) | Назначение | Статус |
|---|---|---|
| engine_init | Инициализация серверного модуля | V |
| engine_update | Фрейм движка | V |
| player_joining | Игрок подключается (до аутентификации) | V |
| player_authenticated | Аутентификация (GUID, sid, имя) | V |
| player_created | Игрок создан в игре | V |
| player_loadedtoserer | Клиент подключён к игре (модель) | V |
| player_update | Тик игрока (позиция, состояние) | V |
| player_updateinteract | Взаимодействие с объектами | V |
| player_changingweapon | Смена оружия | V |
| player_reload | Перезарядка | V |
| player_kitpickup | Подбор снаряжения | V |
| player_spawnatvehicle | Спавн в транспорте | V |
| player_spawnonplayer | Спавн как NPC | V |
| player_spawnonselectedspawnpoint | Спавн на точке | V |
| player_spawned | Готов к действию | V |
| player_resupply | Пополнение снаряжения | V |
| player_reviveaccepted | Спасён (согласие) | V |
| player_reviverefused | Спасён (отказ) | V |
| player_mandownrevived | Бросил оружие, спасён | V |
| player_destroyed | Убит | V |
| player_score | Счёт | V |
| player_teamchange | Смена команды | V |
| player_kickedfromsquad | Вывод из отряда | V |
| player_squadchange | Смена отряда | V |
| player_setsquadleader | Вождение отряда | V |

| player_enteredcapturepoint | Вход в зону захвата | V |
| player_exitedcapturepoint | Выход из зоны | V |
| player_enteredlevel | Переход на новый уровень | V |
| player_left | Покинул сервер | V |
| player_instantsuicide | Мгновенное самоубийство | V |
| soldier_prephysicsupdate | До физики (предсказание) | V |
| player_suppressedenemy | Перекрытие огнём | V |
| player_response | Ответ на действия сервера | V |
| player_chat | Сообщение чата | V |
| player_requestjoin | Запрос на подключение | V |
| player_findbestteam | Выбор лучшей команды | V |
| player_findbestsquad | Выбор лучшего отряда | V |
| player_selectteam | Выбор команды | V |
| server_roundover | Раунд окончен | V |
| server_roundreset | Резет раунда | V |
| level_loaded | Уровень загружен | V |
| level_destroy | Уничтожение уровня | V |
| lifecounter_basedestroyed | Тикер убит | V |
| weaponfiring_create | Создание огневой единицы | V |
| weaponfiring_update | Тик оружия | V |
| weaponfiring_updateammo | Патртосчёт | V |
| gunsway_update | Отпуск оружия | V |
| gunsway_updaterecoil | Отклонение/рекоил | V |
| soldier_healthaction | Действие персонажа по здоровью | V |
| soldier_mandown | Бросил оружие | V |
| vehicle_damage | Транспорт повреждён | V |
| vehicle_destroyed | Транспорт уничтожен | V |
| vehicle_disabled | Транспорт отключён | V |
| vehicle_enter | Игрок в транспорте | V |
| vehicle_exit | Игрок из транспорта | V |
| vehicle_spawndone | Транспорт готов | V |
| vehicle_unspawn | Транспорт удалён | V |
| scoringsystem_statevent | Состояние счётчика | V |
| capturepoint_captured | Зона захвата захвачена | V |
| capturepoint_lost | Зона захвата потеряна | V |
| combatarea_playerdeserting | Игрок покидает область | V |
| combatarea_playerreturning | Игрок возвращается в область | V |
| player_respawn | Респawn игрока | V |

## 4. Hooks (server)

> 9 хуков (список `VU-Docs/content/vext/ref/server/hook/*.md`). Статусы: V = подписался в функции, R = реально работает в Legacy.

### 4.1 Hooks (server) - полный список

| Хук (VEXT) | Назначение | Статус |
|---|---|---|
| bulletentity_collision | Столкновение пули с объектом | V |
| entityfactory_create | Создание сущности | V |
| entityfactory_createfromblueprint | Создание из blueprint | V |
| player_findbestsquad | Выбор лучшего отряда | V |
| player_findbestteam | Выбор лучшей команды | V |
| player_requestjoin | Запрос подключения | V |
| player_selectteam | Выбор команды | V |
| server_suppressenemies | Перекрытие огнём | V |
| soldier_damage | Урон персонажу | V |

## 5. Shared (client+server)

> Общий слой `VU-Docs/content/vext/ref/shared/` - библиотеки, события, хуки, типы. Источник правды для `ext/shared/init.lua` (LegacyEXT). Полные списки - в подкаталогах `library/`, `event/`, `hook/`, `type/`.

### 5.1 Библиотеки shared

| Библиотека | Главные методы | Назначение |
|---|---|---|
| entitymanager | Find/Spawn/Set через Entity | Управление сущностями (игроки, ТС, физемоб) |
| events | Subscribe/Trigger/Request/Init | Подписка и триггер событий (интеграция с server events) |
| hooks | Subscribe/Unsubscribe | Подписка на хуки (слот 2) |
| net | Send/Recv/Parse/Socket | Сетевой транспорт (SP/DP/UNI, Relay) |
| resourcemanager | Load/Unload/Query | Загрузка ресурсов карты |
| sharedutils | Now/GetLocalTime и пр. | Утилиты времени и строк |
| terrainediting | Raycast/Set... | Редактирование ландшафта (в боте редко) |

### 5.2 Структура расширения shared

`ext/shared/init.lua` (или `ext/shared/*.lua`) выставляет библиотеки через возврат таблицы вида `{ events = ..., hooks = ..., net = ... }`. Сопоставление с `VU-Docs/content/vext/ref/shared/library/*.md` - в работе (fun-bots проверяет вызываемые точки).

## 6. Examples (Lua)

> Фрагменты Lua-кода для LegacyEXT. Работают поверх `fun-bots` + `VU-Docs`.

### 6.1 Engine:Init - регистрация хуков и сетевых событий

```lua
local Events = require("events")
local Hooks = require("hooks")

Events:Subscribe("Engine:Init", function()
    Hooks:Subscribe("player_joining", function(p) print("player joining", p) end)
    print("LegacyEXT loaded")
end)
```

### 6.2 NetEvents:Subscribe + Broadcast (кастомика карты)

```lua
local Net = require("net")

Net:Subscribe("Legacy:Tp", function(data)
    local p = Net:ParsePlayer(data)
    print("tp target", p.id)
end)

-- от ботов-саперов: заклинание "teleport"
function TpPlayer(targetId, toPos)
    local msg = { cmd = "tp", target = targetId, pos = toPos }
    Net:Send("Legacy:Tp", msg)
end
```

## 7. Ready-bots (проверка совместимости)

> fun-bots (Joe91/fun-bots) - готовый набор Lua-ботов: `BF-Portable\Battlefield_3_Server\Instance\Admin\Mods\fun-bots` (с 16.10). Сопоставление с VEXT calls - в работе (gap analysis между ext/Server/*.lua и VU-Docs/content/vext/ref/server/*).

| Бот / модуль | Сложность | Статус | Примечание |
|---|---|---|---|
| fun-bots/ext/Server/__init__.lua | Low | Read | Инициализация хоста |
| fun-bots/ext/Server/Bot/BotGetters.lua | Low | Read | Получение игроков/ТС (Player/ControllableEntity) |
| fun-bots/ext/Server/Bot/BotMover.lua | Mid | In work | Навигация - смотрим в SDK/NavPowerSDK (bfxNavigator) |
| fun-bots/ext/Server/Bot/BotShooter.lua | Mid | In work | Выстрел + патроны (WeaponFiring) |
| fun-bots/ext/Server/Bot/BotVehicle.lua | Mid | In work | Управление ТС (Vehicle) |
| fun-bots/ext/Server/Bot/BotSniper.lua | High | Not started | Требует уточнения целей |
| fun-bots/ext/Shared/init.lua | Low | Read | Общий слой |
| fun-bots/mod.json | - | Read | Метаданные мода |

