# PHASE1-MAP — сверка: что боту нужно ↔ что уже хукает botlayer

Дата 2026-10-08. Три источника:
1. `vext-inventory.md` — 303 API VeniceEXT из `VU-Docs\content\vext\ref\` (сгенерировано `vext_inventory.py`).
2. `funbots-api-usage.md` — что вызывают боты `mods\fun-bots` (сгенерировано `funbots_api.py` + ручной разбор подписок).
3. `BOTLAYER-REV.md` — разбор `botlayer.dll` (строки, аннотации Ghidra автора).

**Это документ фазы 1 и вход для фазы 4.** Определяет, что LegacyEXT обязан уметь.

## 1. Объём API VeniceEXT

| сторона | event | hook | library | type | всего |
|---|---|---|---|---|---|
| client | 30 | 24 | 14 | 55 | 123 |
| server | 51 | 9 | 10 | 21 | 91 |
| shared | 16 | 4 | 8 | 61 | 89 |
| **ВСЕГО** | **97** | **37** | **32** | **137** | **303** |

Полный VU = 303 API. Это мало. **Реализовать все 303 — реальная задача, не «годы реверса».**

## 2. Серверные хуки — самые ценные (9 штук)

```
Bulletentity:collision          Player:requestjoin
Entityfactory:create            Player:selectteam
Entityfactory:createfromblueprint   Server:suppressenemies
Player:findbestsquad            Soldier:damage
Player:findbestteam
```
Плюс 4 shared: `Entity:shouldcollidewith`, `Resourcemanager:loadbundles`,
`Terrain:load`, `Visualterrain:load`.

Итого хуков сервер+shared = **13**. Это и есть точки, где можно вмешаться
в поведение сервера. Остальное — события (уведомления) и библиотеки.

## 3. Что боты реально вызывают (51 вызов менеджеров)

Самое частое (число файлов):
```
MathUtils:GetRandomInt         11   MathUtils:GetRandom          7
PlayerManager:GetPlayerById     7   PlayerManager:GetPlayers      5
ChatManager:SendMessage         6   EntityManager:GetIterator     5
PlayerManager:GetLocalPlayer    4   PlayerManager:GetPlayersByTeam 4
ResourceManager:SearchForDataContainer 4
```
Полный список — в `funbots-api-usage.md`. Ключевые группы:

| Библиотека | Зачем боту | Методов |
|---|---|---|
| `PlayerManager` | найти игроков, спавн, удалить | 10 |
| `RaycastManager` | проверка LOS/попаданий (`CollisionRaycast`, `Raycast`, `DetailedRaycast`) | 3 |
| `ChatManager` | `SendMessage`, `Yell` — отчёты бота | 2 |
| `EntityManager` | `GetIterator`, `CreateEntitiesFromBlueprint` | 2 |
| `ResourceManager` | доступ к EBX-данным (спавн, оружие) | 8 |
| `MathUtils` | `GetRandomInt`, `GetRandom`, `FNVHash` | 3 |
| `SharedUtils` | `GetTime`, `GetTimeNS`, `GetTickrate`, `GetLevelName`, `GetCurrentGameMode` | 6 |
| `TicketManager` | `GetCurrentRound`, `GetRoundCount`, `GetTicketCount` | 3 |
| `TeamSquadManager` | `GetSquadPlayerCount` | 1 |
| `NetEvents` | `SendToLocal`, `SendLocal`, `BroadcastLocal`, `Subscribe` | 7 |
| `Events` / `Hooks` | `Subscribe`, `Dispatch`, `Install` | 2 |

**Вывод:** боту не нужен весь VU. Нужны ~51 метод + ~37 событий + 13 хуков.

## 4. События движка, на которые подписан бот (из `Events:Subscribe`)

Извлечено из `mods\fun-bots\ext` (документированные VEXT + 1 кастомный):

**Серверные (нужны LegacyEXT в первую очередь):**
```
Engine:Init            Level:Loaded          Level:Destroy
Engine:Update          Level:LoadResources   Level:RegisterEntityResources
Partition:Loaded       Extension:Loaded      Extension:Unloading
Player:Authenticated   Player:Joining        Player:Left
Player:Killed          Player:Respawn        Player:Chat
Player:TeamChange      Player:EnteredCapturePoint  Player:ExitedCapturePoint
Player:Deleted         CapturePoint:Captured CapturePoint:Lost
CombatArea:PlayerDeserting    CombatArea:PlayerReturning
LifeCounter:BaseDestroyed     ScoringSystem:StatEvent
Server:RoundOver       Server:RoundReset
Soldier:HealthAction   Vehicle:Damage
UpdateManager:Update
```
**Клиентские:**
```
Client:UpdateInput     UI:DrawHud
```
**Кастомные бота (наш код, не движок):**
```
Bot:AbortWait  Bot:ExitVehicle  BotEditor
UI_* (12 штук)  NodeCollection:FinishedLoading
SpawnPointHelper:TeleportTo  Botmanager:RaycastResults  PathMenu:Request
```

## 5. ★ Сверка: botlayer ↔ потребности ботов (главная таблица)

Левая колонка — что боту нужно (событие VEXT). Правая — что botlayer уже хукает
(аннотация автора из Ghidra, адрес в `bf3_server_Final.exe`).

| Потребность бота (VEXT) | Хук botlayer | Статус |
|---|---|---|
| `Player:Killed` | `ServerSoldierHealthModuleImpl::onDamage` | ✅ **покрыто** (урон→смерть) |
| `Player:Chat` | `ServerChatManager::handleMessage` | ✅ **покрыто** |
| `Player:TeamChange` | `ServerPlayerSquadManager::switchTeams` | ✅ **покрыто** |
| `Player:EnteredCapturePoint` | `ServerPlayerSquadManager::switchTeams` | ⚠️ частично |
| `CapturePoint:Captured/Lost` | — | ❌ нет хука capture point |
| `Server:RoundOver/RoundReset` | `ServerPlayerSquadManager::onNewRound` | ✅ **покрыто** |
| `Player:findbestteam` / `selectteam` | `ServerAutoTeamEntity::onMessage` | ✅ **покрыто** |
| `Soldier:HealthAction` | `ServerSoldierHealthModuleImpl::onDamage` | ✅ **покрыто** |
| `Vehicle:Damage` | — | ❌ нет хука vehicle damage |
| `Engine:Update` (тик) | `ServerPlayer::update` | ✅ **покрыто** |
| `Engine:Init` | — | ❌ нет хука инициализации |
| `Level:Loaded` | — | ❌ нет |
| `Level:LoadResources` | — | ❌ нет |
| `Partition:Loaded` | — | ❌ нет |
| `Extension:Loaded` | `DllMain` botlayer | ✅ частично (свой вход) |
| `UpdateManager:Update` | `ServerPlayer::update` | ✅ **покрыто** |
| `Soldier:damage` (хук) | `ServerSoldierHealthModuleImpl::onDamage` | ✅ **покрыто** |
| `Entityfactory:create` | — | ❌ нет |
| `Player:requestjoin` | — | ❌ нет |
| `Server:suppressenemies` | — | ❌ нет (свой код бота) |
| `Bulletentity:collision` | `ServerProjectileEntityUpdater::update` | ⚠️ смежное |

**Покрыто ~12 из 26. Не хватает ~14** — это и есть точная работа для LegacyEXT.

## 6. Пробелы по приоритету

**P0 (без них бот не играет):**
1. `Engine:Init` — инициализация, загрузка brain.ini
2. `Engine:Update` / `UpdateManager:Update` — тик, уже есть через `ServerPlayer::update`
3. `Player:Created/Joining/Authenticated` — появление бота
4. `Player:Killed` — ✅ есть
5. `Soldier:HealthAction`, `Soldier:damage` — ✅ есть

**P1 (качество боя):**
6. `Vehicle:Damage` — нужен хук на vehicle damage
7. `CapturePoint:Captured/Lost` — цель бота (идти к флагу)
8. `Player:ChangingWeapon`/`Resupply` — смена оружия
9. `Bulletentity:collision` — попадания (есть смежный хук на снаряды)
10. `Server:suppressenemies` — подавление (для «человечного» бота)

**P2 (навигация и вспомогательное):**
11. `Level:Loaded` / `Partition:Loaded` — когда можно грузить ресурсы
12. `Entityfactory:create` — спавн сущностей
13. `Player:requestjoin` — обработка запроса входа

**P3 (инфраструктура):**
14. `Client:UpdateInput`, `UI:DrawHud` — клиентская часть (фаза 4 позже)

## 7. Что уже НЕ надо делать

- **Не нужен `PlayerManager:CreatePlayer` через свой код** — боты уже умеют
  `PlayerManager:CreatePlayer` (1 файл). Значит LegacyEXT может оставить создание
  игроков на VEXT-API, а не хукать движок.
- **Не нужен отдельный `Player:Deleted`** — в документации VU нет такого
  события (только `Destroyed`), бот подписан на несуществующее в ref → это
  баг/расширение fun-bots, проверить.
- **Не нужны клиентские UI-хуки для серверных ботов** (24 client hook) —
  только если делаем UI.

## 8. Прямой вывод для фазы 4

Порядок реализации LegacyEXT (по убыванию «польза ÷ сложность»):

```
1. Runtime: Lua 5.4 + обёртки над хуками botlayer (9 штук)      ← уже готово в botlayer
2. PlayerManager-обёртки (10 методов) — самая частая потребность
3. События поверх существующих хуков (P0) — тривиально, хук уже есть
4. RaycastManager-обёртки (3 метода) — LOS бота
5. Новые хуки (Vehicle:Damage, CapturePoint, Level:Loaded) — Ghidra по bf3_server
6. ResourceManager-обёртки (8 методов) — для спавна/оружия
```

**Ключевой инсайт:** ~60% нужных боту событий уже доступны через 9 существующих
хуков botlayer. Настоящая работа — **обёртки**, а не новые хуки. Это сильно
сокращает объём по сравнению с «полный реверс движка».

## 9. Проверить дальше (шаг 3 фазы 1)

1. Ghidra-разбор botlayer.dll (`ghidra-botlayer.txt`): таблица AOB, `.fptable`.
2. Перевести 8 вшитых адресов движка в AOB-сигнатуры по `bf3_server_Final.exe`.
3. Уточнить, что именно `ServerPlayer::update` отдаёт как контекст (нужно для
   `Player:Update` и `UpdateManager:Update`).
4. Сверить `brain.ini` (93 КБ) — какие из 134 флагов бот-слой читает в рантайме.

Все выводы — в `Legacy\ext\MODLOG.md`.