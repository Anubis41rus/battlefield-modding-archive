# BRIEF — общий контекст для агентов пачки заглушек (сборка 155)

Пишет оркестратор (главный Claude). Читай этот файл первым, потом свой трек из промпта.

## Что за проект

Проект «Legacy»: локальный аналог Venice Unleashed (VU) для Battlefield 3. Сервер — `bf3_server_Final.exe` (x86, ImageBase 0x400000; RVA = VA − 0x400000). Наш мост `legacyext.dll` (C++ + встроенный Lua 5.4) исполняет Lua-мод fun-bots (боты) и реализует API VU: Player, SoldierEntity, EntityManager, ChatManager, Events и др. Часть API — заглушки: они принимают вызов мода и ничего не делают (мод при этом не падает). Цель — закрыть пачку заглушек в сборке 155. Ты получил ОДИН трек; остальные агенты работают параллельно над другими треками.

## Пути

Windows: пользуйся инструментами Read, Grep, Glob. В PowerShell — Get-ChildItem, Select-String, Get-Content -TotalCount / -Tail. НЕ используй `ls`, `find`, `grep`, `head`, `tail`, `cat` в Bash: пользователь на Windows.

- Исходники моста: `C:\Games\Legacy\legacyext\src\` — VextPlayerManager.cpp (~8300 строк, читай диапазонами), VextEntityManager.cpp, VextChatManager.cpp, VextTicketManager.cpp, VextRcon.cpp, VextSpatial.cpp, VextGlobals.cpp, VextResourceManager.cpp, EngineHooks.cpp, VextHooks.cpp, LuaEvents.cpp, ModdingInterface.h.
- План и статус: `C:\Games\Legacy\PLAN.md` (таблица «Список заглушек», строки ~30–49), `C:\Games\Legacy\ext\HANDOFF.md`, `STATE.md`, `API-GAP.md`, `STUB-RESEARCH.md`, `ENGINE-ENTITIES.md`, `VU-AUDIT.md`, `KIT-RESEARCH.md`. Журнал `C:\Games\Legacy\ext\MODLOG.md` огромный: ищи Grep-ом по заголовкам `### Сборка` и `### Прогон`.
- Реверс-выводы (вне репо): `C:\Games\re\` — `stub_decomp.txt`, `method_filter.txt`, `wide_scan.txt`, папки `capture\`, `death\`, `aim\`, `vu\`, `peek\`, `cpe\`. Старые скрипты и дампы — `C:\Games\re\temp-archive\<guid>\`.
- Эталон API VU: `C:\Games\Legacy\VU-Docs\content\vext\ref\` (`server\type`, `server\library`, `server\event`, `fb\`, `shared\`).
- Мод fun-bots — ТОЛЬКО ЧИТАТЬ: `C:\Games\BF-Portable\Battlefield_3_Server\Instance\Admin\Mods\fun-bots\ext\` (`Server\`, `Shared\`, `Client\`).
- Бинарник сервера — ТОЛЬКО ЧИТАТЬ: `C:\Games\BF-Portable\Battlefield_3_Server\bf3_server_Final.exe`. Файловое смещение для VA в `.text`: file = VA − 0x400000 − 0xC00. Дизассемблер: Python 3.13 (команда `python`), установлены capstone, pefile, lief. Ghidra headless (JDK 21) медленная — только если capstone не хватает.

## Правила (обязательны)

1. Исследование — только чтение. Не меняй файлы в `legacyext\src`, не трогай `BF-Portable`, `build\`, `activation\`. Сборку (`build.cmd`) не запускай — её делает оркестратор.
2. Стенд не трогать: не запускать `start_run.ps1`, `run_bare.ps1`, `run_cycle.ps1`, `finish_run.ps1`, `join_client.ps1`; не останавливать и не запускать `bf3_server_Final.exe`, бэкенд или клиент bf3; не копировать DLL.
3. Ничего не удалять. Временное клади в scratchpad `C:\Temp\claude\C--Games\c4657c58-f592-45b8-8608-819ca88fc837\scratchpad\<номер трека>\`.
4. Читай настоящее, не гадай. Адрес, сигнатуру, смещение поля — только с подтверждением: дизассемблер, декомпиляция из отчётов, исходник, строка лога, VU-доки. Не подтверждённое помечай «гипотеза», опровергнутое — «опровергнуто». Слово «окончательно» не используй.
5. Не засоряй контекст: читай диапазонами, ищи Grep-ом, большие файлы целиком не вываливай.
6. Все файлы — UTF-8, текст на русском читаемый.
7. Отчёт пиши в `C:\Games\re\stubs-155\<тема>.md` (каталог уже есть). Структура: 1) вопрос; 2) факты с источником (файл:строка или VA); 3) гипотезы и как проверить; 4) опровергнуто; 5) предлагаемая реализация (сигнатуры, файлы, env-флаг по умолчанию ВЫКЛ, риски вроде падения сервера или рассинхрона с движком); 6) что проверить на стенде (конкретные строки лога). До 250 строк.
8. Итоговое сообщение оркестратору — до 12 строк: вывод, путь к отчёту, уверенность (высокая/средняя/низкая), главный открытый вопрос. Полный отчёт в сообщении не дублируй. Если запись файла запрещена, верни полный отчёт в сообщении (до 250 строк).

## Уже сделано в сборке 155 (не трогай эти места, учитывай их)

События `Player:Joining` / `Authenticated` / `Left` / `Destroyed` для человека. Место: VextPlayerManager.cpp, блок создания и ухода записи человека (около строк 1676–1715). Флаг `LEGACYEXT_NOHUMANEVENTS`: по умолчанию события включены, `=1` — выключить. Бэкап исходника до правок 155: `C:\Games\backup\2026-10-15\VextPlayerManager-build155-pre.cpp`.

## Сборка 156 (15.10 ≈23:50, собрана, не развёрнута) и прогон 155 — учитывай

- 156: `Engine:Update` / `UpdateManager:Update` получают НАСТОЯЩИЙ dt (потолок 1 с), раньше постоянные 1/30 (ModdingInterface.cpp, `UpdateTimerLoop`; `LEGACYEXT_FIXED_DT=1` — как раньше). Причина: в прогоне 155 при создании ботов поток Lua тормозил (300 тиков за 48 с), таймеры мода растягивались — боты выходили по одному раз в ~2 с, лишний бот при смене команды уходил через 3–5 с.
- Прогон 155 (без Host, `LEGACYEXT_EARLY_HOST=0`), наблюдения пользователя: ники над ботами ПРОПАЛИ (в 154 были у всех); второго значка (перк) нет ни у кого; крупные иконки способностей; брошенная техника (БТР РФ и США) стоит и не взрывается (в ранних сборках взрывалась); уровень в статистике 2 вместо 3 (прогресс не пишется?); боты кидают аптечки/патроны/гранаты — работает; США захватывают точки и спавнятся на них.
- Смена команды человеком: по коду мода (`BotSpawner.lua:556–586`, `balanced_teams`) — новый бот в команде с недобором + `KillAll` лишнего; это код мода, тот же, что в VU. После выхода человека мод делает `KillAll` (людей 0) — в логе 21:57:37 убиты 14, мёртвый Josiah удалён ⇒ «14 из 64».
- Лог рантайма: `C:\Games\Legacy\LegacyBF3\logs\legacyext.log` (60 МБ — только Select-String/Grep с шаблоном и временем, прогон 155 — строки `[21:3x`–`[22:14`).

## Что сейчас известно (краткая сводка оркестратора)

- Заглушки, которые пишут «заглушка» в лог: VextPlayerManager.cpp:398–407 (Kick, Ban×6, Spawn, SelectWeapon) и :576–580 (AttachSoldier, EnableInput, Fade, EnterVehicle, ExitVehicle).
- Входящий чат: EngineHooks.cpp:222–231 (`DetourChatMessage` — только счётчик), хук `ServerChatManager::handleMessage` VA 0x00E4DAF0 (RET 8, 2 стековых аргумента + ECX). Событие `Player:Chat` мост не шлёт. Исходящий `ChatManager:SendMessage` — только лог (VextChatManager.cpp:34–70).
- Перебор сущностей по typeinfo работает для счётчиков тикетов: VextTicketManager.cpp:35–73 (мир `[0x02B3EE30]`, begin `0x005CF340`, next `0x005CF2F0`).
- Адреса из STUB-RESEARCH.md (ещё не проверены дизассемблером): enterVehicle 0x00E37950, enterControllable 0x00E35450, exitControllable 0x00E35AC0, leaveControllable 0x00E36090, setSoldierTransform 0x00E0C0B0, applyCustomization 0x00E322F0.
