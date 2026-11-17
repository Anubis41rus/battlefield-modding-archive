# Скрипты стенда EVBots (с 16.10.2026; обновлено 20.10 — переименование: `EVBots-dev\stand`, флаги `EVBOTS_*`)

Старые скрипты (`run_bare.ps1`, `join_client.ps1`, `start_run.ps1`, `finish_run.ps1`, `run_cycle.ps1`) устарели и лежат в
`trash\2026-10-16\obsolete-run-scripts\` — не использовать.

| Скрипт | Что делает |
| --- | --- |
| `start.cmd` / `start.ps1` | Останавливает старые сервер и бэкенд, деплоит `build\EVBots.dll` (если поменялась: сначала бэкап рабочей в `backup\<дата>\evbots-before-HHmmss.dll`, после копирования сверяет md5), поднимает бэкенд, затем сервер. Ждёт `LUA-MODE READY` по логу (падение `VEH #` или выход процесса видны сразу). Потом вход клиента. Ключи: `-NoTests`, `-NoJoin`, `-NoDeploy`, `-Force`, `-ReadySec N`, `-ExtraEnv "K=V,K2=V2"` (доп. env-флаги сервера на этот прогон). |
| `start-xp5.cmd` | 23.10: `start.ps1 -StandTickets 100` (полные тикеты, долгий раунд) — для игры с человеком на XP5_002 CQL (спавн ботов в самолёт). `MapList.txt` ставится вручную («XP5_002 ConquestLarge0 1»; прежний — `backup\2026-10-23\MapList-pre-xp5.txt`). |
| `start-frame.cmd` | ★ 26.10 (238-3, реестр А1): `start.ps1 -ExtraEnv "EVBOTS_SERVER_TICK=frame"` — тик Lua сервера из кадра движка, как у VU (по умолчанию — свой поток-таймер). Пользователь на месте. Что смотреть: в `EVBots.log` строки «★ тик Lua — из кадра движка», «★ первый тик Lua из кадра», `heartbeat … тик: кадр движка` (hz, dt_max), «RCON: ответ … в потоке кадра». |
| `start-ab.cmd` | `start.ps1` с опытом А/Б: `EVBOTS_VU_CREATE=ab,EVBOTS_BOT_AI=ab` (группа А — VU-создание и isAI 0, Б — isAI 1). Пользователь на месте. |
| `join.ps1` | Вход клиента сразу на сервер (без меню). Ждёт, пока бэкенд зарегистрирует сервер, персону берёт из `/accounts`. Серверные переменные клиенту не передаёт. Запущенный клиент не трогает. `-Login NAME`. |
| `status.ps1` | Процессы, сервер в бэкенде (игроков N/64), по логу прогона: VEH, ERR, TEST PASS/FAIL, heartbeat, число игроков движка. ★ Сводка журнала самого движка `Instance\Logs\RuntimeLog_*_server.log` (Assert/Crash/Error, путь минидампа) — причина падения бывает только там. `-Errors N`. |
| `tests.ps1` | Ждёт `TESTS-DONE` (до 10 мин, `-WaitMin`) и печатает строки TEST с итогом. Сервер не гасит. |
| `stop.ps1` | Гасит сервер и бэкенд. Пока запущен клиент (может играть человек), отказывается без `-Force`. Клиент не убивает. |
| `cycle.ps1` | Автономный прогон с автокликом «Дислокация». **Только когда пользователь сказал, что ушёл**: без `-UserAway` не запускается. Закрывает клиент, `start.ps1 -Force`, кликает «Дислокация», пока не начнутся тесты, ждёт `TESTS-DONE`. `-PlayMin N` — ещё N минут игры: клик только когда у человека нет тела (строка мода «ЧЕЛОВЕК: тело=false»; раньше клик раз в 15 с стрелял из оружия). `-ExtraEnv`, `-DeployMin`. |
| `cycle.cmd` | `cycle.ps1 -UserAway` (аргументы передаются). |
| `cycle-keep.cmd` | ★ 25.10 (231): `cycle.ps1 -UserAway -PlayMin 20 -KeepUp` — после тестов человек остаётся в игре, стенд не гасится (проверки агента в игре). Гасить — `stop.ps1`. Только когда пользователь ушёл. |
| `cycle-crashmaps.cmd` | ★ 25.10 (233): `cycle-modes.ps1` на картах, где у друга падала старая сборка — MP_012, XP5_002, MP_018 (CQL), до 40 мин; в конце гасит стенд и закрывает свой клиент. Отказ, если клиент запущен. |
| `cycle-play.cmd` | `cycle.ps1 -UserAway -PlayMin 20` — флаги по умолчанию, захватывает смену раунда при коротких раундах. |
| `cycle-modes.cmd` / `cycle-modes.ps1` | ★ 19.10 (сборка 201): **все режимы за один старт, без человека и без автоклика.** Пишет `MapList.txt` (по умолчанию MP_007 CQL → TDM → Rush → SQDM → XP5_002 AirSuperiority → XP5_002 CQL; `-Maps "…;…"`; в конце восстанавливает), стартует с `EVBOTS_TESTS_NOHUMAN=1` (тесты сразу по телам ботов) и `EVBOTS_TESTS_CYCLE=1` (после `TESTS-DONE` мост сам шлёт RCON `mapList.runNextRound`). Ждёт `TESTS-ALL-DONE` / падение (`VEH #`) / `-MaxMin` 35, печатает уровни, FAIL, частые ERR, VEH, status; потом закрывает свой клиент и гасит стенд (`-Keep` — оставить). Отказ, если клиент уже запущен. Лог — `trash\<дата>\run\cycle-modes-*.log`. |
| `cycle-ab.cmd` | `cycle.ps1 -UserAway -PlayMin 15` с опытом А/Б (`VU_CREATE=ab,BOT_AI=ab`). |
| `cycle-natural.cmd` | ★ 22.10: естественный конец раунда (без `runNextRound`, `EVBOTS_TESTS_CYCLE=0`): MP_001 TDM → XP3_Shield CQL, 14 мин — сброс счёта и спавн во 2-м раунде. |
| `cycle-reload.cmd` | ★ 22.10: смена уровня без конца раунда: MP_001 Rush → `runNextRound` SQDM (сценарий падения 20.10 09:24), 12 мин. |
| `cycle-air-natural.cmd` | ★ 22.10: авиация + естественный конец раунда: MP_007 CQL → MP_001 TDM, 16 мин — ракеты/ловушки, посадка в занятую технику. |

Для сравнения контрольных сумм Royale есть `start-royale-checksum.cmd`
(`start.ps1 -ModProfile Royale -NoTests -ChecksumDiagnostics`). Сервер пишет входы
в свой RuntimeLog, клиент — строки CHECKSUM-TYPE/GUID в EVBots-client.log через
опциональный наблюдатель EVBOTS_CHECKSUM_TRACE. Проверка суммы остаётся включённой;
Level.FullNameChecksumLog не включать: он меняет входные данные расчёта.
Обычный запуск не включает этот подробный журнал. Клиентский endPostChunks можно
отключить для сравнения через EVBOTS_CLIENT_POSTCHUNKS=0 в окружении клиента.

Общее — `common.ps1`: пути, env-флаги сервера (только те, что код ещё читает), чтение лога с байтового смещения без
блокировки файла.

- ★ 20.10: `start.ps1` перед стартом пишет в `Instance\Admin\Startup.txt` `vars.gameModeCounter` (`-StandTickets`, по умолчанию 30 —
  короткие раунды) и `vars.ctfRoundTimeModifier 100`, убирает `vars.roundTimeLimit` (в BF3 его нет). EV Launcher на этом ПК пишет туда
  свои значения — стенд от них не зависит. Логи прогона: `stand\logs\EVBots.log` (DLL), `stand\logs\fun-bots.log` (вывод мода, с 206).
- Стенд и установленный EV Launcher одновременно не запускать: одни порты (8080, 10041, 42127, 80, 47900); `Stop-Stand` гасит
  все процессы `EV Backend`. Итоги `tests.ps1`, запущенного сразу после `start.cmd`, не брать: `run-state.json` может быть прошлым.

- ★ 26.10 (сборка 238): **частота движка как у VU** (`-high60`/`-high120`) — файл `run\frequency.txt` с одним числом 30, 60 или
  120 (нет файла — 30). `common.ps1` даёт его серверу как `EVBOTS_FREQUENCY`, `join.ps1` — клиенту. Строки `[freq]` — в
  `stand\activation\logs\ev-core.log`; выключить правки движка — `EVBOTS_FREQ_PATCH=0`.
- Смещение лога и md5 DLL текущего прогона — `stand\logs\run-state.json` (по нему работают status и tests).
- Вывод прогонов — `trash\<дата>\run\` (`start-*.log`, `cycle-*.log`, `backend.out`, `server-loader.out`, `join.out`).
- Скрипты в ASCII: Windows PowerShell 5.1 читает файлы без BOM как ANSI. ★ С 19.10 комментарии в коде — только по-русски
  (пользователь): новый `.ps1` с кириллицей сохранять в UTF-8 с BOM (`cycle-modes.ps1` — так); строки вывода оставлять ASCII.
- Из админ-сессии агента стенд стартует только через `explorer.exe C:\Games\EVBots-dev\stand\run\<имя>.cmd`
  (explorer аргументы не передаёт — для вариантов есть отдельные `.cmd`).
- Сообщение `click 1110 646: error target window is not in the foreground` в окне cycle — не ошибка: впереди другое окно,
  Windows не даёт перехватить фокус; клик повторяется.
