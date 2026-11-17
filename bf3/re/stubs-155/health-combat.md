# Трек 4 — оживление, зоны боя, StatEvent, LifeCounter, Level:Destroy, статистика человека

Агент (haiku) 15.10.2026; файл записал оркестратор из сообщения агента (агенту запись .md была запрещена).
Временные скрипты агента: `C:\Temp\claude\C--Games\c4657c58-f592-45b8-8608-819ca88fc837\scratchpad\4\` (find_refs.py, dump_sites.py, dump_branches.py).

## 0. Вывод
1. Статистика в наборе стенда (`C:\Games\Legacy\LegacyBF3`, бэкенд Legacy.Backend) НЕ пишется: нет обработчика GameReporting (компонент 0x1C; команды 1, 100 submitTrustedMidGameReport, 101 submitTrustedEndGameReport) — отчёты сервера получают пустой OK. progress.json KOTuk (personaref 234471775) не менялся с 06.10 17:19Z: rank 2, kills 41, score 1373.87. Уровень 2 — это сохранённое значение. Уверенность высокая.
2. Старый бэкенд BF3LegacyLan.Backend (исходник `re\appsrc`) отчёты принимает и пишет: лог 15.10 14:10:28 «MidGame 26 applied», kills 41→45, score 1373→1510, но rank остался 2. Гипотеза: rank сервер шлёт только в EndGame (101), которого в логах нет. Уверенность средняя.
3. Пять событий мост не шлёт (подписок/рассылки нет). Мод ждёт: StatEvent CrateArmed/Disarmed (Rush, MCOM); HealthAction только OnRevive (задержка принятия оживления 50–300 мс, низкий риск); CombatArea Deserting/Returning (инверсия пути атакующих); BaseDestroyed (пересчёт целей Rush); Level:Destroy (очистка при смене уровня).
4. Точки входа в движке найдены (обработчик gameplay-сообщений 0x011DB670, revive-обработчик 0x00DE1E30, PointSystem::addPoints 0x0131CF90). Адреса хуков OnRevive, updateDeserters, updateDestroyLevel и поле LifeCounter не подтверждены.
5. Порядок: порт GameReporting в Legacy.Backend → события CombatArea/BaseDestroyed через 0x011DB670 → StatEvent и Level:Destroy.

## 2. Факты
### 2.1 Мод fun-bots
- `Server\__init__.lua` RegisterEvents 104–139: Level:Destroy (111), Soldier:HealthAction (123), ScoringSystem:StatEvent (134), CombatArea:PlayerDeserting/Returning (136–137), LifeCounter:BaseDestroyed (138).
- OnScoringStatEvent 270–280: только CrateArmed (86) → GameDirector:OnMcomArmed; CrateDisarmed (87) → OnMcomDisarmed; CrateDestroyed (88) закомментирован.
- OnCombatAreaDeserting/Returning 282–288 → GameDirector.lua:474–481 ToggleDirectionCombatZone (только Team1, _InvertPathDirection).
- OnLifeCounterBaseDestoyed 290–292 → GameDirector.lua:529–531 `_UpdateValidObjectives()`; isFinalBase не используется.
- OnLevelDestroy 375–381: очистка BotManager, BotSpawner, NodeEditor, AirTargets, GameDirector. Клиент: Client\__init__.lua:68, 128–134.
- OnSoldierHealthAction 470–475 → BotManager.lua:229–236: при OnRevive (7) `SetActiveDelay(0.05–0.3)`.

### 2.2 VU-доки
- Soldier:HealthAction(soldier, action: HealthStateAction); HealthStateAction 0..9 (OnRevive=7) — VextEnums.cpp:1369–1371 совпадает.
- CombatArea:PlayerDeserting/Returning(entity, player); LifeCounter:BaseDestroyed(lifeCounter, isFinalBase: bool).
- ScoringSystem:StatEvent(player, objectPlayer|nil, statEvent, paramX|nil, paramY|nil, value: float); CrateArmed=86, CrateDisarmed=87, CrateDestroyed=88 (VextEnums.cpp:483–485); Revive=63, ReviveAccepted=66, ManDownRevive=69.
- Level:Destroy() без параметров.

### 2.3 Бинарник (capstone)
- Хеши сообщений (`re\temp-archive\19a41ba6-...\hashes.txt`): Deserter 0x387C1E27, DeserterReturn 0x8D77BB8D, PlayerBaseDestroyed 0x0E585CA4, PlayerRevive 0x25B72903, ReviveAccepted 0x942685A2, ReviveRefused 0xD6BF76B5, ManDownRevived 0x69596737, SoldierManDown 0xBF3BF771 (imm в коде не найден).
- 0x011DB670 — обработчик gameplay-сообщений: `push ebp; mov ebp,esp; and esp,-16; sub esp,0x24; push ebx; push esi; mov esi,ecx; test byte [esi+0x120],1; push edi`. msg=[ebp+8], тип=[msg+8]. cmp 0x0E585CA4 @0x011DB691 → 0x011DB753 (BaseDestroyed); cmp 0x8D77BB8D @0x011DB6A2 → 0x011DB70C (DeserterReturn); cmp 0x387C1E27 @0x011DB7B5 → 0x011DB839 (Deserter).
  - BaseDestroyed: регистрация типа (call 0x005306C0), `mov edi,[edi+0x30]`, jne 0x011DB78C (не разобран).
  - DeserterReturn: `mov edi,[edi+0x28]; push edi; mov ecx,esi; call 0x011D3710` (индекс, −1 — нет; не разобран).
  - Поля [msg+0x28], [msg+0x30] — указатели, тип НЕ подтверждён.
- 0x00DE1E30 (death\k1.txt:4): cmp 0x942685A2 @0x00DE1E7D; cmp 0x25B72903 @0x00DE2445; cmp 0xD6BF76B5 @0x00DE216B. 0x00DCC160 — только регистрация (опровергнуто как логика оживления).
- 0x0131CF90 — fb::PointSystem::addPoints (подтверждено начало: `sub esp,0x1AC; push ebp; mov ebp,[esp+0x1B4]`), связь со StatEvent не подтверждена.
- Второй диспетчер тех же хешей около 0x0120AED0 — не разобран.
- Не найдены: ServerCombatAreaTriggerEntity::updateDeserters, Server::updateDestroyLevel, ServerLifeCounterEntity, ServerStatEventTriggerEntity::event/sendEvent, SimpleStateMachine<serverHealthModule>::perform.
- Вторая колонка method_index.txt — НЕ адрес и не размер.

### 2.4 Мост
- В legacyext\src нет подписок/рассылки этих пяти событий; есть только enum в VextEnums.cpp.

### 2.5 Legacy.Backend (`C:\Games\Legacy\backend\src\Legacy.Backend\`)
- Диспетчер BlazeServices.cs:92–119: Util, Authentication, Stats (100), Redirector (104), UserSessions (108), GameManager (112); прочее — Unknown (140–147: DBG «… не реализован», пустой OK).
- Stats: только GetStatGroup (4) и GetStatsByGroupAsync (0x10). Обновления нет. UserSessions — только UpdateNetworkInfo.
- GameReporting 0x001C объявлен (BlazeComponents.cs:21, 33), обработчика нет.
- Запись progress.json — только CloneProgress (ProfileStore.cs:315–323 → SaveProgress 295–307): при входе без статов, CreateBot, HTTP-клон.
- Хранение — файлы profiles\<personaref>\{profile,progress,next-id}.json; журнала отчётов нет.
- Выдача: StatsCatalog.cs:152–169 (1432 стата, F2, нет значения — 0.00); rank — WebHost.cs:840.
- BLASE-FIELDS.md: раздела GameReporting нет — имена полей брать из старого кода/захвата, не выдумывать.
- TdfCodec.cs:292–296: сервер присылал GameReporting 0x1C/0x64 (723 Б) — отчёты до бэкенда доходят.

### 2.6 Старый BF3LegacyLan.Backend (`C:\Games\re\appsrc\BF3LegacyLan.Backend.Blaze\`)
- GameReportingService.cs:31 регистрирует (28,1), (28,100), (28,101); ApplyReportAsync 49–70 (проверка IsDedicatedServer 54–58); ApplyToPersonaAsync 72–: AppendReportAsync (85), StatMergePolicy.Plan (97, класса в re\appsrc нет), UpdateStatsAsync (107).
- Логи: backend-20261015-140554-816.log:182–184 — 14:10:28 MidGame 1490B, 26 applied, dropped not in catalog 48; 11.10 — 4 applied / 0 applicable. submitTrustedEndGameReport (101) — ни разу.
- progress.json BF3LegacyLan\profiles\234471775: updatedUtc 2026-10-15T02:10:28Z, c___k_g 45, c___sa_g 1510.40, rank 2. Каталог: rank mode max (5631–5634).

### 2.7 Стенд
- run_bare.ps1:25 — $Kit = C:\Games\Legacy\LegacyBF3, бэкенд Legacy.Backend, stdout → trash\2026-10-11\g.out (перезаписывается при каждом старте; лог бэкенда прогона 155 не сохранился).
- backend-live-NN.log (до 41, 13.10): GameReporting — 0 совпадений; getStatGroup/getStatsByGroupAsync есть.
- Часовой пояс UTC+12.

## 3. Гипотезы
- H1 (высокая): потеря статистики = нет GameReporting в Legacy.Backend. Проверка: сохранить g.out до старта; искать «GameReporting cmd=100|101 … не реализован»; updatedUtc progress.json не меняется.
- H2 (средняя): rank (mode max) приходит только в EndGame (101). Проверка: довести раунд до конца на старом наборе или декодировать пакет MidGame.
- H3 (средняя/низкая): часть статов отбрасывается каталогом («dropped: not in catalog»).
- H4 (средняя): клиент берёт rank из getStatsByGroupAsync — сверить после порта.

## 4. Опровергнуто
- «Бэкенд не отдаёт статистику при входе» — отдаёт.
- «Старый бэкенд не пишет» — пишет (14:10:28, 26 applied).
- «Ранг потерян клонированием 15.10» — статы изменил MidGame, rank остался 2.
- «Мост шлёт эти пять событий» — нет.
- «0x00DCC160 — логика оживления» — регистрационный хелпер.
- «method_index.txt даёт адреса» — нет.

## 5. Предлагаемая реализация (ничего не сделано; флаги по умолчанию ВЫКЛ)
- P1 (статистика, бэкенд): порт GameReportingService в Legacy.Backend — case Component.GameReporting для 1/100/101, проверка выделенного сервера, поля отчёта задокументировать по захвату/GameReportReader.cs, журнал сырых пакетов, UpdateStats по mode каталога (max для rank). Флаг `--progress on|off` (код — off, скрипт стенда — on). Риски: запись в profiles (бэкап до прогона), завышение статов.
- P2 (события): перехват 0x011DB670 (первые 6 байт `55 8B EC 83 E4 F0`, правило 18 — сверка, всё-или-ничего) для CombatArea и LifeCounter; сначала расшифровать [msg+0x28]/[msg+0x30]. Флаг `LEGACYEXT_ENGINE_EVENTS=1`.
- P3 (StatEvent): проверить addPoints 0x0131CF90 на id StatEvent (0x56–0x58).
- P4 (Level:Destroy): адреса нет; эмуляция в мосте при смене уровня.
- P5 (HealthAction OnRevive): отложить.

## 6. Что проверить на стенде
- g.out (копия до старта): «GameReporting cmd=100: компонент GameReporting не реализован».
- `C:\Games\Legacy\LegacyBF3\profiles\234471775\progress.json` updatedUtc до/после (сейчас 2026-10-06T17:19:05Z).
- После P1: «submitTrustedMidGameReport PID 234471775: MidGame N applied», изменение kills/score/rank; «submitTrustedEndGameReport» в конце раунда.

## Открытый вопрос
Присылает ли сервер rank в MidGame или только в EndGame (101) — от этого зависит, хватит ли P1 для уровня 3.
