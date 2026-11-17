# VEXT-API-MAP — что требуется от движка для fun-bots

Собрано исследованием (субагент, 10.10.2026) по реальному коду мода
`BF-Portable\Battlefield_3_Server\Instance\Admin\Mods\fun-bots` (с 16.10; раньше `Legacy\LegacyBF3\mods`) и документации `Legacy\VU-Docs\content\vext\ref\`.
Самодельных методов нет — всё сверено с доками. Это дорожная карта фазы 4.

Статус на 10.10 (вечер): сделано — Events, Console, print→лог, 16 таблиц-
перечислений, class(), json, string.starts/ends, свой require (с починкой
выдачи значения!), загрузчик модов, порядок shared→server, NetEvents,
SQL (настоящий SQLite), RCON. **fun-bots загружается ПОЛНОСТЬЮ** — весь
`server\__init__.lua` выполнен без ошибки в обоих контекстах тест-хоста.
Дальше — движковая часть (Engine:* и менеджеры), приоритет 1.

Приоритеты: 0 — загрузка мода, 1 — спавн ботов, 2 — поведение ботов,
3 — клиентский UI / node-редактор / WebUI (на сервере не требуется).

## ✅ Сделано

- **Events**: Subscribe/Unsubscribe/Dispatch/DispatchLocal —
  `src\LuaEvents.cpp/h`, подключён.
- **Console** (Register/Deregister/DeregisterAll) + print→лог —
  `src\LuaDebugConsole.cpp/h`. Debug — глобал мода, не наш.
- **Перечисления**: 16 таблиц (~1214 записей) по докам —
  `src\VextEnums.cpp/h` (InputDeviceKeys, Realm, TeamId, SquadId,
  EntryInputActionEnum, StatEvent, WeaponSlot и др.).
- **NetEvents** (сервер): Subscribe (2 формы), объект NetEvent с
  Unsubscribe, все Send*/Broadcast* — `src\LuaNetEvents.cpp/h`...
  *описка*: `src\VextNetEvents.cpp/h`, подключён. Реальные подписки;
  сетевого моста нет — заглушки с логом.
- **SQL**: настоящий SQLite 3.54.0 в DLL (`thirdparty\sqlite\`,
  amalgamation), 8 методов по `ref\server\library\sql.md` —
  `src\VextSql.cpp/h`. Первая версия падала на `bad self`, переписана
  в стиль VextUtils (C-функции без self + Lua-чанк с self-методами).
- **RCON** (сервер): RegisterCommand (2 перегрузки, реальные handle),
  SendCommand → {} (движка нет), Deregister, GetServerGuid → nil,
  TriggerEvent — заглушка; RemoteCommandFlag — `src\VextRcon.cpp/h`.
- **Починка require**: кэш-таблица не возвращается вместо значения
  модуля (баг стека после pcall) — MODLOG, 10.10 вечер.
- class() (middleclass), json, string.starts/ends, require, загрузчик,
  порядок shared→server — ранее.

## 🔄 В работе

- Движковые события `Engine:Init/Update/Message` и менеджеры
  `PlayerManager/Player/Entity` — следующий блок (приоритет 1).

## ⏳ Очередь

## 0 — загрузка мода (без этого не доходит до Extension:Loaded)

- **Events**: Subscribe, Dispatch, (опц. Unsubscribe, DispatchLocal)
- **NetEvents**: Subscribe, SendTo, SendToLocal, SendLocal (client),
  Send (client), BroadcastLocal, SendUnreliableToLocal
- **Hooks**: Install
- **SharedUtils**: IsClientModule, IsServerModule, GetLevelName,
  GetCurrentGameMode, GetTime, GetTimeNS, GetTickrate
- **MathUtils**: GetRandomInt, GetRandom, FNVHash
- **ResourceManager**: SearchForDataContainer, SearchForInstanceByGuid,
  FindDatabasePartition, GetSettings, RegisterInstanceLoadHandler, AddRegistry,
  MountSuperBundle
- **EntityManager**: GetIterator + EntityIterator:Next(),
  CreateEntitiesFromBlueprint
- **Console** (client): Register (команды config.*, Select, Add, …)
- **RCON** (server): SendCommand, RegisterCommand
- **ChatManager** (server): SendMessage, Yell
- **SQL** (server): Open, Close, Query, Escape, Error, LastInsertId
- **Net** (shared): GetHTTPAsync (UpdateCheck)
- Типы-касты: Entity(), SpatialEntity(), ServerSettings(),
  SyncedGameSettings(), FiringFunctionData(), AutoTeamEntityData(),
  HumanPlayerEntityData(), SoldierWeapon(), RegistryContainer(),
  EntityCreationParams(), EntityBus.entities
- Конструкторы: Vec3(), Vec2(), Vec4(), LinearTransform(), Guid(), EntryInput()

## 1 — спавн ботов (BotManager:CreateBot/SpawnBot, BotSpawner)

- **PlayerManager** (server): CreatePlayer(name, team, squad),
  DeletePlayer(player), GetPlayerCount(), GetPlayerById/ByName/ByOnlineId
- **Player** (server): CreateSoldier, SpawnSoldierAt, SelectUnlockAssets,
  SetSquadLeader, EnterVehicle, ExitVehicle; поля teamId, squadId, selectedKit,
  soldier, input, corpse
- **EntryInput**: конструктор; поля deltaTime, flags; методы SetLevel/GetLevel
- **SoldierEntity**: SetTransform, SetPose, Kill; поля maxHealth,
  worldTransform, pose, health
- **TeamSquadManager** (server): GetSquadPlayerCount(team, squad)
- **TicketManager** (server): GetTicketCount(team), GetCurrentRound(),
  GetRoundCount()
- **Entity** (EntityBusPeer): Init(Realm.Realm_ClientAndServer, true),
  RegisterEventCallback(...)
- **EntityManager:GetIterator** по "ServerCharacterSpawnEntity",
  "ServerVehicleEntity", "ServerCapturePointEntity",
  "ServerInputRestrictionEntity", "ServerSyncedBoolEntity"
- **ServerUtils** (server): GetCustomGameModeName()

## 2 — поведение ботов (ИИ, стрельба, движение, медики)

- **PlayerManager**: GetPlayers(), GetPlayersByTeam(team),
  GetPlayersBySquad(team, squad), GetPlayerByOnlineId(id)
- **SoldierEntity**: weaponsComponent.weapons[] (SoldierWeapon: primaryAmmo,
  secondaryAmmo, name), currentWeapon, currentWeaponSlot, pose, worldTransform
- **EntryInput**: :SetLevel(EntryInputActionEnum.*, v) — десятки членов
  (EIAThrottle … EIACycleRadioChannel)
- **Vec3**: :Distance, :Clone, :Normalize, :Dot; поля x/y/z; арифметика + - *
- **LinearTransform**: поля trans, forward, left, up; :Clone()
- **PhysicsEntityBase**: :GetPartTransform(partId) -> QuatTransform
- **QuatTransform**: :ToLinearTransform()
- **RaycastManager**: client Raycast/CollisionRaycast/DetailedRaycast;
  server CollisionRaycast/DetailedRaycast (+RayCastFlags)
- **ChatManager** (server): SendMessage — фолоу-боты, сообщения в чат
- Касты: SoldierWeapon(...), SpatialEntity(...)

## 3 — клиентский UI / node-редактор / WebUI

- **ClientUtils**: WorldToScreen, GetWindowSize, GetCameraTransform
- **DebugRenderer**: DrawText2D, DrawSphere, DrawLine, DrawOBB (+Vec4 цвета)
- **InputManager**: WentKeyDown, WentKeyUp, GetLevel(concept)
- **RaycastManager**: всё выше + DetailedRaycast
- **WebUI**: Init, Show, Hide, EnableMouse, EnableKeyboard, ResetMouse,
  ResetKeyboard, ExecuteJS
- Поля Player: squadId, teamId, soldier, name (для намтагов)

## Сигнатуры PlayerManager (server, из playermanager.md)

- CreatePlayer(name, team, squad) -> Player|nil
- DeletePlayer(player) -> void
- GetPlayerById(id) -> Player|nil
- GetPlayerByName(name) -> Player|nil
- GetPlayerByOnlineId(id) -> Player|nil
- GetPlayerCount() -> int
- GetPlayers() -> Player[]
- GetPlayersByTeam(team) -> Player[]
- GetPlayersBySquad(team, squad) -> Player[]
- GetLocalPlayer() -> Player|nil (client)

## Player / SoldierEntity / EntryInput (спавн и управление)

- Player:CreateSoldier(blueprint, transform) -> SoldierEntity|nil
- Player:SpawnSoldierAt(soldier, transform, pose) -> void
- Player:SelectUnlockAssets(characterCustomizationAsset, visualUnlockAssets[],
  miscUnlockAssets?[]) -> void
- Player:SetSquadLeader(leader: bool, squadPrivate: bool) -> void
- Player:EnterVehicle(vehicle, entryId) -> void
- Player:ExitVehicle(force, exitOnEntry) -> void
- Player.input = EntryInput() — + поля deltaTime, flags
- EntryInput:SetLevel(action: EntryInputActionEnum, level: float) — SetLevel/GetLevel
- SoldierEntity:SetTransform(transform) / SetPose(pose, imm, pending) / Kill()
- soldier.maxHealth = … / soldier.worldTransform (*).trans/forward
- soldier.weaponsComponent.weapons[] — SoldierWeaponsComponent
- Entity:Init(Realm.Realm_ClientAndServer, true)
- Entity:RegisterEventCallback(fn) -> int (на EntityBusPeer)

## ResourceManager / EntityManager

- SearchForDataContainer(name) -> DataContainer|nil
- SearchForInstanceByGuid(guid) -> DataContainer|nil
- FindDatabasePartition(guid) -> DatabasePartition|nil
- GetSettings(name) -> DataContainer|nil
- RegisterInstanceLoadHandler(partitionGuid, instanceGuid, callback)
- AddRegistry(reg, compartment)
- MountSuperBundle(superbundle)
- EntityManager:GetIterator(type) -> EntityIterator; EntityIterator:Next() -> Entity|nil
- CreateEntitiesFromBlueprint(blueprint, params) -> EntityBus|nil
- EntityCreationParams() + поля transform, networked

## Коммуникация (Events / NetEvents / Hooks / Console / RCON / Chat / Net / SQL)

- Events:Subscribe(name, ctx?, fn) -> Event; Events:Dispatch(name, ...)
- NetEvents:Subscribe(name, ctx?, fn); SendTo/SendToLocal/BroadcastLocal/
  SendUnreliableToLocal; client Send/SendLocal
- Hooks:Install(hook, priority, ctx?, callback)
- Console:Register(name, description, context?, callback) — client
- RCON:SendCommand(command) -> string{}; RCON:RegisterCommand(command,
  flags: RemoteCommandFlag, context?, callback) -> int
- ChatManager:SendMessage(message, toPlayer) -> bool; ChatManager:Yell(msg, dur) -> bool
- Net:GetHTTPAsync(url, callback)
- SQL:Open() -> bool; Query(query, ...) -> table|nil; Escape(text); Error();
  LastInsertId()

## Глобальные перечисления (нужны как значения)

Realm.Realm_ClientAndServer · ResourceCompartment.ResourceCompartment_Game ·
TeamAssignMode.TamOneTeam · RemoteCommandFlag.RequiresLogin ·
EntryInputFlags.AuthoritativeAiming · RayCastFlags.* · EntryInputActionEnum.*
(десятки) · WeaponSlot.* · CharacterPoseType.* · UpdatePass.* ·
HealthStateAction.OnRevive · StatEvent.* (CrateArmed/CrateDisarmed) ·
InputDeviceKeys.* · InputConceptIdentifiers.*

Примечание: StatEvent и UpdatePass в выгрузке VU-Docs отдельными файлами не
представлены (только в примерах) — источник имён реальный VU.

## События «от движка» (мод не рассылает сам — обязана поднять прослойка)

Серверные: Engine:Init; Engine:Update(dt, simDt); Extension:Loaded;
Extension:Unloading; UpdateManager:Update(dt, pass); Level:Loaded(level, mode,
round, roundsPerMap); Level:Destroy; Server:RoundOver(time, winningTeam);
Server:RoundReset; Player:Authenticated(player); Player:Joining/… (полный
список в ext/Server/__init__.lua).
