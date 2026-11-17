# ev-core.dll — ядро EVBots (аналог `vu-core.dll`)

Решение пользователя 23.10.2026: как у Venice Unleashed — две DLL.

| VU | EVBots | Что делает |
|---|---|---|
| `vu-core.dll` | **`ev-core.dll`** | хуки движка, окно сервера (консоль, панель), связь с бэкендом (у VU — Zeus, у нас — EV), порядок старта после защиты |
| `veniceext.dll` | `EVBots.dll` | Lua-рантайм и API модов (fun-bots); пишет в консоль через `ev-core.dll` (у VU veniceext берёт логгер `GetConsoleLogger` у vu-core) |

## Что сейчас умеет (ev-core 1, 23.10.2026)

Окно выделенного сервера `bf3_server_Final.exe` как у VU:
- **консоль** — строки `[2026-10-23 20:31:05:12] [info] [EVBots] …` (формат VU); строки движка Debug/Info скрыты, Warning и
  выше видны. `EVCORE_CONSOLE_ENGINE=all` — все строки движка, `none` — никаких;
- **панель** — `Master: EV`, `GameState: Online` (вместо `battlefield-3-pc` / `IN_GAME`).

Как: адрес в переходнике журнала `0x00401CEE` (jmp на приёмник `0x00421300`) и указатель в vtable панели `0x02648D70`
(метод `0x01143170`). Код игры не патчится. Перед записью все места сверяются по байтам (правило 18), не совпало одно —
не пишем ничего. `EVCORE_WINDOW=0` — без правок. Разбор — `ext\SERVER-WINDOW-2026-10-23.md`.

## Канал клиент↔сервер (К4, `EvNet.cpp`)

TCP 7948 (`EVCORE_HARMONY_PORT`), кадр `u32 LE длина | данные`. **С ev-core 4 (25.10.2026) отправка — через очередь:** у каждого
соединения своя очередь и поток-отправитель, `EvNetSend`/`EvNetBroadcast` только ставят кадр и сразу возвращаются (тик сервера
и кадр клиента не ждут сеть). Очередь одного соединения больше 16 МБ или ошибка `send` (таймаут 15 с) — соединение
закрывается, клиент переподключается. Тест: `build\test\EvNetTest.exe build\ev-core.dll` (порядок 2000 кадров, «зависший»
клиент без чтения).

## Перехваты и правка памяти движка (ev-core 5, сборка 237, 26.10.2026)

Как у VU: код и память движка меняет только ядро, `EVBots.dll` ставит свои обработчики через него (`src\EvHook.cpp`,
`EVBots.dll` — тонкий клиент `InlineHook`). Перехват — 8 байт `E9 rel32 + NOP`, кража целого числа инструкций
(`src\X86Length.cpp`, сверка — `build\test\X86Length.Dump.exe` + `re\verify_decoder.py`), трамплин, побайтный откат.
Замена указателя в слоте и запись байтов — со сверкой ожидаемого (правило 18). Журнал — строки `[hook]` / `[patch]`.

## Экспорт

- `void EvCoreConsoleWrite(const char* level, const char* source, const char* textUtf8)` — строка в консоль окна;
  до создания окна строки копятся и выводятся, как только консоль готова.
- `int EvCoreVersion()` — 6 с 26.10.2026 (окно и Raw Input, сборка 238), 5 — перехваты (237), 4 — 25.10 (очередь канала).
- Окно игры и Raw Input меню (`src\EvWin.cpp`, 238): `EvWndSubclass(hwnd, handler)` — оконную процедуру игры подменяет ядро,
  `handler(hwnd, msg, wp, lp, &result)` из EVBots.dll: 1 — обработано; `EvWndUnsubclass(hwnd)`; `EvRawMouseRelease()` /
  `EvRawMouseRestore()` — снять/вернуть Raw Input мыши игры (usage 1/2, NOLEGACY), строки `[win]` в журнале.
- `EvHookInstall(target, detour, name, &trampoline, &stolen, &instr)` → 0 ок, 1 уже перехвачено, 2 плохой адрес,
  3 пролог не читается, 4 неизвестная инструкция; `EvHookRemove(target)`, `EvHookRemoveAll()`, `EvHookCount()`,
  `EvIsExecutable(addr)`, `EvPatchPointer(slot, value, expect, &old, name)`, `EvPatchBytes(addr, expect, bytes, len, name)`.
- `EvNetSetHandler`, `EvNetServerStart`, `EvNetClientConnect`, `EvNetSend`, `EvNetBroadcast`, `EvNetClientConnected` — канал.

## Загрузка

Сервер: `evbots-loader.exe … --dll ev-core.dll --dll EVBots.dll` (ev-core первым). Журнал — `<папка DLL>\logs\ev-core.log`.
В клиенте (`bf3.exe`) — канал и все перехваты клиента (кадр, PushScreen, Present, курсор win32u) — ставит ядро (237).
`EVBots.dll` берёт ядро из своей папки, если загрузчик его не загрузил (тест-хост, client-harness).

## Сборка

`build.cmd` → `build\ev-core.dll` (MSVC x86, как `evbots-dll`).
