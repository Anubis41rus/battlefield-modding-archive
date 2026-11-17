# Legacy: готовность ботов «как у VU», оценка v2 — 18.10.2026

Снимок документов на ≈20:15: STATE, PLAN, MODLOG (блок «Сборка 186» и «Пачка заглушек, 2-я часть»), HANDOFF, предыдущая оценка
READINESS-2026-10-18. Код моста `legacyext\src` и Lua мода (`BF-Portable\Battlefield_3_Server\Instance\Admin\Mods\fun-bots\ext`)
читались только для чтения. Стенд, сборка, Ghidra и игра не запускались. Прогона после 20:01 в журнале нет: оценка может отставать.

Метки: [прогон] — есть в MODLOG/STATE (лог или слова пользователя); [код] — реализовано в мосте, в прогонах не видно;
[нет] — заглушка или отсутствует. В таблице два числа: «подтв.» — подтверждено прогоном, «реал.» — реализовано (включая [код]).
Веса мои, погрешность ±10 п.п.

## 1. Итог

| # | Пункт | Вес | Подтв. | Реал. | Коротко |
| --- | --- | --- | --- | --- | --- |
| 1 | Серверный VEXT API | 15 | 60 | 75 | По имени 13 из ≈88 методов мода — заглушки или пусто; 32 игровых события мода есть в мосте |
| 2 | Режимы: CQ, TDM, Rush, SQDM | 20 | 45 | 59 | CQ и TDM — [прогон]; Rush — этапы не подтверждены; SQDM — баг найден после прогона; GM/AirSuperiority не гонялись |
| 2б | Бот как солдат: киты, внешность, перки, иконки, отряды (добавлен) | 10 | 45 | 58 | Киты 8/8 и выбор через движок 40/40 — [прогон]; перки 0 из 21 — не работают |
| 3 | Техника и авиация | 15 | 55 | 65 | Езда и стрельба из техники — [прогон]; взлёт вертолётов и самолётов — [прогон] 185; регрессия 186-2 на большой карте |
| 4 | Гаджеты, медик, пополнение, StatEvent | 10 | 50 | 60 | Реанимация — [прогон]; StatEvent идёт (20:01); аптечки и ящики на карте не найдены (0) |
| 5 | Смена раундов, карт, ротация | 10 | 70 | 80 | Смена раунда по движку и смена карт — [прогон] 15:21–19:08; ожидание уровня на CQL — исправлено в исходнике, не собрано |
| 6 | Статистика, ранги, бэкенд | 8 | 50 | 65 | Реальные отчёты игры разобраны (15:44), двойной учёт исправлен; звания и синхронизация между ПК не проверены |
| 7 | Клиентская часть VEXT | 7 | 0 | 5 | Клиентского модуля нет; NetEvents:Send* только пишут в журнал |
| 8 | Комплект для друга | 5 | 40 | 60 | Тихая установка проверена, um publish check PASS; на втором ПК не проверен; нужна пересборка |
| | ИТОГ | 100 | ≈49 | ≈61 | Прошлая оценка ≈54 (другая сетка пунктов) |

**Итог: реализовано ≈61 %, подтверждено прогонами ≈49 %.**

## 2. Что изменилось с прошлой оценки (18.10 ≈15:20)

- Закрыто прогоном: RCON — вход и чтение переменных [16:42] (`Player:Kick` → `admin.kickPlayer` — [код]);
  `Entity:RegisterEventCallback` [17:09, 18:43]; `StatEvent`, `gunSway` [17:09];
  `SetSquadLeader` через RCON [20:01: OK].
- Закрыто кодом: `ExitedCapturePoint`, `Extension:Unloading`, `FNVHash`, `SpatialEntity`, `EntityFactory:Create` (186-2),
  `LifeCounter:BaseDestroyed` (после 19:08).
- Устарело: «чат выключен» — с 186 включён (`VextChatManager.cpp:41–42`), в игре не проверен; «победитель всегда 0» и
  «TDM не закрывался» — исправлено [15:28]. Не закрыто: перки, клиент, зона Rush, RCON-команды мода, отправка NetEvents.

## 3. Пункт 1. Серверный VEXT API (подтв. 60 / реал. 75)

- Мод вызывает ≈88 имён методов VU (Server+Shared, пересечение с `VU-Docs\content\vext\ref`); по имени заглушка или пусто — 13:
  - на игровом пути: `TicketManager:GetCurrentRound/GetRoundCount` (всегда 0; мод берёт их в `Server\__init__.lua:773`, только
    резервный путь — `Level:Loaded` получает счётчик раундов моста, `ModdingInterface.cpp:340–344`);
    `SoldierEntity:SingleStepEntry` (только поле; `Server\BotManager.lua:153`, техника работает через EnterVehicle [прогон]);
    `RCON:RegisterCommand` (хранит, движок не вызывает; мод регистрирует `funbots.config.*` в `Server\Commands\RCON.lua:268`);
  - сеть: `NetEvents:SendTo/SendToLocal/BroadcastLocal/SendUnreliableToLocal` — только журнал (`VextNetEvents.cpp:104–110`,
    вызовы 128–211; мод: `Server\Commands\Console.lua:74` и клиентские UI);
  - редко: `physicsEntityBase:GetPart` → nil (только чат-команда, `Server\Commands\Chat.lua:104`); `Net:GetHTTPAsync` → callback(nil)
    (`VextNet.cpp:45–50`; проверка обновлений `Server\UpdateCheck.lua:181`, интернета нет по замыслу);
  - не на игровом пути: `EntityManager:CreateEntitiesFromBlueprint` — функции нет, только `SpawnGrenade` (`Server\__init__.lua:677`);
    `ResourceManager:AddRegistry`, `MountSuperBundle` — пусто (`Shared\__init__.lua:73–91`, код за флагом
    `USE_LOAD_BUNDLE_BUGFIX=false`; следствие — пропадает оружие на MP_011); `SoldierEntity:Destroy` — флаг по замыслу.
- Работает (код; часть [прогон]): PlayerManager; CreatePlayer/DeletePlayer; Player:CreateSoldier, SpawnSoldierAt, SelectUnlockAssets,
  EnterVehicle, ExitVehicle, SetSquadLeader; SoldierEntity:Kill, SetPose, SetTransform, ApplyCustomization; Entity:RegisterEventCallback,
  FireEvent; EntityManager:GetIterator (8 типов); ResourceManager (поиск, GetSettings); RaycastManager (кэш 2 с); SQL (sqlite3);
  ChatManager (очередь, `VextChatManager.cpp:29–60`); Events, Hooks; MathUtils; SharedUtils (уровень и режим из движка); MakeWritable.
- События: мод подписан на 87 имён на сервере. 32 игровых есть в мосте [код]; 2 упоминаются, но клиентские или редакторские
  (`Client:RequestSettings`, `NodeCollection:FinishedLoading`). Остальные 53: редактор путей (39 имён: `NodeEditor`, `NodeCollection`,
  `PathMenu`, `BotEditor`, `SpawnPointHelper`), `ConsoleCommands` (5), `UI_Request_*` (3), клиентские `Client:RequestEnterVehicle` и
  `RequestChangeVehicleSeat` (2), `Level:LoadResources` и `RegisterEntityResources` (выключены вместе с бандлами, 2), `Bot:*` (мод шлёт себе сам, 2).
- Хуки мода. Сервер (4): `Soldier:Damage` [прогон: ответный огонь, сборка 167]; `BulletEntity:Collision` [прогон: урон пуль];
  `EntityFactory:Create` [код, 186-2: установлен, ловушки и ракеты не проверены]; `ResourceManager:LoadBundles` [нет, выключено].
  Клиент (2): `UI:PushScreen`, `Input:PreUpdate` — [нет].
- Не проверено в игре: чат (`Player:Chat`, `ChatManager:SendMessage`), `Vehicle:Damage` без стрелявшего.

## 4. Пункт 2. Режимы (подтв. 45 / реал. 59)

- **Conquest** (CQS MP_001, CQS MP_003, CQL MP_007). [прогон] 16:42 (MP_001→MP_003→MP_001, VEH 0), 17:09 (CQ×3, 81 посадка,
  VEH 0). [прогон] 13:xx: авиация и техника на CQL. [нет] 20:01 — регрессия на старте CQL: мост отдал моду 0 флагов, цель
  авиации (0,0,0); 9 ботов в технике без цели убиты модом через 50 с (`GameDirector.lua:217`). Исправлено в
  `ModdingInterface.cpp:251–259` (предел 20 с убран), не собрано.
- **TDM.** [прогон] конец раунда по движку (15:28, «победила команда 2», 439 с); смена уровня без исключений (15:53, 33 посадки
  во 2-м раунде).
- **Rush.** [прогон 19:08] цели получили событие `#D0488C09`; оба МКОМ взорваны (19:15:46, 19:15:56) — боты закладывают и
  взрывают. [код] `LifeCounter:BaseDestroyed` уходит моду на `#2CFC2C6C`/`#324B3FE6` — после 19:08, не проверено. [код] спавн без
  точки спавна (все 11 точек с `+0x19C=0`, 186-2) — не проверено. [нет] зона Rush (`ServerSyncedBoolEntity`): мод только пишет в
  журнал. [нет] `CrateArmed` (86) не пришёл ни разу (19:08).
- **SQDM.** [прогон 19:08] смен команд 0, убийств модом 0, смертей от урона 13. Причина: мод ставит команды 3 и 4, мост писал 1/2,
  петля TeamChange → Kill. Исправлено (команды 1..4), не проверено.
- **GM, AirSuperiority и др.** В журнале не упоминаются — [нет].

## 5. Пункт 2б. Бот как солдат (подтв. 45 / реал. 58)

- Киты 8/8 из базы движка; выбор через движок 40 из 40 [прогон 185, 06:44].
- Перки: 0 из 21 разблокированы даже при `vars.unlockMode all` [прогон 185]; запись полями эффекта не даёт. [нет].
- Значок класса: флаг 0x40 после кита (`KitFlagOn`, 185, [код]); подтверждения пользователя в журнале нет.
- Ники: `isAI 0` даёт очки и звания; ники есть не у всех (READINESS 18.10).
- Отряды: `SetSquadLeader` → RCON `squad.leader`, «OK» у всех лидеров [прогон 20:01]; эффект в игре не проверен.

## 6. Пункт 3. Техника и авиация (подтв. 55 / реал. 65)

- Наземная: посадка 60 из 60 [прогон 05:18, 185]; едет 9–13 м/с [прогон 180]; стреляет из техники — подтвердил пользователь
  (183). Застрявшую машину не переносим (`LEGACYEXT_VEH_TELEPORT` выключен, падение 16:07). Танк на камне — жалоба 20:01 [нет].
- Вертолёты: взлёт до 259 м, крен ≈0 после правки знака крена [прогон, 185]. Стрельба с вертолётов и наведение ракет [нет].
- Самолёты: 10 взлётов до 852 м на MP_007 [прогон, 185]. Динамический спавн на XP5 — правка 185 через `GetCurrentGameMode`, не проверена.
- Ловушки и дым (`EntityFactory:Create`, 186-2): [код], эффект не проверен. AA-управление в журнале не упоминается.
  Регрессия 186-2 (20:01, CQL): вертолёты и самолёты не взлетают на старте большой карты — см. пункт 2.

## 7. Пункт 4. Гаджеты, медик, пополнение, StatEvent (подтв. 50 / реал. 60)

- Реанимация: `Soldier:HealthAction OnRevive` [прогон 08:49 BOT_Grayson, 09:20 BOT_NoFaTe, 185]; «реанимаций за день 10» (18.10).
- Слоты оружия: убийства со слотов 2, 5, 6, 7 [прогон 17:09–18:43]. Медики с дефибриллятором 6–9 [прогон 185].
- StatEvent: 45 событий (коды 18, 22, 24, 25) [прогон 17:09]; центральный перехват `executeEvent` (186-2) — «StatEvent идут» [прогон
  20:01]. МКОМ 86/87 через перехват — [нет], не проверено.
- Пополнение: аптечек и ящиков на карте 0 [прогон 19:08]; `ServerSupplySphereEntity` и «ДИАГ гаджеты» добавлены [код], результата нет.

## 8. Пункт 5. Раунды, карты, ротация (подтв. 70 / реал. 80)

- Конец раунда по движку → `Level:Destroy` → `Level:Loaded` → `Server:RoundReset` [прогон 15:21, 15:44, 16:42].
- Смена карт и режимов: CQ×3 [прогон 17:09]; Rush → SQDM [прогон 18:43, 19:08]; ротация по `MapList` [прогон].
- Регрессия на старте CQL (20:01): см. пункт 2, в исходнике исправлено, не собрано.
- Счётчик раундов моста (`GetCurrentRound` — 0) не влияет на `Level:Loaded` (событие берёт `roundCount` из моста).

## 9. Пункт 6. Статистика, ранги, бэкенд (подтв. 50 / реал. 65)

- Разбор `GameReporting` (`Blaze\GameReporting.cs`): проверен на 12 журналах (убийства 45 = `progress.json`) [прогон, selftest].
- Игра прислала отчёты 0x64 (30 игроков) и 0x65 [прогон 15:44]. Двойной учёт исправлен (профиль 234471788 — по журналам, повторного
  прогона нет).
- `POST /accounts/{login}/progress` (слияние максимумом) и `ProgressSync.cs` в лаунчере: [код], проверены selftest и HTTP без игры
  (18.10). Звание за игроком и синхронизация между ПК в игре [нет].

## 10. Пункт 7. Клиентская часть VEXT (подтв. 0 / реал. 5)

- В `legacyext\src` клиентских библиотек нет: поиск `InputManager|WebUI|ClientUtils|CommoRose` даёт только enum в `VextEnums.cpp`.
- `NetEvents:SendTo*` и `Broadcast*` — журнал «транспорт не подключен» (`VextNetEvents.cpp:104–110`).
- Файлы клиентской части мода (`ext\Client\*.lua`, 8 файлов) не загружаются. Не работают: меню, роза команд, настройки, ники на клиенте,
  консольные команды (`Console:Register` в `LuaDebugConsole.cpp` — TODO).

## 11. Пункт 8. Комплект для друга (подтв. 40 / реал. 60)

- Отдан 18.10: `dist\LegacyFriend-kit-2026-10-18` — установщик `LegacySetup` (брандмауэр, русский язык через реестр, проверка путей).
  Тихая установка распаковала все файлы; `um publish check` — PASS (23 файла).
- На втором ПК не проверен. Нужна пересборка с итоговой DLL и бэкендом после прогона 20:15 (STATE, «Открыто»).
- Лаунчер пишет `Config.lua` мода (`InitNumberOfBots = 15`) при старте хоста. Это расходится с правилом «мод не трогаем»; решение
  пользователя не принято (MODLOG, «Открыто»).

## 12. Заглушки: полный список (файл:строка)

Используемые модом на игровом пути или рядом:
1. `legacyext\src\VextTicketManager.cpp:116–122` — GetCurrentRound, GetRoundCount → 0 (мод: `Server\__init__.lua:773`).
2. `legacyext\src\VextPlayerManager.cpp:465` — SoldierEntity:SingleStepEntry, только поле (мод: `Server\BotManager.lua:153`).
3. `legacyext\src\VextPlayerManager.cpp:302` и `VextEntityManager.cpp:1095` — ph:GetPart → nil (мод: `Server\Commands\Chat.lua:104`).
4. `legacyext\src\VextRcon.cpp:79–116` — RegisterCommand хранит, движок не вызывает (мод: `Server\Commands\RCON.lua:268`).
5. `legacyext\src\VextNetEvents.cpp:104–110, 128–211` — Send*/Broadcast* → только журнал; заголовок 13–16; лог 274.
6. `legacyext\src\VextNet.cpp:8, 45–50, 73–74` — async → callback(nil) (мод: `Server\UpdateCheck.lua:181`).
7. `legacyext\src\VextResourceManager.cpp:364–365, 380` — MountSuperBundle, UnmountSuperBundle, AddRegistry: пусто (мод: `Shared\__init__.lua:73–91`, флаг выключен).
8. `legacyext\src\VextUtils.cpp:281–287` — GetTickrate → 30 (TODO); мод: `Server\BotManager.lua:55, 651` — допустимо.
9. `legacyext\src\VextPlayerManager.cpp:280–290` — velocity: фолбэк (1,0,0) до первого замера; `LEGACYEXT_VEL_STUB` по умолчанию включён (`VextPlayerManager.cpp:10666–10668`).
10. `legacyext\src\VextEntityManager.cpp:1041` — здоровье техники (`health`) = 100; `internalHealth` из движка (183).
11. `legacyext\src\VextEntityManager.cpp:1301` — EntityManager:GetIterator: пусто для остальных типов (HANDOFF §6).
12. `legacyext\src\VextPlayerManager.cpp:477` — SoldierEntity:Destroy: флаг по замыслу.
13. Нет функции: `EntityManager:CreateEntitiesFromBlueprint` (мод: `Server\__init__.lua:677`, только SpawnGrenade).

Не вызываются модом (в Server и Shared нет вызовов):
14. `legacyext\src\VextPlayerManager.cpp:631–638, 850–852` — Player:Ban*, Spawn (false), SelectWeapon, AttachSoldier, EnableInput, Fade.
15. `legacyext\src\VextEntityManager.cpp:1339–1341` — EntityManager:FindEntityByName → nil.
16. `legacyext\src\VextSpatial.cpp:85–87` — SpatialEntity:DistanceTo → 0; `VextSpatial.cpp:88–` — AxisAlignedBox, нулевые данные.
17. `legacyext\src\VextUtils.cpp:105–117, 166–172, 254–260, 288–292` — GetYPRFromULF и GetTransformFromYPR (nil), RandomGuid, GetContentPackages ({}), GetMemoryUsage (0).
18. `legacyext\src\VextResourceManager.cpp:381–392` — BeginLoadData (0), EndLoadData, PollBundleOperation, BeginClear, EndClear, AllocateDynamicCompartment (таблица), DestroyDynamicCompartment.
19. `legacyext\src\VextRcon.cpp:165` — TriggerEvent: только журнал; GetServerGuid → nil.
20. `legacyext\src\VextResourceManager.cpp:1873` — DataContainer:Clone (живой объект) → nil.
21. `legacyext\src\LuaDebugConsole.cpp:175, 193, 204` — Console:Register/Deregister: TODO, клиентская консоль.

Устаревшие комментарии «заглушка» (код уже работает):
22. `legacyext\src\VextChatManager.cpp:11–12` — чат шлёт (очередь, строки 29–60).
23. `legacyext\src\VextSpatial.h:1, 7` и `VextSpatial.cpp:1, 4` — SpatialEntity уже прокси (186-2); заглушка остаётся у AxisAlignedBox и DistanceTo (п. 16).
24. `legacyext\src\LuaRuntime.cpp:565, 573`; `VextGlobals.cpp:1, 91` — библиотека Globals; мод использует свой `Server\Model\Globals.lua`.

## 13. Топ-5 недоделок

1. Регрессия на больших картах (CQL): ожидание уровня 20 с → 0 флагов → авиация не взлетает, боты в технике убиты через 50 с.
   Правка в `ModdingInterface.cpp:251–259` есть, не собрана.
2. Клиентского VEXT нет (5 %): меню, роза команд, настройки; NetEvents:Send* пишут только в журнал.
3. Перки не работают: 0 из 21 разблокированы даже при unlockMode all; значок класса не подтверждён.
4. Rush и SQDM не подтверждены после правок: этапы по `LifeCounter:BaseDestroyed`, спавн без точки, зона Rush (только журнал),
   SQDM — команды 3 и 4.
5. RCON-команды мода (`funbots.config.*`) не вызываются движком; аптечки и ящики на карте не найдены (0); `Config.lua` — решение открыто.
