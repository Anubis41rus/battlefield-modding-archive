# HANDOFF — с чего начать новую сессию по EVBots (бывший Legacy)

> ★★★★★★★★★★ **26.10 ≈17:30 — сборка 238-3 (EVBots 32cd34a1…, ev-core 6 2e5297e3…) собрана, НЕ на стенде.** Старт —
> `ext\NEXT-SESSION-2026-10-27.md`. Частота 30/60/120 Гц как у VU (`EvFreq.cpp`; сервер при 30 Гц — без правок; поправки
> физики VU при 60/120 — клиент и двойники сервера), выбор
> частоты в лаунчере, гонка трамплина в ядре исправлена, А1 — тик Lua из кадра сервера за флагом (`run\start-frame.cmd`).
> Собирать из Bash (`cmd //c "…\build.cmd"`), не из PowerShell с UTF-8 (§1). MODLOG «Сборка 238, часть 3».
>
> ★★★★★★★★★ (история) **26.10 ≈13:05 — сборка 238 (EVBots 99b53fdc…, ev-core 6 54fe1a69…) собрана, НЕ на стенде.** Беззвучные
> падения 236-2/237 — `sprintf_s` в моей строке журнала (fail-fast CRT), исправлено + обработчик CRT в обеих DLL. Окно игры
> и Raw Input меню — тоже в ядре (`EvWin.cpp`). Стенд — только по слову пользователя. MODLOG «Сборка 238».
>
> ★★★★★★★★ (история) **26.10 ≈12:50 — сборка 237 (EVBots e6f68937…, ev-core 5 90059401…) на стенде.** Три DLL как у VU:
> перехваты, замена слотов и патчи байтов движка — только `ev-core.dll` (`ev-core\src\EvHook.cpp`, журнал `[hook]`/`[patch]`
> в `ev-core.log`), `EVBots.dll` ставит обработчики через ядро (`InlineHook` — клиент); `lua.dll` — отдельно. Серверная
> эмуляция клиента мода выключена (`EVBOTS_SERVER_CLIENT_EMU=1` — вернуть). Сборка: сначала `ev-core\build.cmd`, потом
> `evbots-dll\build.cmd` (кладёт ядро в `build\` для тест-хоста). Открыто: беззвучное падение сервера (236-2, без дампа —
> WER выключен), порча списка трансформа солдата (236). План/опись — `ext\DLL-SPLIT-2026-10-26.md`, MODLOG «Сборка 237».

> ★★★★★★★ **26.10 ≈05:00 — сборка 234 (50df24eb…) на стенде.** Сделаны заглушки клиента (В2/В4/В6/В8/В12/В17) и
> удаление ботов как VU. **Открыто главное:** сервер падает в `00E0B030` (высадка из техники, испорченный список);
> зацепка — assert «Trying to enter non-spawned vehicle». Старт новой сессии — `ext\NEXT-SESSION-2026-10-26.md`.
>
> ★★★★★★ **26.10 ≈02:00 (история) — прогон 233-б с пользователем:** бот стреляет первым (подтверждено), меню F12 в полноэкранном
> видно, колесо Alt работает, ошибки JS нет; PASS 246 / FAIL 0, VEH 0. Открыто: другу — 233 (по слову); колесо во
> время загрузки; автоклик `cycle.ps1`. MODLOG «Прогон 233-б».
>
> ★★★★★ **25.10 ≈19:40 (история) — сборка 233 (e344710f…), стенд погашен.** Закрыто за вечер (факты — MODLOG «Сборка 231»,
> «Прогон 231», «Прогон 233»): мышь в меню F12 и колесе Alt в игре (Raw Input игры с `RIDEV_NOLEGACY` — снимаем на время
> меню); курсор после колеса (233: окну игры 0x8006/0x8005 — игра ставит свой курсор); меню в полноэкранном — в кадре игры
> (`WebUiPresent.cpp`, перехват Present; по журналу рисуется, глазами не подтверждено); `Player.id` = id движка — номера
> ботов клиент = сервер 12/12, боты агрятся на человека; карты друга MP_012, XP5_002, MP_018 — без падений.
> Открыто: колесо Alt во время загрузки (как у VU — не известно); меню F12 в полноэкранном глазами; другу — 233.
> Не баги (код мода, на VU так же): SETTINGS остаётся после F12; «атаковать» без чата; бледная подсветка колеса.
> Новое для стенда: `run\cycle-keep.cmd`, `run\cycle-crashmaps.cmd`; без игры — `client-harness` (мышь «игры» с NOLEGACY,
> `EVBOTS_WEBUI_INFRAME=1` — swap chain и меню в кадре). Флаги: `EVBOTS_WEBUI_RAWMOUSE=0`, `EVBOTS_WEBUI_INFRAME=0/1`.
> **Правило 25.10:** пользователь сказал «вышел» — стенд и клиент не поднимать, только документы.
>
> ★★★★ (история) **Новая сессия после 25.10 — начать с `ext\NEXT-SESSION-2026-10-25.md`** (меню мода, ввод клиента, номера
> ботов). На стенде 230 + ev-core 4. Прогон 230: номера ботов клиент ≠ сервер — причина «бот не стреляет первым».
>
> ★★★ **25.10.2026 — где мы сейчас** (подробно — STATE «★ СВОДКА 25.10», MODLOG сверху):
> - На стенде последней прогнана **EVBots.dll 227** (866a79c2…): лучи клиента из движка работают, бот видит и убивает человека
>   с нашим клиентом. Стенд остановлен. Другу ничего нового не отдано: друг откатился на сборку до новых DLL (на 221/224 у
>   них «колом у всех» и вылеты, журналов нет).
> - **ev-core 4** (6dfd6b9e…, не развёрнута): отправка канала через очередь и свой поток на соединение (гипотеза «колом»).
>   `start.ps1` развернёт её при следующем старте.
> - Число ботов: лаунчер задаёт RCON-командой мода; у мода это размер команды с людьми. «+1» сделан и откачен («как у VU»).
> - **Следующее — меню F12 fun-bots как у VU** (браузер в игре; у VU — Gameface ≈26 МБ, всё 32-битное; без интернета).
>   Шаг 0 — выбор своего движка (32 бита, вес, отдавать ли другу). MODLOG «Меню F12 — разведка».
>   ★ Проба А прошла (WebView2, вне игры): `EVBots-dev\webui-probe\`, MODLOG «Меню F12 — проба А».
>   ★ **Сборка 228** (7876fc68…, не прогнана): WebUI и ввод клиента как у VU (`src\VextWebUi.cpp`, `src\VextClientInput.cpp`),
>   меню открывает сам мод по F12. Без игры — `webui-probe\build\client-harness.exe <EVBots.dll> <Admin\Mods> 1.png 2.png`.
>   `EVBOTS_WEBUI=0` — выключить. Для меню нужно право мода `UserInterface` (RCON `funbots.Permissions <ник> *` — по решению
>   пользователя). ★ Прогон 228 — меню в игре работает (право выдано). **Сборка 229** (d948bc0e…, не прогнана): меню —
>   дочернее окно игры, ввод игры при меню глушится, окно настроек в экран, Alt. Проверка без игры:
>   `client-harness.exe <EVBots.dll> <Admin\Mods> меню.png настройки.png колесо.png`. Прогон 229: в окне работает, в
>   полноэкранном меню не видно. **Сборка 230** (0a98c3fa…, не прогнана): курсор через win32u, F12 без дребезга,
>   диагностика номеров ботов (клиент/сервер). Полноэкранный — решение пользователя. Следующий прогон: 230.
> - Вопрос пользователя 25.10: частоты 60/120 Гц у VU — это частота сервера (`-high60`/`-high120`), не 64 бита; реестр Г7.
>
> ★★ **Новая сессия после 23.10 — начать с `ext\NEXT-SESSION-2026-10-23.md`** (клиентская часть: что сделано 23.10, где что
> лежит, новые правила, план К5 по шагам, что ждёт решения пользователя). Реестр расхождений с VU — `ext\VU-DIVERGENCES.md`.
> 20.10.2026: проект переименован в EVBots — папки, код, флаги `EVBOTS_*`, лог `EVBots.log`, DLL `EVBots.dll`.
> ★ 23.10 ≈05:05 СМЕНА КУРСА (решение пользователя): сервер — опорная сборка 214 (cc1f15bb), к нему вернёмся позже; **следующий
> шаг — клиентская часть**. К вечеру 23.10 сделаны К1 (разведка, слоты, Ghidra клиента), свой загрузчик, каркас К3 (сборка 215).
> 23.10 ≈04:50: другу — `C:\Games\dist\EVBots-214-dll\` (EVBots.dll 214 + ИНСТРУКЦИЯ.txt, кладётся в `EVBots\activation\`).
> 23.10 ≈04:47: 214 cc1f15bb прогнана (XP5_002 → XP5_004 с пользователем) — ERR 0, PURE CALL 0, Player:Left сразу. Лежит только в
> `evbots-dll\build` и `stand\activation`; у друга и в `dist\EVBots-2026-10-20` — старые (210+/210).
> 23.10 ≈04:30: собрана 214 cc1f15bb…: Player:Left человека сразу (`NativeBridge.LevelChanging()`, второе значение
> `NativeBridge.Humans()` — список прочитан уверенно). 213 прогнана 04:19 — ERR 0, PURE CALL 0. Стенд погашен, MapList возвращён.
> 23.10 ≈04:20: собрана 213 8ede9704… (прогнана 04:19): PURE CALL — `CallSoldierBool` (труп) больше не зовёт компонент с `_purecall`
> в vtable; «ДИАГ неверный путь». 212 прогнана 04:01 (XP5_002 → XP5_004): спавн в самолёт работает, мост легче. Стенд погашен,
> MapList возвращён. Смена карты на лету — `python -I C:\Games\re\rcon\rcon.py "mapList.clear" "mapList.add <карта> <режим> 1"
> "mapList.setNextMapIndex 0" "mapList.runNextRound"`.
> 23.10 ≈04:30: собрана 212 c0b1c70c… (прогнана 04:01): спавн в самолёт на XP5 (солдат моду после посадки движком), оптимизация блока
> «позиции», `EVBOTS_VELSCAN=1` — включить скан памяти «ДИАГ скорость движка» (по умолчанию ВЫКЛ), PURE CALL — `[esp+4]`. На стенде
> прогнана 211 440e0e33 (XP5_002 CQL, 03:29); `MapList.txt` пока XP5_002 CQL, запуск — `run\start-xp5.cmd`.
> Сборка 211: 440e0e33… (23.10 ≈03:30: + диагностика PURE CALL; 22.10 ≈18:00: XP5 самолёты в воздухе, тихий журнал, «ДИАГ вход»), на
> стенде последней прогнана c27fa3ae… (16:38). Журнал DLL по умолчанию тихий — подробный: `EVBOTS_LOG_VERBOSE=1` (стенд ставит сам); у друга и в установщике — a91bd3d2 (210+); что дальше — `PLAN.md` «☐ Осталось сделать».
> 22.10 (211): посадка в занятую технику снимается (тело не ставится в чужой самолёт), ракеты — перехват конструктора ServerMissileEntity
> (ловушки), смена уровня без конца раунда (runNextRound) ловится, лаунчер: «Применить список», «Следующая карта» (endRound),
> «Боты берут самолёты». Подробно — MODLOG «Сборка 211».
> Таблица «было → стало» — `EVBots-dev\README.md`. Ниже старые номера и даты — как были.

(обновлено 19.10.2026 ≈20:27; на стенде 202 (md5 0b632bd5…, несколько людей на сервере, прогон 20:10 PASS); собрана 203 (54f1a678…, не прогнана) — отдана другу как DLL (`dist\legacyext-203\`, класть в `LegacyFriend\payload\activation\`); бэкенд EA8686D6…)

> 16.10 ≈10:50 файл переписан с нуля: устаревшие блоки «(история)», старые скрипты запуска и удалённые флаги убраны,
> чтобы не путать. Прежняя версия — `C:\Games\trash\2026-10-16\docs-archive\HANDOFF-before-170.md` (только как история,
> инструкции оттуда НЕ применять). История по сборкам — `ext\MODLOG.md` (журнал, записи `### Сборка N` выше старых).

Пользователь: русский, на «ты». Цель: **всё как в VU, только локально**. Lua-боты мода fun-bots должны работать без правок мода
(ходят, стреляют, ездят на технике, ники и значки, перки, оружие, модели, спавн как в VU). Нативных ботов нет.
botlayer.dll — только справочник для реверса. API VU делать общим механизмом, как у VU, а не частным нативным обходом.

Читать по порядку: `AGENTS.md` (раздел «Главная цель» и правила) → этот файл → `ext\STATE.md` → `PLAN.md` (верхний блок) → свежие записи `ext\MODLOG.md`. Токены и память — `C:\Games\AGENT-IDEAS.md`; уборка — `ext\CLEANUP-2026-10-17.md`.

## 1. Что это за система
- Сервер `C:\Games\BF-Portable\Battlefield_3_Server\bf3_server_Final.exe` (x86, база 0x400000, без ASLR, RVA = VA−0x400000).
- Наш рантайм (аналог VeniceEXT) — `C:\Games\EVBots-dev\evbots-dll\` (C++, Lua внутри строк `R"LUA(`), DLL `EVBots.dll`.
  Главный файл моста — `src\VextPlayerManager.cpp` (Lua-чанк PlayerManager, натив `NativeBridge.*`, хук тика
  `EVBotsBridgeRetryPending`). Ресурсы как у VU — `src\EngineResources.cpp` (база данных движка, рефлексия, load-хендлеры).
  Лог — поток-писатель `src\EVBotsLog.cpp` (★ 238: при 32 МБ — в один `<имя>-prev.log`, прежний удаляется).
- Сборка: `cmd //c "C:\\Games\\EVBots-dev\\evbots-dll\\build.cmd"` (полный путь, из Bash; сначала то же для `ev-core\build.cmd`).
  ★ 238-3: НЕ из PowerShell с префиксом UTF-8 — консоль 65001, cmd рвёт строки `build.cmd` с кириллицей, `cl` не находится, DLL
  не пересобирается, а тест-хост проходит на старой (ложные 77/77). После сборки сверять время/md5 `build\EVBots.dll`.
  Предупреждения = ошибки (C2220); C4505 — неиспользуемая статическая функция. Вывод MSVC в cp866 — пользователю не
  показывать. Тест-хост `build\EVBots.Test.exe` (норма 77/77 на 238). Lua-чанки тест-хост не компилирует: проверять скриптом luachk
  (lua54 из `C:\Games\trash\2026-10-10\lua54\lua54.exe`, `loadfile` каждого `R"LUA(` блока).
- Лог рантайма: `C:\Games\EVBots-dev\stand\logs\EVBots.log`.
- Мод fun-bots: `BF-Portable\Battlefield_3_Server\Instance\Admin\Mods\fun-bots\ext\` — ТОЛЬКО ЧИТАТЬ. Мод не трогаем никаким способом (AGENTS, «Главная цель»). Код не менялся; отступление — `ext\Shared\Config.lua` (InitNumberOfBots 15 вместо 10), оригинал в `backup\2026-10-17\fun-bots-check\`.
- Бэкенд Blaze: `C:\Games\EVBots-dev\backend\` (C#), поля протокола — `backend\BLASE-FIELDS.md`. HTTP: `http://127.0.0.1:8080/`
  (`/servers` — сервер и число игроков, `/accounts` — персоны).
- Комплект для друга (с 20.10): установщик `dist\EVBots-2026-10-20\EVBots-Setup.exe` (рядом — папка `EVBots\`, из неё
  собран `kit.zip`). Кладётся рядом с `Battlefield 3` и `BF-Portable`, ставит `EVBots\` (EV Launcher, EV Backend, EVBots.dll,
  пустые profiles/logs), одно окно UAC (брандмауэр, URL-ACL 8080). Исходники — `EVBots-dev\launcher\src\EVLauncher\` и
  `…\EVBotsSetup\`; как собрать — `EVBots-dev\README.md`. Старые комплекты LegacyFriend и установка `C:\Games\LegacyFriend\` —
  в `trash\2026-10-20\`.

## 2. Как гонять стенд — только `C:\Games\EVBots-dev\stand\run\` (описание — `run\README.md`)
- Пользователь на месте: `explorer.exe C:\Games\EVBots-dev\stand\run\start.cmd` (из админ-сессии агента стенд иначе не
  стартует: `refusing to run elevated`). start.ps1 сам: останавливает старое, деплоит `build\EVBots.dll` с бэкапом рабочей,
  ждёт `LUA-MODE READY` (видит падение `VEH #`), заводит клиента. «Дислокацию» жмёт пользователь. Вывод —
  `trash\<дата>\run\start-HHmmss.log`.
- `run\status.ps1` — процессы, игроки в бэкенде, VEH/ERR/TEST текущего прогона. `run\tests.ps1` — итог тестов.
  `run\stop.ps1` — гасит сервер и бэкенд (пока запущен клиент — отказ без `-Force`).
- ★ С 19.10 (сборка 201) прогон без человека — `explorer.exe C:\Games\EVBots-dev\stand\run\cycle-modes.cmd`: все режимы за один
  старт (RCON `mapList.runNextRound` после каждого набора тестов), без автоклика; итог — в конце `trash\<дата>\run\cycle-modes-*.log`.
  Только когда клиент не запущен (скрипт сам отказывается).
- Старый вариант с автокликом: `run\cycle.ps1 -UserAway` (закрывает клиент, автоклик «Дислокация», тесты) — пользователь 19.10: «не надо автокликать».
- Сервер не гасить, пока в игре человек; клиент bf3 не убивать без разрешения. Во время прогона пользователя не собирать.
- Старые `trash\2026-10-1x\run_bare/start_run/join_client/finish_run/run_cycle` устарели (лежат в
  `trash\2026-10-16\obsolete-run-scripts\`), не использовать.

## 3. Флаги окружения (код читает только эти; набор для стенда — `run\common.ps1`)
Стенд всегда: `EVBOTS_BRIDGE=1`, `SPM_MGR=1`, `SPM_TICK=1`, `SPM_SOLDIER=1`, `LIVE_SEARCH=1`, `EBX_INDEX=<путь>`,
`TESTS=1` (кроме `start.ps1 -NoTests`).
По умолчанию ВКЛ (выключатель `=0`): `MISSILE_HOOK` (211, ракеты для `EntityFactory:Create` — перехват конструктора ServerMissileEntity), `DLPROC_FILTER` (207, DownloadProcessed ботов), `INPUT_NATIVE`, `EARLY_BODIES`, `TP_REAL`, `BOT_WEAPONS`, `VU_BULLET`, `BOT_DELETE`,
`AI_DEATH_PATCH`, `ENGINE_RES`, `LOAD_HOOK`, `SQUAD_HOOK`, `SPM_CUSTOM`, `PTS_REG`, `SOLDIER_VU`, `POSE_REAL`, `VF13C_HOOK`,
`VEL_HOLD` (172), `EARLY_NOTEAM` (167: тела не ждут команду человека; `=0` — ждать), с 185: `KIT_FLAG` (флаг кита 0x40),
`SELECT_ENGINE` (выбор через обработчик движка 0x00E331B0), `VU_LICENSES` (70 лицензий VU при создании бота),
`ENGINE_SPAWN` (тело — createSoldier+spawnSoldier движка, как VU), `COMBATAREA` (перехват выхода за границу зоны);
с 186: `ROUND_HOOK` (конец раунда из RCON onMessage), `STAT_EVENTS` (ScoringSystem:StatEvent), `CHAT_EVENTS` и `CHAT_SEND`
(чат мода — раньше были выключены).
С 190 `VEH_TELEPORT` снова по умолчанию ВКЛ (`=0` — выкл.): перенос машины через настоящий setter `vtbl+0x5C` (0x00843570) со сверкой
всех звеньев (MODLOG «Сборка 190»). В 185–189 звался +0x6C 0x00D9BB40 — спавн сущности, падение 186. С 188 `TICK_DEADLINE` (тик Lua
по сроку 30 Гц; `=0` — старый сон 33 мс). С 192 `TICK_PRIO` (поток тика выше обычного; `=0` — обычный). С 193 `ORDER_HOOK` (приказы колеса
BF3 → меню мода; `=0` — выкл.). С 187 боты без тела удаляются из движка (leaveSquad перед deletePlayer), флага нет.
RCON сервера для `RCON:SendCommand` / `Player:Kick`: `EVBOTS_RCON_PORT` (47900), `EVBOTS_RCON_PASSWORD` (change-this-rcon-password).
Со значениями: `BOT_AI` — ★ с 175 по умолчанию isAI 0 + перехват vf13C (очки/звания/иконки, ников нет); `=1` — isAI 1
(ники, без очков); `=ab` — опыт А/Б по группам. `VU_CREATE` — 1 (по умолчанию: сообщение создания + LevelLoaded +
расширения) / 0 / `ab`. `MOD_SEL_WAIT_MS` (174) — сколько посадка ждёт выбор оружия мода, 3000; 0 — не ждать.
`BRIDGE_TICK_MS` (172) — тик моста не чаще N мс (по умолчанию каждый кадр).
По умолчанию ВЫКЛ (включатель `=1`): `NOTELEPORT`, `VU_SPAWN`, `WAIT_INLEVEL`, `PREROUND_PATCH`, `ROUND_SWAP`, `KIT_FLAG`,
`AI_GUARD`, `VEH_PROBE`, `MODLOG`, `API_PROF`, `NOHUMANEVENTS`, `SPM_PREPHYS`.
Карта/режим для модов (185): из движка (ServerLevel → LevelSetup, MODLOG «Сборка 185»); `EVBOTS_LEVEL_NAME` /
`EVBOTS_GAMEMODE` — только явная подмена; не прочитано — `Levels/MP_001/MP_001`, `ConquestSmall0`.
Флаги, удалённые в 168–169 (их имена остались только в комментариях): `EARLY_HOST`, `ICON_AB`, `ICON_AB2`, `FIXED_DT`,
`EARLY_NODES`, `AI_PLAYER`, `DUMP_INPUT`, `SPM_SPAWN`, `SPM_POINTS`, `SQUAD_FOLLOW`, `CHOICE_BY_NAME` (выбор по имени
теперь всегда), `NATIVE_ASSETS`, `LOOKUP_AB`, `BODY_LEAD_MS`, `BRIDGE_ONCREATE`, `BOT_CONN_SLOT`.

## 4. Что работает (факты прогонов 166–174 и слова пользователя)
★ 25.10 (прогон 227): клиент считает лучи сам — `RaycastManager:CollisionRaycast` из движка zbf3 (`01199D10`, this =
`[0x023813A0]`, тот же объект, что у VU), попадания в тела ботов, `Botmanager:RaycastResults` клиент → сервер по каналу
ev-core; бот видит и убивает человека с нашим клиентом. Открыто: у друга с 224 зависания и вылеты (MODLOG «Жалоба друга»).
Ходьба/бег по путям мода, захват разных точек; ответный огонь (в том числе по человеку); урон пуль через хук VU
`BulletEntity:Collision`; «ПРОТИВНИК УБИТ» и лента; модели, киты (8/8 из базы движка), перки, оружие с навесами по выбору мода
с первой посадки (174); `ResourceManager:GetSettings` через SettingsManager; load-хендлеры мода; поиск ассетов только через
базу движка; лог без зависаний (поток-писатель, 167); тик Lua ≤0,1 с. Боты за 4–5 с после входа клиента в уровень (172);
смена раунда на реальном конце раунда с телами во 2-м раунде (173–174); после ухода человека 0/64 (170). Очки, звания,
иконки — у ботов с isAI 0 (174; в 175 по умолчанию всем).

Техника (178–183): боты садятся в БМП/Водник/Хамви/LAV сами (мод), ★ ЕЗДЯТ (180: 9–13 м/с, `veh-drive` PASS) и ★ УБИВАЮТ
из техники (183, пользователь). `ToEuler`: x = курс, y = крен вокруг forward, z = −подъём·cos(крен); `internalHealth` из движка
(183, `emVehHealth`). Transform
машины — как в движке (нос = forward, без перестановки); `Quat:ToEuler` под соглашение VU (x курс вокруг Y); `quatToBasis` —
столбцы матрицы (до 180 были строки → курс −ψ). `type()` для кода мода отдаёт "userdata" на Vec3/LinearTransform (как VU;
181). `physicsEntityBase.partCount/GetPartTransform` — из физики машины (182, натив `emPart`, адреса — MODLOG «Сборки 179–182»).
Диагностика: `ДИАГ езда` (ошибка курса), тест `veh-drive`. Флаги: `EVBOTS_VEH_YAW_SIGN=-1` (опыт, не нужен), `EVBOTS_VEH_ENTER=0`.
Замеры памяти — по шаблону `re\peek\peekents.py` (transform = [сущность+0x1C], строки по 0x10, места [+0xFC..+0x100),
physicsEntityBase = [машина+0x144], части — [[peb+0x8C]+4i], число — [peb+0x84]).

19.10 (сборки 187–192, факты прогонов): тик Lua ровно 30 Гц без пауз (dt_max 0,05–0,09 с; heartbeat пишет `hz=`, ожидание лока,
мост/мод в мс, «проснулся позже»); сборщик мусора Lua как у VU (`lua_gc(LUA_GCINC, 105, 200, 16)`, Lua та же 5.4.2 — `vubin\lua.dll`);
перенос застрявшей машины (`vtbl+0x5C`); NaN/inf от мода движку не пишутся (`NonFiniteGuard`); удаление ботов без тела. Езда «намного
лучше» (пользователь); заторы у базы США на старте — пути мода сходятся (MODLOG «Сборки 191–192»).

185 (не на стенде): карта/режим из движка, `player.corpse` и реанимация (`Soldier:HealthAction OnRevive`), `RaycastManager`
(серверный набор VU на очереди лучей) — MODLOG «Сборка 185».

## 5. Что открыто — ★ единый список с галочками: `PLAN.md`, блок «☐ Осталось сделать» (20.10 ≈03:45)
★ 20.10: сборка 207 на стенде и в установщике `dist\EVBots-2026-10-20\EVBots-Setup.exe` (отдаётся другу). Новое за день: вывод мода —
`logs\fun-bots.log`; фильтр `DownloadProcessed` ботов (`EVBOTS_DLPROC_FILTER=0` — выкл.); «ДИАГ магазин больше ёмкости»; бэкенд —
UserAdded серверу при входе (`--no-join-user-to-server`), GOS-достижения выкл. (`--gos-achievements`), `server.xml` на 127.0.0.1:80
(`--no-editorial`); лаунчер — тикеты по режимам через RCON (`RconRules.cs`); стенд `start.ps1` сам ставит `vars.gameModeCounter 30`.
Не запускать стенд и установленный EV Launcher одновременно (одни порты). Ниже — состояние на 19.10 (история).

### 19.10 (история) ★ С ЧЕГО НАЧАТЬ НОВУЮ СЕССИЮ (19.10 ≈11:00; ★ 20:10 — сначала прогон 202, PLAN верх)
★ 19.10 ≈20:10, сборка 202: людей на сервере может быть несколько (как VU) — каждый в списке мода со своим указателем движка
(`EVBotsHumanPtr`, `EVBotsPlayerByPtr`, натив `NativeBridge.Humans()`), onlineId = persona `[игрок+0x20]`; лучи «человек видит бота» —
как клиент мода у каждого человека. `RealPlayer()` — только для диагностики/тестов; новый код на одного человека не писать.

На стенде — диагностика 200 (md5 c821f534…, = 199 + «ДИАГ скорость движка»); у друга — комплект 19d (мост 199, 101d616b…).
Прогоны без человека: `explorer.exe …\run\start.cmd` (без автоклика — боты играют сами), итог — `run\status.ps1`, `run\tests.ps1`,
остановка — `run\stop.ps1 -Force`. Старый `cycle.ps1` с автокликом НЕ запускать (пользователь 19.10: «не надо автокликать»).
1. **Скорость из физики, как VU** (мод: упреждение прицела, парашют, газ самолёта `VehicleJetControl.lua:181`). Сейчас
   `soldier.velocity` и `vehicle.velocity` — производная позиции (`EVBotsVelocityStep`, VextEntityManager `k == "velocity"`).
   ★ Найдено (MODLOG «Сборка 200»): у солдата скорость движка — Vec3 `[[игрок+0x3C4]+0x2B0]` (80 % совпадений). Сделать натив
   чтения и отдавать моду её (производная — запас). У машины поле не найдено — искать в теле Havok.
2. **PURE CALL** — ловится только с человеком в игре: строки «★ PURE CALL» + «солдаты на стеке» (3 из 4 в 197 —
   `ServerSoldierEntity::prePhysicsUpdate` 0x00E0A7C0, деструктор локального объекта 0x0044E840).
3. С человеком проверить: урон ботов по человеку ×0,8 («Soldier:Damage ДО урона»), Gun Game, «за мной» (ввод человека), Yell на экране
   (секунды?), `Player:Killed` inflictor.
4. Остальное (заглушки, по важности): `Player:EnteredCapturePoint` без игрока (нужен список игроков зоны флага у движка);
   `CreateEntitiesFromBlueprint` (nil — XP4_Quake не взрывает препятствия); `Player:Killed` weapon; `LifeCounter` isFinalBase;
   `Player:Joining` guid/ip; `Entity:Is("ServerVehicleSpawnEntity")`; `PhysicsEntityBase:GetPart`; `MathUtils` YPR;
   `SingleStepEntry`, `Fade` — по смыслу не нужны серверу (объяснение в коде); меню мода/NetEvents — клиентский VEXT.
Закрыто 19.10 (сборки 196–199): статистика после смены режима, отряды на смене уровня, replayGame (кик «Game stuck»), перенос машины
без NaN, перки без мусора, `isAllowedToSpawnOn`, чат игроку, `Level:Loaded` round, Soldier:Damage ДО урона (самоубийства гранатой — нет),
Yell/Ban/Spawn/SelectWeapon/EnableInput/AttachSoldier. Лаунчер: режим → карты, общая база аккаунтов (MODLOG 19.10 ≈09:20).
Ещё раньше открыто: реанимация человека ботом; заторы техники на старте; иконки перков у США, значок класса (Q «садись ко мне» —
снято 23.10: пользователь ошибся в объяснении);
магазин сверх ёмкости; maxPlayers >64.

## 6. Заглушки рантайма — `evbots-dll\src`, см. `ext\STUB-RESEARCH.md`, `ext\API-GAP.md`
- С 199 НЕ заглушки: `Player:Ban*` (RCON banList.add), `Spawn`, `SelectWeapon`, `AttachSoldier`, `EnableInput` (у ботов); `Fade` —
  эффект клиента (журнал). EnterVehicle/ExitVehicle — с 178; `Player:Kick` — с 186 через RCON (admin.kickPlayer).
- `SpatialEntity`, `AxisAlignedBox` (VextSpatial.cpp) — нулевые данные; `EntityManager:FindEntityByName` (nil), типы кроме
  CapturePoint/CharacterSpawn/Vehicle/SyncedBool/InputRestriction/LifeCounter/Objective/SupplySphere — пусто
  (VextEntityManager.cpp). `RCON` — с 186 настоящий (Frostbite RCON к 127.0.0.1:47900).
- С 186 не заглушки:
  - `Entity:RegisterEventCallback` — перехват onEvent, слот vtable +0x10;
  - `ScoringSystem:StatEvent` — центральный `ScoringDispatcher::executeEvent` 0x013164F0 (старые источники 0x011749C0 и
    триггер карты 0x012BC7C0 — только при `EVBOTS_STAT_OLD=1`);
  - `EntityFactory:Create` — перехват `createEntity` 0x00DA36E0 (ракеты, ловушки); `SpatialEntity` — прокси над живыми обёртками;
  - `Player:SetSquadLeader` — RCON `squad.leader` с повтором, пока бот не попал в движок;
  - `LifeCounter:BaseDestroyed` — на события счётчика жизней `#2CFC2C6C`/`#324B3FE6` (Rush, прогон 19:08);
  - спавн без точки спавна (в Rush все точки `+0x19C = 0`) — тело на позицию мода, как VU;
  - `Level:Loaded` — по сигналу движка `server.onLevelLoaded` (перехват RCON onMessage ставится до загрузки первой карты),
    не по таймеру; при смене карты — только сигнал после конца раунда;
  - `Vehicle:GetPlayerInEntry` — место занято, пока посадка в очереди моста (`NativeBridge.VehSeatQueued`): у VU
    `EnterVehicle` синхронный, иначе два бота садятся на одно место пилота;
  - `weaponFiring.gunSway` — `[WF+0xA0]`;
  - `Player:ExitedCapturePoint`, `Extension:Unloading`;
  - `MathUtils:FNVHash` — хеш движка djb2-xor.
- С 198 не заглушки: `ChatManager:SendMessage(text, player | {players})` (RCON admin.say … player), `TicketManager:GetCurrentRound/
  GetRoundCount` и `Level:Loaded` round/roundsPerMap (RCON mapList.getRounds).
- Открыто:
  - клиентский VEXT (меню мода на левом Alt; `NetEvents:Send*/Broadcast*` только пишут в лог — VextNetEvents.cpp);
  - `ChatManager:Yell` — с 199 через RCON admin.yell; `Player:SelectWeapon` — мод не вызывает (выбирает оружие вводом);
  - `SoldierEntity:SingleStepEntry` — только поле;
  - зона Rush (`ServerSyncedBoolEntity`, 0 штук; мод только пишет в журнал);
  - `CrateArmed` (86) — ждём из `executeEvent` (прогон ≈20:15).
- Конец раунда и смена уровня (186): `Server:RoundOver` из движка (RCON onMessage, `ServerRoundOverMessage`), затем
  `Level:Destroy` (уровень ушёл) → `Level:Loaded` → `Server:RoundReset`. Пока уровень меняется, ворота спавна закрыты;
  тикеты — запасной путь. Кэши сущностей сбрасываются на `Level:Destroy`. SQDM — команды 1..4.

## 7. Реверс (вывод вне проекта: `C:\Games\re\`)
- `re\death` — смерть ИИ-игрока, патчи (EDIT-MAP H4). `re\capture` — флаги, точки спавна (`ext\ENGINE-ENTITIES.md`).
  `re\ray` — `GameWorld::rayCast`. `re\aim\report.md` — огонь; `re\aim\botlayer-damage.md` — урон. `re\vu\` — разбор
  `veniceext.dll` (биндинги, `SEQUENCE-REPORT.md`, `FIELDS-REPORT.md`), `ext\VU-AUDIT.md`.
- Чтение памяти живого сервера (только PROCESS_VM_READ): `re\peek\`.
- Полный декомпилят `veniceext.dll` (VU) по кускам 8 МБ — `re\vu\decomp\chunkNN.decomp.txt` (делает другой проект
  пользователя, EV: `%LOCALAPPDATA%\VeniceUnleashed\client\EV\`; план кусков `re\vu\veniceext.chunks.txt`, строки
  `veniceext.strings.txt`). Строки и luaL_error в декомпиляте раскрыты; файлы по 25–50 МБ — искать grep'ом по
  `@ 0x<адрес>`, целиком не читать. Таблица движковых функций VU: слоты `[0x18E74xxx]` ← `mov [слот], адрес_bf3.exe`
  (скрипт поиска — см. `re\vehicle\NOTES.md`).
- ★ Свой журнал движка: `BF-Portable\Battlefield_3_Server\Instance\Logs\RuntimeLog_*_server.log` (новый на каждый старт,
  время UTC) — assert'ы с контекстом («Soldier … Player … Weapon …», файл движка и строка) и `Crash:`; рядом минидамп
  `CrashDump_*.dmp` (только стеки потоков; читать `python -m minidump` / пакет `minidump`). Сводку печатает `run\status.ps1`.
  Причина падения 173 (DataBus assert) была только там, в EVBots.log — лишь `VEH #1`.
- Ghidra 12.1.4 headless (`C:\Games\tools\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat`, JDK 21), проект
  `re\ghidra-proj-server`. Часто быстрее capstone+pefile по exe. Правило 18: патчи байтов — только после сверки.
- ★ Ghidra MCP (23.10): `tools\ghidra-mcp\start.ps1` → `mcp__ghidra-server` (bf3_server_Final.exe) и
  `mcp__ghidra-botlayer`: декомпиляция/xrefs/байты из уже открытого проекта без нового analyzeHeadless (старт ~30 с,
  малая функция ~0,1 с, огромная `00833a00` ~7 с). Скилл `ghidra-mcp`. Пока MCP открыт — headless по копии `re\ghidra-jsrv`.
- ★ Клиент (23.10): статика — только по `Battlefield 3\zbf3.exe` (расшифрованный снимок `bf3.exe`: тот же timestamp и
  адреса секций; `.text` у `bf3.exe` зашифрован DRM, энтропия 8,00). Ghidra-проект `re\ghidra-proj-client\bf3client.gpr`.
  Клиентские слоты VU — `ext\CLIENT-SLOTS-2026-10-23.md`, план клиента — `ext\CLIENT-RECON-2026-10-23.md`.
- ★ Свой загрузчик `EVBots-dev\loader\` (`evbots-loader.exe`, 23.10): несколько DLL, клиенту `activation.dll` первой,
  серверу `EVBots.dll`; тест `test\test.ps1`. Стенд (`stand\run\*`) на нём с 23.10 14:03; лаунчеры — тоже (код,
  `EVBots.Core\Paths.FindLoader`, откат на `activation-loader.exe` в старых установках). Комплект `dist\EVBots-2026-10-23`
  и установщик — с ним.
- ★ Реестр расхождений с VU — `ext\VU-DIVERGENCES.md` (23.10): рантайм, сервер, клиент, сеть; правило — новое «не как VU»
  записывать туда же.
- ★ Клиентский режим `EVBots.dll` (сборка 215, К3): в `bf3.exe` — ждёт расшифровки кода, клиентский Lua (`VextClient.cpp` —
  клиентские библиотеки VU, пока заглушки), моды из `EVBOTS_CLIENT_MODS`, журнал `logs\EVBots-client.log`. Чтобы включить на
  стенде: в `run\join.ps1` клиенту `--dll EVBots.dll` (после `activation.dll`) и `EVBOTS_CLIENT_MODS=<Admin\Mods>` после
  очистки `EVBOTS_*`. Тест-хост — раздел «10. клиентский режим».
- Субагенты — только с согласия пользователя (модель haiku).

## 8. VU для сверки порядка событий
VU-сервер подготовлен: `Documents\Battlefield 3\Server` (server.key — секрет, не выводить; `Admin\MapList.txt`,
`ModList.txt` = event-order + fun-bots; мод `Admin\Mods\event-order` пишет все серверные события с метками времени).
Запуск `vu.exe -server -dedicated` упирается в окно активации EA — проходит только сам пользователь (логин/пароль не вводить,
активаторами не обходить). Порты те же, что у стенда: одновременно не запускать.

## 8а. Проверяющий (с 18.10)
Субагент `verifier` (`C:\Games\.claude\agents\verifier.md`, Haiku 5.5 `claude-haiku-5-5` с 18.10, только чтение): перепроверяет правку и выводы —
не выдуман ли адрес/поле/таймер, есть ли строка лога у причины, сделано ли как у VU. Звать после правки моста, перед
сборкой и перед записью вывода в MODLOG/STATE. Аналог «фонового ревью» Hermes (`C:\Games\AGENT-IDEAS.md`), но по запросу.
Подхватывается в новой сессии. С 22.10: у `verifier` — `effort: high`; новый `scout` (`C:\Games\.claude\agents\scout.md`, Haiku,
`effort: low`) — быстрый поиск по логам/MODLOG/исходникам, только чтение. Правила «осмотрись / готово / проверено или догадка» — `C:\Games\CLAUDE.md`.

## 9. Грабли (не повторять)
- **19.10: `sleep_for(33 мс)` в Windows = 46,8 мс** (шаг таймера 15,6 мс) — тик был ~20,7 Гц, а мод считает время кадрами
  (`1/GetTickrate()`), руль PID без dt раскачивался. Тик — только по сроку (таймер высокого разрешения).
- **19.10: наш серверный exe с FatalAssert** (у VU сервер — клиентский bf3.exe). Любой NaN/inf от мода в ввод/прицел/трансформ
  роняет сервер (Ми-28, «Active camera transform is invalid»). Всё, что мост пишет в движок из Lua, — через `NonFiniteGuard`.
- **19.10: слоты vtable у серверных классов сдвинуты на +8 против клиента VU** (SpatialEntity: VU getter/setter +0x50/+0x54 →
  сервер +0x58/+0x5C). Сверять по коду, не по номеру слота.
- **19.10: мета-запись «имя метода» рядом с кодом — не всегда сам метод** (`onSpottingMessage` 0x02C71940 → 0x012C4460 — чужая
  функция). Обработчик сообщения искать по имени типа сообщения (`Venice…Message`) и сверке `[msg+4]/[msg+8]`.
- **19.10: прежде чем звать реверс «с нуля» — `re\`** (декомпилят VU, слоты VU, Ghidra-проект сервера, `squad_decomp.txt` и др.).
- **18.10: не придумывать таймеры и пороги «на глаз»** (пользователь: «откуда ты 3 секунды взял? делаем как у VU»). Ждать
  событие/состояние движка (сигнал, очередь, флаг), как это делает VU. Предел 20 с на ожидание уровня сломал авиацию на MP_007.
- **18.10: слот vtable +0x6C (0x00D9BB40) — спавн сущности, не setTransform.** Для машины в мире — падение сервера.
- Lua моста работает в своём потоке-таймере (под `LuaRuntime::Gate`), движок — в главном: всё, что меняет движок, идёт
  очередями в `EVBotsBridgeRetryPending`. VU-семантику «сразу» (например, `soldier:Kill()` → `soldier = nil`) надо
  отдавать в Lua немедленно, а движок догонять в тике (170).
- Python-heredoc портит `\0`, `\r\n` в строках — скрипты писать файлами (Write), документы править Edit/Write (правило 9).
- `rm` запрещён — только перенос в `trash\<дата>\`. Номер сборки растёт только при развёртывании.
- Счётчик на 0x01183000 — ClientDamageListener::onMessage, НЕ урон; урон мерить по hp `float [солдат+0x34]`.
- Не утверждать «оружия нет» по старым данным — сверять с последним прогоном и словами пользователя.
