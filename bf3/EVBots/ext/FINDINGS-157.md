# FINDINGS-157 — находки 9 агентов (15–16.10.2026) и план реализации пачкой

Источник: 9 агентов-исследователей (haiku, max, с согласия пользователя), только чтение. Полные отчёты — `C:\Games\re\stubs-155\`
(`vehicles-enum.md`, `vehicles-enter.md`, `chat.md`, `health-combat.md`, `player-control.md`, `class-badge.md`,
`us-capture-spawn.md`, `squad-follow.md`, `nick-hp-vu.md`). Всё статическое (дизассемблер, код, лог) — на стенде НЕ проверено,
кроме пункта 7 (сделан в 157, не развёрнут). Решение пользователя: реализовать ВСЁ пачкой (флаги), потом ОДИН заход тестов.

## Состояние на 16.10 ≈00:40
- Развёрнута 156 (настоящий dt таймера Lua). Собрана, НЕ развёрнута 157 (пачка ботов, см. п.7), md5 132F10B6…, тест-хост 44/44.
- Прогон 156 (с Host): TESTS-DONE PASS=19 FAIL=2 SKIP=4, VEH 0; ники пользователь видел; у одного бота — значок перка «пулемётчик»;
  боты сразу побежали; «много косяков» — список пользователь ещё не дал (спросить).
- ⚠ Запуск стенда из Claude Code с правами администратора НЕ работает (`activation-loader: refusing to run elevated`).
  Обход: `Start-Process explorer.exe '<scratchpad>\start156.cmd'` (cmd вызывает `start_run.ps1`) или Claude Code без админа.

## ★ Сборка 158 (16.10 00:32) — развёрнута, не прогонялась
Сделано: Host по умолчанию ВЫКЛ (как VU), RCON vars.maxPlayers=64, флаг LEGACYEXT_AI_GUARD=1 (выкл.). Не сделано — всё остальное ниже
(GameReporting, чат, техника, KIT_FLAG, BODY_LEAD_MS, TEAM_MOVE, отряд/таб, 17 из 64, respawn FAIL, vu-weapons) → сборка 159.
Если в прогоне 158 без Host пропадут ники — сначала проверить `LEGACYEXT_AI_GUARD=1`, затем реализовать BODY_LEAD_MS (п.9).

## ★ Прогон 157 (16.10 00:13–00:25, с Host и тестами, пользователь в игре) — НАЧАТЬ НОВУЮ СЕССИЮ С ЭТОГО
Лог (факты):
- Пачка РАБОТАЕТ: мод запросил 14 ботов за 4 с (00:14:04.5–08.4, шаг 0,3 с), мост создал все 14 в одном проходе (00:14:08.456);
  15-й (Colton) — 00:14:30. Lua держит ~30 Гц. VEH 0.
- НО первое тело бота только на 74,9 с (00:15:34), человека — 94,5 с: тела ждут человека (путь Host / «ДО ДИСЛОКАЦИИ»).
  Поэтому пользователь видел «боты странно появлялись, мало».
- Тесты: TESTS-DONE PASS=29 FAIL=1 SKIP=1 (FAIL vu-weapons: оружие видно у 9 из 14). Позже: TEST respawn FAIL BOT_Colton и BOT_Evan
  (через 30 с солдат=0) — возрождение не пришло.
- Смена команды 4 раза (00:17:39, 00:21:24, 00:24:34, 00:24:50): лишний удалён через 3,1–4,4 с (00:17:42 удалён бот без тела ни разу).
Пользователь (со слов, не проверено логом):
1. Таблица (TAB) 8 на 8, а в меню «Отряд» отряд показан забитым: «в табе нас 8, а в отряде якобы 9» — после перехода США→РФ отряд
   не освобождается сразу; при заходе за РФ «долго убирался лишний бот с отряда».
2. Счётчик сервера 17 из 64 (ожидается 16: 15 ботов + человек). Кандидаты: служебный Host в счёте, либо новый бот создан до удаления
   лишнего (3–4 с), либо бот без тела, не удалённый из движка. Проверить.
3. Значки: левые (класс/специализация в HUD) — у всех есть; ПРОБЛЕМА с правыми. В «Отряд» видел 2 значка: США Alpha — медик и
   пулемётчик; РФ Alpha — пулемётчик и инженер (ключ). То есть второй значок у части ботов появился.
4. Ники видит.
5. Вопрос пользователя: в VU работает без Host-игрока — у нас Host оставлен для теста? ОТВЕТ: Host — наш служебный игрок
   (`LEGACYEXT_EARLY_HOST`, по умолчанию ВКЛ с 151, как 136; тесты `LEGACYEXT_TESTS=1` ставит start_run). В VU Host нет.
   Без Host (`LEGACYEXT_EARLY_HOST=0`, прогон 155) ники пропали — лечится п.9 (BODY_LEAD_MS) — цель: работать как VU, без Host.
ЗАДАЧИ из прогона 157 (добавить в пачку): (а) отряд/таб — снятие бота из отряда при удалении (squad не освобождается сразу);
(б) 17 из 64; (в) тела раньше человека без Host (как VU: боты бегают до дислокации) + ники через BODY_LEAD; (г) respawn FAIL;
(д) vu-weapons; (е) правые значки.
Пользователь: в новой сессии — БОЛЬШАЯ пачка, агенты нашли много; всё реализовать разом под флагами, потом один заход.

## Находки по трекам (кратко) и что делать

### 1. Брошенная техника (vehicles-enum.md)
- Взрывает ДВИЖОК: prePhysicsUpdate → abandonCheck 0x00DF9DA0. Мод и мост не участвуют. Таймер [veh+0x250], данные компонента
  d=[[veh+0x10C]+0x14]: ApplyDamage 0x1ED, TUA 0x1C0 (10 с), TUAID 0x1C4 (10 с), KeepAliveRadius 0x1E4 (20/50 м), RespawnRange 0x1D8 (15).
- Таймер сбрасывает: солдат в месте (P2) или солдат в радиусе KeepAlive (P3). Гипотеза H1: боты рядом сбрасывают. H3: у части спавнов
  MP_001 ApplyDamage=False (Base3_RU, Base4_RU, CQ_logic_RU:617 …) — тогда не регрессия.
- СДЕЛАТЬ: пробник `LEGACYEXT_VEH_PROBE=1` (строки «VEH probe: …» раз в 2 с: флаг, таймер, солдат в радиусе).
- Перечисление для мода: TypeInfo ServerVehicleEntity 0x02C17090, vtable 0x025BF8E8; kTypes в VextEntityManager.cpp + обёртка
  (transform, data, entryCount=([+0x100]−[+0xFC])/4, GetPlayerInEntry=[[e+0x4C]...], controllableType data+0xD4) + алиасы
  `ControllableEntity(x)`, `VehicleEntityData(x)` — флаг `LEGACYEXT_VEHICLES=1`.

### 2. Посадка в технику (vehicles-enter.md)
- thiscall, ECX=ServerPlayer: enterVehicle 0x00E37950 (техника, entryId) RET 8 → AL; enterControllable 0x00E35450 RET 8;
  exitControllable 0x00E35AC0 RET 0; leaveControllable 0x00E36090 (a1,a2,a3 байтовые флаги) RET 0xC. STUB-RESEARCH ошибался.
- Вызывать только из DetourSpmUpdate (каждый кадр) через очередь. События: хук enterVehicle (AL=1 и [p+0x3D8]==техника),
  leaveControllable (технику читать до вызова). Блокер — п.1 (технику моду не отдаём).

### 3. Чат (chat.md)
- Вход: handleMessage 0x00E4DAF0 (уже перехвачен `DetourChatMessage`, EngineHooks.cpp:221): M=arg1 (текст UTF-16 [M+0]..[M+4],
  маска [M+0x110]/[+0x114], канал [M+0x118]), S=arg2 (0 у admin.say). Маска: всем −1/−1; команда lo=TeamId; отряд lo=17+33*team+squad.
- СДЕЛАТЬ `LEGACYEXT_CHAT_EVENTS=1`: очередь как PushDmg → `NativeBridge.PollChat` → `Events._DispatchEngine('Player:Chat', player, lo, text)`;
  UTF-16→UTF-8 своей функцией (штатный 0x49C460 режет кириллицу); guard рекурсии.
- Исходящий `LEGACYEXT_CHAT_SEND=1`: ChatMgr = [[0x02B3EE2C]+0x34]+0x3034; ctor M 0x011346B0(M, type, lo, hi, text, 0, 0) RET 0x18;
  handleMessage(ChatMgr, M, 0); только в потоке движка.

### 4. Статистика и события (health-combat.md)
- ★ Уровень 2: в Legacy.Backend НЕТ обработчика GameReporting (0x1C; cmd 1/100/101) — отчёты сервера получают пустой OK
  (BlazeServices.cs:116–147). progress.json KOTuk не менялся с 06.10. Старый BF3LegacyLan.Backend
  (`C:\Games\re\appsrc\BF3LegacyLan.Backend.Blaze\GameReportingService.cs`) это умеет — ПОРТИРОВАТЬ (поля не выдумывать, BLASE-FIELDS.md
  дополнить по старому коду). rank, вероятно, только в EndGame (101).
- События (мост не шлёт): CombatArea Deserting/Returning и LifeCounter:BaseDestroyed — диспетчер 0x011DB670 (хеши 0x387C1E27,
  0x8D77BB8D, 0x0E585CA4; поля msg+0x28/+0x30 не расшифрованы); StatEvent CrateArmed/Disarmed — кандидат PointSystem::addPoints
  0x0131CF90; Level:Destroy — эмулировать при смене уровня; HealthAction OnRevive — отложить.

### 5. Управление игроком (player-control.md)
- Мод из заглушек зовёт только `Player:Kick` (1 раз, BotSpawner.lua:154). Кик ботов не нужен (DeleteEnginePlayer работает).
- ★ RCON-фейк: `vars.maxPlayers` → "100" (VextRcon.cpp:123), у сервера 64 — СДЕЛАТЬ честный ответ (флаг `LEGACYEXT_MAXPLAYERS`).
  Фейковы также vars.playerRespawnTime, modlist.
- admin.kickPlayer 0x01139D50 → ServerConnection::kickPlayer 0x00E48890 (низкий приоритет).

### 6. Значки (class-badge.md)
- Значок КЛАССА — из кита игрока +0x90; движок пишет через setSelectedCustomization (0x8C2660) и ставит флаг репликации 0x40 в
  +0x374 (ветка кита 0xE331B0). Мост (ApplyBotKitClass, VextPlayerManager.cpp:4442) флаг 0x40 НЕ ставит. Перк — набор +0x94,
  флаг 0x20 мост ставит. Но в 136 значок был и без 0x40 — флаг лишь кандидат.
- СДЕЛАТЬ опыт `LEGACYEXT_KIT_FLAG=1` (ставить 0x40 после записи кита). Набор бота: 26 записей (дубль MbtProximityScan), лимит 37.

### 7. Пачка ботов и точки (us-capture-spawn.md) — СДЕЛАНО в 157
- Причина «по одному раз в 2 с»: LegacyBridgeRetryPending раз в 2000 мс (SpmProbe.cpp:529–537), синхронный CreateEnginePlayer ждал
  проход, одна заявка за проход. 157: при `LegacyEngineSpawnPointBest()==0` — сразу очередь (`LEGACYEXT_CREATE_SYNC=1` — старое).
- Lua падал до 6,3 Гц в окне создания (блокировка ~26 %; остальное не найдено) — замерить.
- Спавн на точке: на MP_001 голова пути для спавна только у CP2 («b»); кто владеет CP2 — тот там и спавнится. Асимметрии в коде нет.

### 8. Смена команды и отряд (squad-follow.md)
- Перевода бота в моде нет (KillAll + новый, BotSpawner.lua:556–586); Lua `teamId` мост откатывает за 0,5 с (VextPlayerManager.cpp:1741–1766).
- Перевод в мосте возможен: addToTeam(p, team, 0, 0) 0x0118EF80 → addToSquad; убить тело до смены, Lua и движок в одном тике,
  не через Bot:Kill. Флаг `LEGACYEXT_TEAM_MOVE=1`. Риск средний — последним.
- «Бегают со мной»: следования нет; спавн у солдата человека 20 %, в его технику 70 % (GameDirector.lua:1221–1277).

### 9. Ники (nick-hp-vu.md)
- H9: ник есть, если ввод выдан за десятки секунд до первого тела (154: 57–77 с — есть; 155 без Host: 0,4–2 с — нет; 156 с Host — есть).
- СДЕЛАТЬ `LEGACYEXT_BODY_LEAD_MS=N` (задержка первого тела после ввода, тест 60000) и `LEGACYEXT_AI_GUARD=1` (не писать +0x318=0 при
  отложенной посадке, VextPlayerManager.cpp:8120; перевыдать флаг).
- Ник/HP врага вблизи — штатно BF3 (UINametagComp: 15 м, HealthBarSize 78, ShowEnemyNametagOnAim).

## План сборки 157 (пачкой, всё под флагами, по умолчанию как сейчас кроме уже сделанного п.7)
1. Бэкенд: GameReporting (порт со старого) — уровень/прогресс. Бэкап `profiles\` до прогона.
2. RCON maxPlayers честный.
3. Ники: BODY_LEAD_MS + AI_GUARD.
4. Чат входящий (CHAT_EVENTS), исходящий (CHAT_SEND).
5. Пробник брошенной техники (VEH_PROBE); перечисление техники (VEHICLES).
6. Значок класса: KIT_FLAG.
7. Перевод бота при смене команды (TEAM_MOVE) — последним.
Потом: тест-хост 44/44, деплой (бэкап DLL), ОДИН заход с пользователем (без автокликов, если он на месте).
Перед правкой каждого .cpp — копия `backup\2026-10-16\<Имя>-build157-pre.cpp` (VextPlayerManager уже есть).
