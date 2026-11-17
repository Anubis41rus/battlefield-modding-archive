# Окно выделенного сервера как у VU — разбор `bf3_server_Final.exe` (23.10.2026)

Просьба пользователя (скрины VU): в консоли окна — свои строки `[дата] [info] [VeniceEXT] …`, в панели — `Master: Zeus`,
`GameState: Online`. Делаем `[info] [EVBots] …` и `Master: EV`, `GameState: Online` в `ev-core.dll` (аналог vu-core).

**Важно (факт):** у VU сервер — клиентский `bf3.exe` в режиме дедика (все установщики хуков vu-core бьют по адресам
клиентского exe; окно «Battlefield 3™ Server» на скринах VU — от `bf3.exe`). Наш сервер — отдельный `bf3_server_Final.exe`,
поэтому места искали сами. Инструменты: строки и capstone по exe, Ghidra (`re\ghidra-jsrv`, pyghidra).

## Консоль окна

| Что | Адрес | Факт |
|---|---|---|
| Окно, `LISTBOX` и `EDIT` | `FUN_00422880` (`0x422E61` — `"LISTBOX"`) | `RegisterClassExA` + `CreateWindowExA` |
| Приёмник журнала окна | `FUN_00421300(record)`, `ret 4`, `ecx` не читается (сразу `lea ecx,[esp+0x28]`) | `record[0]` → `{int level; const char* file; int line; …}`, `record[1]` — канал, `record[2]` — текст. Уровни: 1 Debug, **2 — «сырая» строка без префикса**, 4 Info, 8 Warning, 0x10 Assert, 0x20 Error, 0x80 Validate, 0x100 FatalAssert, 0x200 Crash. Остальные форматируются как `файл(строка): "канал": Info: текст` |
| Вход | переходник `0x00401CEE: E9 0D F6 01 00` (jmp `0x421300`) | единственная ссылка на приёмник |
| Пролог приёмника | `B8 3C 81 00 00` + `call __alloca_probe` (rel32) | InlineHook (8 байт) сюда нельзя — в украденных байтах относительный `call` |
| Очередь окна | `FUN_00421030`: под замком `[0x028772AC]` строки в очередь, лимит `[0x028430FC]`, `PostMessageA(окно, 0x8003)` | потокобезопасно; замка нет (окно не создано) — строки не попадают |

**Как сделано (ev-core 1):** rel32 переходника `0x401CEE` → наш обход (4 байта, сверка до записи). Обход: строки движка
Debug/Info скрываем (Warning и выше видны; `EVCORE_CONSOLE_ENGINE=all|warn|none`), остальное — в оригинал. Свои строки —
оригинал с уровнем 2 и готовым текстом `[YYYY-MM-DD HH:MM:SS:cc] [info] [EVBots] …` (UTF-8 → ANSI). До создания окна свои
строки копятся и выводятся при первой строке движка после появления замка.

## Панель

| Что | Адрес | Факт |
|---|---|---|
| Статистика (UpTime, PlayerCount, GhostCount, Memory, BuildId, kbps, State) | около `0xD5E77B…0xD5EFB6` | не трогаем |
| Пары `GameMode`, `Environment`, `Master`, `GameState`, `GameId`, `PingSite`, `MatchId` | виртуальный `FUN_01143170(this, list)`, thiscall, `ret 4`; слот vtable `0x02648D70` (.rdata) | значения: `Master` = vfunc+0xC объекта Blaze (`battlefield-3-pc`), `GameState` = vfunc+0x10 (`IN_GAME`) или `OFFLINE` |
| Узел списка | 0x28 байт: `[0] след, [1] пред, +8 ключ, +0x18 значение`; строка `{begin, end, cap, arena}` | кольцевой список с головой `list` (новый узел — в конец) |

**Как сделано:** указатель в слоте vtable → наш обход (оригинал, затем правка значений): `Master` → `EV`, `GameState` →
`Online` (`OFFLINE` → `Offline`). Пишем в буфер самой строки (новое значение короче), память движка не выделяем и не
освобождаем.

## Строки EVBots.dll в консоли (сборка 217)

`Initializing the EVBots engine (VeniceEXT API)...`, `Loading EVBots module 'fun-bots' 3.0.0.`, `[fun-bots] Loaded shared/server
script '__init__.lua'.`, `Successfully loaded EVBots module …`, `Level loaded: Levels/MP_007/MP_007 (ConquestLarge0).`,
`Level destroyed.`, `Player 'X' is requesting to join the server.` / `has established a connection to the server.` /
`left the server.`, ошибки событий Lua, `print` модов (`[fun-bots] …`). Плюс от ev-core: `[EV-Core] Initializing the EV core v1...`.
