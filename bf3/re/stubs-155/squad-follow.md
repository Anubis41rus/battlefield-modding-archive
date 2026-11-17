# Трек 8. Смена команды человеком, перевод бота, отряд «бегает за игроком» (сборка 155)

Дата: 15.10.2026. Режим: только чтение. Стенд, сборка, DLL, `legacyext\src` не трогались, ничего не удалено.
Пути мода: `MOD` = `C:\Games\BF-Portable\Battlefield_3_Server\Instance\Admin\Mods\fun-bots\ext`. Копия `C:\Games\Legacy\LegacyBF3\mods\fun-bots` совпадает по номерам строк ключевых мест (BotSpawner.lua:332, 556, 2322; BotManager.lua:842, 1637), версия одна.
Метки: **[факт]** подтверждено кодом, дизассемблером, декомпиляцией или логом; **[гипотеза]** не подтверждено, дан способ проверки; **[опровергнуто]**.
Скрипты анализа бинарника (read-only): `C:\Temp\claude\C--Games\c4657c58-f592-45b8-8608-819ca88fc837\scratchpad\8\` (dis.py … dis5.py, strs.py, strs2.py).

---

## 0. Итог

1. **[факт]** В моде нет режима «перевод бота». При смене состава команд мод делает `KillAll` лишнего в одной команде и `SpawnWayBots` в другой (BotSpawner.lua:556–586). Это и есть «создаёт нового, через 3–5 с удаляет лишнего». Ни один SpawnMode и ни один TeamSwitchMode перевода не делает.
2. **[факт]** Lua-запись `Player.teamId` у движкового бота не меняет команду в движке. Это обычное поле таблицы (VextPlayerManager.cpp:115–124, 605), а мост раз в 0,5 с возвращает команду из движка и откатывает запись (1741–1766). Поэтому 5 мест мода, которые пишут `teamId` (см. 2.1), на нашем мосте не переносят ботов.
3. **[гипотеза, главная версия расхождения с VU]** В VU запись `Player.teamId` могла менять команду живого бота в движке (VU-доки не запрещают запись, пометки read-only у `teamId` нет: player.md:24, 133–135). Тогда тот же код fun-bots в VU давал перенос. Проверки нет (VU-сервера нет).
4. **[факт]** Перевод в движке делается штатной последовательностью движка: `addToTeam(игрок, команда, 0, 0)`, затем `addToSquad(игрок, отряд, 0, 0)`. Движок так переносит игроков в своей балансировке (re\balance_decomp.txt:292–341). Перенос требует: проверку ёмкости команды, убийство тела до смены, одновременное обновление Lua и движка в одном тике, и обход `Bot:Kill()` (он выключает респавн).
5. **[гипотеза, главная для пункта 3]** «Бегают со мной» не является автоследованием мода. Автоматическая связь «отряд человека ↔ боты» в моде только такая: боты отряда человека спавнятся у его солдата (20 % на спавн) или садятся в его технику (70 %). Команда «Follow Me» (командное колесо мода) в моде есть, но её действие приходит клиентским NetEvent `BotEditor` (action `follow_me`, UIServer.lua:34, 88–93), а мост клиентские NetEvent по коду не маршрутизирует. В бинарнике есть движковый приказ `OrderType_FollowMe`; действует ли он на AI-ботов, не проверено.
6. **[факт]** Перенос ботов в отряд человека (`SquadFollowEnabled`) в мосте выключен по умолчанию (VextPlayerManager.cpp:4242–4250, флаг `LEGACYEXT_SQUAD_FOLLOW=1`). Если его не включали на стенде, эта причина исключается.

---

## 1. Вопросы трека

1. Почему при смене команды человеком мод создаёт нового бота, а не переводит существующего? Есть ли в моде режим перевода? Что могло быть в VU иначе?
2. Если делать перевод в мосте: как сменить команду существующему движковому боту (поле +0x31C, `addToTeam` VA 0x0118EF80, `addToSquad`), что ещё нужно (тело, респавн, отряд), какие риски.
3. Логика следования за отрядом/игроком в моде и что ей отдаёт мост.

---

## 2. Факты

### 2.1 Мод: балансировка команд и смена команды

- **Конфиг** (`MOD\Shared\Config.lua`): `SpawnMode = balanced_teams` (36), `TeamSwitchMode = SwitchForRoundTwo` (38), `SpawnInBothTeams = true` (39), `InitNumberOfBots = 8` (40), `NewBotsPerNewPlayer = 1.6` (41), `FactorPlayerTeamCount = 1.0` (42), `BotTeam = 0` (43), `TraceDelta = 0.35` (132), `KeepOneSlotForPlayers = true` (163), `RespawnWayBots = true` (169).
- **Registry** (`MOD\Shared\Registry\Registry.lua`): `BOT.BOT_DESTROY_DELAY = 0.05` (195); `BOT_TEAM_BALANCING` THRESHOLD 6, ALLOWED_DIFFERENCE 1 (202–208); `BOT_SPAWN.KEEP_BOTS_ON_NEW_ROUND = true` (213); `PROBABILITY_SQUADMATE_SPAWN = 20` (220); `PROBABILITY_SQUADMATE_VEHICLE_SPAWN = 70` (222); `PROBABILITY_TEAMMATE_SPAWN = 1` (224); `PROBABILITY_TEAMMATE_VEHICLE_SPAWN = 50` (226). `CheckProbability(p)` = `random(1..100) <= p` (`MOD\Shared\Utilities.lua:169–171`).
- **Формула balanced_teams** (`MOD\Server\BotSpawner.lua:556–586`): `targetCount = 8 + floor((maxHumansInOneTeam - 1) * 1.6 + 0.5)`; цель на команду = `targetCount / (NrOfTeams/2)` (две команды: = targetCount); команда человека × 1.0. Счёт команды `s_TeamCount = боты(активные) + люди + s_BotsToDelay` (462–464, 473). При одном человеке: 8 ботов в пустой команде, 7 в команде человека, итого 15 ботов + человек (совпадает с ожиданием пользователя).
- **Смена команды человеком** (движок меняет `+0x31C` человека → мост ставит `p.teamId` → balance-цикл раз в 2 с, BotSpawner.lua:116–119):
  - команда, которую человек покинул: недобор → `SpawnWayBots(n, …, team)` (582) → `_SpawnSingleWayBot` (1286) → `GetBot` → `CreateBot` → `PlayerManager:CreatePlayer` (новый игрок-бот, BotManager.lua:842–896);
  - команда, в которую человек пришёл: перебор → `KillAll(n, team)` (584) → `BotManager:KillAll` идёт с конца списка команды (BotManager.lua:939–959) и вызывает `Bot:Kill()` (Bot.lua:456–465; строка 463 `soldier:Kill()` — совпадает с логом «убит модом: …Bot.lua:463»);
  - `Bot:Kill()` → `ResetVars()` → `_SpawnMode = NoRespawn` (BotSetters.lua:145–146) → `IsInactive()` true (BotGetters.lua:194–200) → `DestroyDisabledBots` (BotManager.lua:989–1006) ставит в очередь → `DestroyBot` → `Bot:Destroy()` → `PlayerManager:DeletePlayer` (BotManager.lua:1054–1095; Bot.lua:467–477).
- **Переиспользование неактивного бота** (`_SpawnSingleWayBot`, BotSpawner.lua:1301–1309): берётся `GetInactiveBot(team)` только той же команды, и команда берётся у самого бота (1300, 1307). Перевода между командами здесь нет.
- **OnTeamChange** (BotSpawner.lua:332–362; регистрация `__init__.lua:117`): для человека `GetBotById(p.id)` = nil, ничего не делает. Для бота с тем же id убивает тело (336–338) и `teamId` пишет Lua-only (339). При `BotTeam ≠ 0` выталкивает человека (342–359). При `BotTeam = 0` (по умолчанию) — ничего.
- **Lua-only записи `teamId`** (то, что не переносит бота в движке на нашем мосте):
  - BotSpawner.lua:339, 356 (OnTeamChange);
  - BotSpawner.lua:540 (keep_playercount, «Move players if needed», только при `SpawnMode = keep_playercount`; пишет и людям, и ботам без солдата);
  - BotSpawner.lua:2337 (`_SwitchTeams`, вызывается из OnLevelLoaded при `SwitchForRoundTwo`, строки 75–81; пишет всем, в т.ч. людям);
  - BotManager.lua:849 (`CreateBot` при повторном использовании имени бота).
- **Респавн живого-мёртвого бота** (`Bot:_UpdateRespawn`, Bot.lua:547–559; вызов из `StateIdle.lua:28`, когда солдата нет): если `_Respawning` и `_SpawnMode ≠ NoRespawn`, после `RespawnDelay` вызывается `TriggerRespawnBot` (BotSpawner.lua:369–407) → спавн того же объекта бота с командой из `p_ExistingBot.m_Player.teamId` (1297–1300) и отрядом, который выбирает `_GetSquadToJoin` (1316–1324).
- **Отряд при спавне**: `GetBot` ставит `SetSquadLeader(true)` первому боту отряда (BotSpawner.lua:724–726; только Lua-флаг). `_GetSquadToJoin` = первый отряд, где `TeamSquadManager:GetSquadPlayerCount < 4` (1764–1776). Человек в этом счёте тоже учитывается (см. 2.2, TeamSquadManager).
- **Режимы, которые могли бы перенести** (`Shared\Constants\SpawnModes.lua`, `TeamSwitchModes.lua`): `keep_playercount`, `increment_with_players`, `fixed_number`, `manual`, `balanced_teams` — все через KillAll/Spawn (BotSpawner.lua:492–700). Единственный «перенос» — Lua-запись teamId (см. выше).

### 2.2 Мост (legacyext, текущий исходник)

- **Player — обычная таблица** (VextPlayerManager.cpp:105–124): `Player.__newindex` делает `rawset` (кроме штамповки `_native` у `input`/`nativePtr`). Поле `teamId` создаётся в конструкторе (605), поэтому его запись не проходит через `__newindex`. Lua-запись `teamId` в движок не попадает.
- **Синхронизация команды** (VextPlayerManager.cpp, раз в 0,5 с: 1645–1647):
  - человек (1719–1740): `+0x31C` из движка ≠ `p.teamId` → `p.teamId = ht`, `Player:TeamChange`; отряд из движка (`EngineSquad`, 1724, 1730);
  - бот (1741–1766): `+0x31C` из движка ≠ `p.teamId` → `p.teamId = et` (откат Lua-записи), `SetBotSquad`, `ClearBotPos` (1758, «точка была для старой стороны»), `Player:TeamChange` (1761). `Player:TeamChange` → `OnTeamChange` → убийство тела (BotSpawner.lua:336–338).
- **Синхронизация отряда бота** (1770–1787): движковый отряд приводится к `p.squadId` через `JoinSquad` (addToSquad) раз в ~2 с на бота (`_sqAt`, `syncTicks`).
- **Спавн бота** (VextPlayerManager.cpp:4898–4902): `ApplyBotKit` → `RegisterBotTeam` (`addToTeam(ptr, team, squad=1, flag=0)`, 3493–3524) → `RegisterBotPoints` → `AssignBotSquad` (addToSquad по выбору мода, 3852–3878) → `ApplyBotPositionOrQueue`.
- **Прямая запись `+0x31C` при создании** (2970–2971): `*(int*)(p + kOffPlayerTeam) = team` (kOffPlayerTeam = 0x31C, 2553). Это прямой доступ к памяти, мимо сеттера движка (см. 2.3).
- **Отряд человека**: `DetourAddToSquad` (SpmProbe.cpp:1206–1215) запоминает отряд человека (`g_humanSquad`, 947; `LegacyEngineHumanSquad`, 2998).
- **SquadFollowEnabled** (VextPlayerManager.cpp:4242–4250, по умолчанию выкл.): при смене отряда человека (8017) и стабильной команде 3 с (8018) вызывает `CallAddToSquad(bot, отряд человека)` для **всех** ботов команды человека (8023–8034). Это единственный в мосте механизм «боты за человеком».
- **TeamSquadManager** (VextTeamSquadManager.cpp:40–59; VextPlayerManager.cpp:7375–7382): `GetSquadPlayerCount = боты (Lua: onlineId==0, teamId, squadId) + люди (движок: +0x31C и EngineSquadOf)`.
- **Player:SetSquadLeader** (572–573): только Lua-флаг `isSquadLeader`. Движковый лидер не выставляется (API-GAP.md:43).
- **NetEvents**: мост доставляет только события, которые сам формирует: `Client:RequestSettings` (1072) и `Botmanager:RaycastResults` (1945) через `NetEvents._Deliver` (VextNetEvents.cpp:114). Для `Client:RequestSettings` мост эмулирует клиента мода (1037–1073), то есть реальные клиентские сообщения не приходят. Клиентские NetEvent из мода (`BotEditor`, `UI_Request_CommoRose_Show`, `UI_Request_Open`) в мосте не обрабатываются (grep по src: нет).

### 2.3 Движок: addToTeam, addToSquad, setTeam (дизассемблер и декомпиляция, read-only)

- **addToTeam(игрок, команда, отряд, флаг)**, VA 0x0118EF80 (RVA 0xD8EF80, file 0xD8E380). **[факт]** Четвёртый аргумент — байт-флаг, а не менеджер: в входе `mov bl,[esp+0x14C]` (аргумент 4), он используется в `test bl,bl` и в отладочной строке `addToTeam(%s, %s, %d, %d)`. Противоречие SpmProbe.cpp:1193–1196 («f(игрок, команда, отряд, менеджер)») снято в пользу флага. Комментарий VextPlayerManager.cpp:3486–3491 верен.
- **Тело addToTeam** (декомпиляция `C:\Games\re\squad_decomp.txt:5–118`, сверено с дизасм. 0x118F0C6…):
  1. проверка индекса команды (`Attempted to add to non-existant team`, строки 35–38);
  2. если `flag ≠ 0`: если `count(команда) >= max` → `return 0` (`Attempted to add to full team`, 53–65);
  3. `oldTeam = игрок+0x31C`; `FUN_00E207A0(игрок, команда)` = сеттер команды (строка 67);
  4. если `отряд == 0`: `leaveSquad(игрок, oldTeam)` = `FUN_0118D590` (68–70);
  5. иначе: если у игрока нет отряда — `addToSquad(игрок, …)` (`FUN_0118D8D0`, 81–106); если отряд уже есть — `return 1` без смены отряда (107–117).
- **Сеттер команды FUN_00E207A0** (дизасм, VA 0x00E207A0, thiscall ECX=игрок, 1 аргумент, `ret 4`). **[факт]** Если команда та же — выход. Иначе: вызывается `FUN_008BAF50(команда)`; если у игрока есть солдат (+0x3C4) — виртуальный метод солдата `+0x128(команда)`; затем `FUN_00E20540(old, new)`. Запись `+0x31C` в самом сеттере не видна; в первых 1401 инструкциях `FUN_00E20540` записи вида `[reg+0x31C]` не найдено. **[не проверено]** где именно пишется `+0x31C` и что делает `+0x128` солдата.
- **addToSquad(игрок, отряд, fa, fb)**, VA 0x0118D8D0 (RVA 0xD8D8D0). **[факт, дизасм.]** Если текущий отряд (vfunc `+0x20` через `[0x285BAD8]`) равен запрошенному — путь «уже в отряде», возврат 1 (0118DA0A–0118DAA9). При `отряд == 0` — вызов `FUN_0118C220` и возврат 1 (назначение `FUN_0118C220` не проверено; 0118DAB4–0118DACD). Иначе — проверка лимита и вход (декомпиляция: `squad_decomp.txt:214`).
- **leaveSquad(игрок, команда)**, VA 0x0118D590 (декомпиляция `squad_decomp.txt:128–…`; строка-маркер `leaveSquad(` в 0x0118D64E по MODLOG:5154). Выход из отряда в указанной команде; поиск отряда через `FUN_011754A0` (дизасм. 0x11754A0 — возвращает отряд игрока в команде).
- **Перенос игрока движком** (балансировка, декомпиляция `C:\Games\re\balance_decomp.txt`, функция `FUN_0117EBE0` = `ServerPlayerSquadManager::switchTeams`, VA 0x0117EBE0, MODLOG:1555): строки 292–341. Проверка ёмкости `FUN_00E5F110(команда) < max`, затем `addToTeam(игрок, команда, 0, 0)` (300, 314, 328–329), затем `addToSquad(игрок, отряд, 0, 0)` (341). Это штатный «перенос»: сначала команда (с выходом из старого отряда), потом вход в отряд новой команды. Строка 476: `addToTeam(…, 0, 1)` — вариант с проверкой полноты.
- **Смена сторон раунда** (`FUN_0117EA40` = `ServerPlayerSquadManager::onNewRound`, VA 0x0117EA40; `C:\Games\re\newround_decomp.txt:34, 50`): при флаге из `registerTeams` (VA 0x0118CFB0) всем игрокам (включая ботов) вызывается `FUN_00E207A0(команда % N + 1)` — то есть setTeam. Это причина «движок поменял команду всем ботам 1↔2» на старте раунда (MODLOG:6078–6081, факт по логу: строки «команда бота … в движке сменилась» пачками, например 01:51:08.439, 02:00:12.126, 02:15:11.803).
- **Строки движка** (MODLOG:5153–5154): `addToTeam(` 0x0118F000, `addToSquad(` 0x0118D951, `leaveSquad(` 0x0118D64E, `joinSquad(` 0x0118ECC1/EEB5/919EC.
- **Автобаланс движка выключен**: `vars.autoBalance false` (`BF-Portable\Battlefield_3_Server\Instance\Admin\Startup.txt:9`).

### 2.4 Логи: два события смены команды человеком (`C:\Games\Legacy\LegacyBF3\logs\legacyext.log`)

Событие 1 (21:40):
- 369747 `TRACE addToSquad(человек, отряд 3, 0, 0)` — движок поставил человека в отряд 3 (сигнатура joinSquad + emptySquad=0, см. STATE.md:344–345);
- 369748 `команда человека (KOTuk1) в движке: 1 -> 2` (35.627);
- 369749 `бот BOT_SchmidtdoggTabard убит модом: …Bot.lua:463` (36.186, +0,56 с);
- 369750–369753 `CreateEnginePlayer` → `игрок "BOT_Benjamin" создан в движке … (всего было 16, команда 1)` (36.775, +1,15 с);
- 369787–369790 `ЖИЗНЬ BOT_SchmidtdoggTabard: удалён модом` (38.790) → `удалён в движке (deletePlayer)` (38.808, +2,6 с после убийства);
- 369795 `addToSquad(игрок F1FE0F80, отряд 2, 0, 0) -> 1` (новый бот в отряд).

Событие 2 (21:54):
- 375633 `TRACE addToSquad(человек, отряд 3, 0, 0)` (23.441);
- 375634 `команда человека (KOTuk1) в движке: 2 -> 1` (23.723);
- 375635 `бот BOT_Benjamin убит модом: …Bot.lua:463` (23.758, +0,04 с);
- 375642–375645 `CreateEnginePlayer` → `BOT_KormiXyston создан в движке (всего было 16, команда 2)` (24.063 → 25.972);
- 375684 `бот F1FE2780 вошёл в отряд 2 (выбор мода)` (28.183);
- 375693 `BOT_Benjamin: удалён модом` (28.585) → `удалён в движке (deletePlayer)` (30.042, +6,3 с после убийства).

В логе **не видно**, кто инициировал смену команды человека (игрок или движок). STATE.md:439–440 и MODLOG:5916–5921 фиксируют переключение человека движком (switchTeams) примерно через 30–40 с после деплоя, причина не подтверждена. Это надо учесть при выводах о «смене команды человеком».

### 2.5 VU-доки

- `server\type\player.md`: `teamId` (24, 133–135) и `squadId` (25, 137) без пометки `true`; у `id` (12) и `soldier` (22) пометка `true`, её смысл в доках не описан. Поведения записи `teamId` у живого бота в доках нет.
- `server\event\player_teamchange.md`: `Player:TeamChange(player, team, squad, oldTeam)`.
- `fb\bfserversettings.md`: `autoBalance` (63, 281), `teamSwitchImbalanceLimit` (49, 225) — настройки движка. У нас автобаланс выключен (Startup.txt:9). Если в VU он был включён, движок переносил игроков сам — это вторая гипотеза «перевода» в VU.

### 2.6 Отряд, «бегающий за игроком» (мод)

- **Follow Me — единственный код следования**: `BotManager:CommandBotsToFollow` (BotManager.lua:1637–1687) выбирает до 2 ботов **той же команды** (не отряда) в радиусе 30 м, не в технике и не в состоянии следования, и ставит `_FollowTargetPlayer` (`SetupBotFollowing`, 1716–1719). Вызов: `_onBotEditorEvent` при `request.action == 'follow_me'` (UIServer.lua:88–93). Событие `BotEditor` приходит как NetEvent от клиента (UIServer.lua:34) или как `Events:Subscribe` (35); в `Server\` его никто не диспетчит. Пункт меню «Follow Me» строится в `_onUIRequestCommoRoseShow` (метка UIServer.lua:521). Отмена — UIServer.lua:216 и `CommandBotsToStopFollowing` (1690–1711).
- **Движение следования**: `BotMovement.lua:515` (по умолчанию — путь), 726–800 (следование: трасса по позициям человека, `TraceDelta = 0.35`, точки > 0,2 м; при `_FollowTargetPlayer.soldier == nil` — сброс, 786–788). Вызывается из `StateMoving.lua:39`, то есть только в состоянии движения по пути. `GameDirector.lua:208` не убивает ботов-последователей за «неверный путь».
- **Больше нигде `_FollowTargetPlayer` не ставится** (grep по `Server\`: Bot.lua:224, BotSetters.lua:203, BotMovement.lua:787 — только сброс).
- **Mimic/Mirror** (копирование ввода человека, BotMovement.lua:1154–1176) включается только чат-командами `!mimic`/`!mirror` (Chat.lua:346–359) для статичных ботов (`SetStaticOption`, BotManager.lua:772–785; `SetVarsStatic`, BotSpawner.lua:755/782/815). Чат-команды доходят до мода только если мост шлёт `Player:Chat` (BRIEF: мост этого не делает).
- **Спавн отряда у человека** (GameDirector.lua):
  - `GetSpawnableBeaconOrMate(team, squad)` (1183–1209): маяки и солдаты отряда; `PROBABILITY_SQUADMATE_SPAWN = 20` (1192, 1200);
  - `TrySpawnInVehicle(player, isBot, isSquadMate)` (1221–1253): `PROBABILITY_SQUADMATE_VEHICLE_SPAWN = 70` — бот садится в технику члена отряда, если там есть место;
  - `TrySpawnOnSoldier` (1255–1277): 20 % на узле пути в радиусе 6 м от солдата отряда;
  - `GetSpawnPath(team, squad)` (1297–1345): сначала отряд, потом команда;
  - после спавна (BotSpawner.lua:202–205) бот получает **случайный** путь от маяка/солдата (`SetVarsWay(nil, true, …)`, режим `RespawnRandomPath`).
- **Какие боты «моего отряда»**: `_GetSquadToJoin` (BotSpawner.lua:1764–1776) сажает ботов в первый отряд с < 4 игроками, человек в счёт входит (через движок, `LegacyCountSquadHumans`). Поэтому ботов в отряде человека обычно 3.
- **Что отдаёт мост** (для таких вызовов): `PlayerManager:GetPlayersBySquad` (VextPlayerManager.cpp:763–771), `TeamSquadManager:GetSquadPlayerCount` (VextTeamSquadManager.cpp:53–55), `Player.squadId/teamId` (Lua, синхронизированы с движком: 1730, 1735, 1747–1752), `Player.isAllowedToSpawnOn` (608 — по умолчанию `true`), `Player.soldier`. Отряд человека мост даёт через `LegacyEngineHumanSquad` только для SquadFollowEnabled (8010–8032).
- **Движок** (strings из `bf3_server_Final.exe`, strs2.py): `OrderType_FollowMe` (0x02114444), `UIMessageType_ChatFollowMe` (0x020DDB84), `OrderType_GoGoGo`, `OrderType_RequestRide` и др. — это приказы командира отряда движка. В мосте обработчика приказов нет (grep: `FollowMe|OrderType` в `legacyext\src` — пусто). Поведение AI-игроков по приказам движка не проверено. У AI-игроков движковый флаг `+0x318` (VextPlayerManager.cpp:2972–2981) и `ServerAILocoComponent` (2977–2978, по комментарию).

---

## 3. Гипотезы и как проверить

**H1. Причина расхождения с VU — Lua-запись `teamId` переносит бота в VU, а здесь откатывается.**
- Основание: 2.1 (5 мест записи), 2.2 (rawset, sync 1745–1747), 2.5 (VU-доки без read-only у teamId).
- Проверка на стенде (без VU): записать `teamId` живому боту из теста Lua и посмотреть лог. Ожидаемое: через ≤0,5 с строка `команда бота <имя> в движке сменилась: a -> b` (1759–1760) и `Player:TeamChange`, т.е. откат. Если откат есть — подтверждает, что в нашем мосте запись не работает.
- В VU проверить невозможно; это гипотеза, не вывод.

**H2. В VU мог работать автобаланс движка (переносит игроков и ботов сам).**
- Основание: `vars.autoBalance false` у нас (Startup.txt:9), в VU-докách есть `autoBalance` (fb\bfserversettings.md:63).
- Проверка: в VU-настройках сервера (если есть доступ) — значение `autoBalance` по умолчанию. У нас не воспроизводится, пока выключен.

**H3. «Бегают со мной» — спавн отряда у человека + техника (20 % и 70 %).**
- Основание: 2.6 (GameDirector.lua:1192–1200, 1222, Registry:220, 222; `_GetSquadToJoin` 1764).
- Проверка: тестовый лог мода (Debug.Server.BOT, `m_Logger:Write` в GameDirector.lua:1193, 1201, 1250, 1265 — пишется только при включённом Debug BOT; проверить Shared\Debug.lua). Визуально: доля ботов отряда, появившихся у солдата человека за N спавнов.

**H4. «Бегают со мной» — движковый приказ «Follow Me» (OrderType_FollowMe) для AI-ботов отряда.**
- Основание: строка `OrderType_FollowMe` и `UIMessageType_ChatFollowMe` в бинарнике (2.6); AI-флаг +0x318 у ботов (VextPlayerManager.cpp:2972–2981).
- Проверка: человек подаёт «Follow Me» через командное колесо движка (не мод); наблюдать, идут ли боты его отряда за ним при `Mode` пути. Лога нет (моста нет обработчика приказов) — только визуально.

**H5. «Бегают со мной» — командное колесо мода «Follow Me» (CommandBotsToFollow).**
- Основание: UIServer.lua:34, 88–93, 521. Действие `follow_me` приходит клиентским NetEvent `BotEditor`; в мосте нет маршрутизации клиентских NetEvent (VextNetEvents.cpp содержит только `_Deliver`, который вызывает сам мост). Значит, по коду маловероятно. Если пользователь всё же нажимал «Follow Me», в логе этого не видно: `ChatManager:SendMessage` в лог не пишется (в legacyext.log нет строк «following you» / «stopped following»). Спросить пользователя.

**H6. Включён `LEGACYEXT_SQUAD_FOLLOW=1` на стенде.**
- Проверка: переменная окружения стенда (скрипты запуска `trash\2026-10-13\start_run.ps1`, `trash\2026-10-11\run_bare.ps1` — только чтение, не запускать). Если включена, при смене отряда человека боты его команды переходят в его отряд (лог: `отряд человека = N: в него перешли K ботов команды T`, VextPlayerManager.cpp:8034).
- **[факт по логу]** Строки `отряд человека = … в него перешли` есть 10 раз, все в интервале 00:26:58–01:11:58 (ранние прогоны, до сборки 78; с 78 перенос выключен по умолчанию, MODLOG:5995). В диапазоне 21:40–21:54 (сборка 155) их нет. Значит, в прогоне 155 перенос, скорее всего, выключен. Вывод по переменной — после проверки окружения запуска.

---

## 4. Опровергнуто

- **[опровергнуто]** «Мод переводит бота в другую команду, когда человек меняет команду». Мод создаёт нового и убивает старого (BotSpawner.lua:582–584; лог 369749–369795, 375635–375693).
- **[опровергнуто]** «Запись `Player.teamId` из Lua меняет команду движкового бота». Это rawset (VextPlayerManager.cpp:115–124); мост откатывает запись в течение 0,5 с (1745–1747).
- **[опровергнуто]** «Есть режим или настройка перевода бота». SpawnModes и TeamSwitchModes (`Shared\Constants\*.lua`) не делают переноса; единственные Lua-записи — см. 2.1.
- **[опровергнуто]** «Мод сам ведёт бота за отрядом или за игроком». Следование — только по `CommandBotsToFollow` (BotManager.lua:1637, 1718). Mimic/Mirror — только по чат-командам для статичных ботов (Chat.lua:346–359; BotManager.lua:772–785).
- **[опровергнуто]** «Расхождение в описании addToTeam: 4-й аргумент — менеджер» (SpmProbe.cpp:1193–1196). Дизасм.: 4-й аргумент — байт-флаг (2.3).
- **[факт, без эффекта]** `g_BotStates.States.Following` (BotManager.lua:1655) нигде не определён (grep по `Server\`), условие всегда истинно. На работу команды не влияет: `CommandBotsToStopFollowing` вызывается раньше.

---

## 5. Предлагаемая реализация (перевод бота; флаг по умолчанию ВЫКЛ)

Цель: при смене команды человеком перевести существующего бота, а не создавать нового. Суммарно 15 ботов + человек.

### 5.1 Мост (native, `VextPlayerManager.cpp`)

- Флаг: `LEGACYEXT_BOT_TRANSFER=1` (по умолчанию выкл.). Чтение как в `SquadFollowEnabled()` (4244–4250); экспорт в Lua глобалом по образцу `LEGACYEXT_EARLY_HOST` (8245–8250).
- Нативная функция (только в тике хука, по образцу `RegisterBotTeam`, `__try`/`__except` как в 3514–3523):
  `bool TransferBotTeamNative(unsigned long ptr, unsigned int team, unsigned int squad)`:
  1. проверить, что ptr — бот мода (`g_watchBot`), `team` ∈ {1, 2}, `squad` ∈ 1..32;
  2. проверить ёмкость: `FUN_00E5F110(team) < max(team)` (как в балансировке движка, balance_decomp.txt:297–299); иначе вернуть 0;
  3. если есть солдат — убить тело **до** смены команды (нативный kill, как в логе 369754 «бот … убит (ServerSoldierEntity::kill)»);
  4. `addToTeam(ptr, team, 0, 0)` — VA 0x0118EF80, RVA 0xD8EF80, cdecl, 4 стековых аргумента (сеттер команды и выход из старого отряда, см. 2.3);
  5. `addToSquad(ptr, squad, 0, 0)` — VA 0x0118D8D0, RVA 0xD8D8D0 (`AssignBotSquad`, 3852–3878);
  6. `ClearBotPos(ptr)` (6040);
  7. лог: `Bridge: перевод бота <имя>: команда a -> b, отряд N (LEGACYEXT_BOT_TRANSFER)`.
- Lua-сторона (в том же тике, чтобы sync 1745–1747 не откатил): `NativeBridge.TransferBot(ptr, team, squad)` → при успехе сразу `rawset` `p.teamId`, `p.squadId`. Возвращает 1 / 0 (полон или ошибка) / -1 (не бот).
- Отказ от записи `teamId` через `__newindex` не предлагается: поле существует в таблице, перехват потребует прокси-таблицы (отдельная задача).

### 5.2 Мод (Lua, `BotSpawner.lua` → `UpdateBotAmountAndTeam`, балансировка, 556–586)

- Если `LEGACYEXT_BOT_TRANSFER` включён и есть одновременно избыток в команде B и недобор в команде A:
  - для `n = min(избыток, недобор)` выбрать живого бота команды B (не в технике, с солдатом, с конца списка);
  - вызвать `NativeBridge.TransferBot(ptr, A, squad)`, `squad` = `_GetSquadToJoin(A)`;
  - `m_BotManager:RefreshTables()`; остаток — старым путём (`KillAll`/`SpawnWayBots`).
- Не вызывать `Bot:Kill()` (он выставит `NoRespawn`, 145–146). Тело убивать `soldier:Kill()` (как в OnTeamChange, 336–338) — тогда `_UpdateRespawn` (Bot.lua:547–559, из `StateIdle.lua:28`) сам посадит бота в новой команде, если `_Respawning` и режим пути.
- Для ботов с фиксированным путём (`_Respawning = false`, BotSetters.lua:36) вызвать `TriggerRespawnBot(bot)` вручную (BotSpawner.lua:369).
- `OnTeamChange` (BotSpawner.lua:332) для переведённого бота повторно убьёт тело. Либо глушить `Player:TeamChange` для переноса, либо принять повторное убийство.

### 5.3 Риски

1. **Побочные эффекты сеттера движка** (`FUN_00E207A0` → `+0x128` солдата, `FUN_00E20540`) при живом солдате не расшифрованы. Поэтому тело убивать до смены команды (п. 3 выше).
2. **Ёмкость команды**: `addToTeam` с флагом 0 не проверяет полноту (2.3). Проверка `FUN_00E5F110` обязательна.
3. **Рассинхрон**: если движок переведён, а Lua нет (или наоборот) в разных тиках, sync (раз в 0,5 с) откатит Lua и пошлёт `Player:TeamChange` → `OnTeamChange` убьёт тело снова (1745–1762, BotSpawner 336–338). Обновлять оба в одном тике.
4. **Отряд**: `addToSquad` может вернуть 0 (отряд полон, в движке 4 места). Тогда бот без отряда; счётчики `TeamSquadManager` (VextTeamSquadManager.cpp:40–51) и синхронизация отряда (1770–1787) должны остаться согласованными.
5. **Падение сервера**: вызовы движка вне тика хука (контекст TLS) падают (STATE.md:340, «вне хука TLS пуст»). Только в тике; `__try` на каждом вызове.
6. **Ники и иконки**: в 154–155 ники зависят от ИИ-флага и времени выдачи ввода (MODLOG:6296–6297). После перевода возможна регрессия ников/значков. Проверить на стенде.
7. **Не трогать** `Player.__newindex`, `CreateBot`, `KillAll` без отдельного теста: они используются в других режимах.

---

## 6. Что проверить на стенде (строки лога в `legacyext.log`)

Все пункты — при прогоне по процедуре пользователя (без автодислокации, см. правило 19). Не запускать без согласия.

1. **H1 (откат Lua-записи)**: после записи `teamId` живому боту — строка `команда бота <имя> в движке сменилась: a -> b` (VextPlayerManager.cpp:1759–1760) и `Player:TeamChange` в пределах 0,5 с.
2. **Текущее поведение при смене команды человеком** (эталон): цепочка из 2.4 — `TRACE addToSquad(человек, отряд N, 0, 0)` → `команда человека (…) в движке: a -> b` → `убит модом: …Bot.lua:463` → `CreateEnginePlayer` → `создан в движке … (всего было 16, команда …)` → `удалён модом` → `удалён в движке (deletePlayer)`. Сумма: 15 ботов + человек.
3. **Новый режим (если включён `LEGACYEXT_BOT_TRANSFER=1`)**: строка `перевод бота …` вместо пары «создан в движке / удалён в движке» для одного бота; число ботов остаётся 15; `Bridge: бот %p вошёл в отряд %d (выбор мода)` (3874) для переведённого.
4. **Отряды**: `Bridge: бот %p не вошёл в отряд %d (движок ответил %u)` (3876) — должно быть пусто; `отряд человека = N: в него перешли` (8034) — должно быть пусто, если `LEGACYEXT_SQUAD_FOLLOW` не включён.
5. **Кто переключил человека**: время от деплоя (STATE.md:439: 30–40 с) и действия игрока — по логу не различимо, фиксировать вручную.
6. **H3 (спавн у солдата отряда)**: лог мода `spawn at squad-mate` / `spawn at beacon` (GameDirector.lua:1201, 1193) — только при Debug BOT.
7. **H4/H5 («бегают»)**: визуально, с отметкой времени; в лог мода «following you» не пишется (2.6).
8. **Ники после перевода** (MODLOG:6296–6297): проверить наличие ников у переведённых ботов.

---

## Ограничения и открытое

- Не проверено: поведение записи `teamId` в VU (VU-сервера нет); семантика `+0x128` солдата и `FUN_00E20540`; точное место записи `+0x31C` в сеттере; тело `FUN_0118D590` (leaveSquad) и `FUN_011754A0`; движковое поведение AI-ботов по приказам `OrderType_FollowMe`.
- Не проверено на стенде: все пункты раздела 6.
- Не выяснено по логу: кто инициировал смену команды человека в 21:40 и 21:54 (игрок или движок).
- Файлы анализа: `C:\Temp\claude\C--Games\c4657c58-f592-45b8-8608-819ca88fc837\scratchpad\8\` (только вывод дизасм. и strings; в репозиторий ничего не записано, кроме этого отчёта).
