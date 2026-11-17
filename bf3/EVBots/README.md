# EVBots — свой локальный аналог Venice Unleashed

VU-подобный стек для BF3 локально (LAN, без интернета): Lua-рантайм уровня VeniceEXT, бэкенд Blaze, лаунчер. Цель — моды VU
(fun-bots) без правок. До 20.10.2026 проект назывался **Legacy**. Прежняя версия этого файла (статусы 08–15.10) —
`C:\Games\trash\2026-10-16\docs-archive\Legacy-README-before-170.md`.

**С чего начать:** `ext\HANDOFF.md` → `ext\STATE.md` → `PLAN.md` (верхний блок). Цель — `stand\GOAL.md`.
Правила — `C:\Games\AGENTS.md`.

## Переименование Legacy → EVBots (20.10.2026)

Старые журналы (`ext\MODLOG.md` до 20.10, разборы, отчёты) не переписывались: в них старые имена. Соответствие:

| Было | Стало |
| --- | --- |
| `C:\Games\Legacy\` | `C:\Games\EVBots-dev\` |
| `Legacy\LegacyBF3\` | `EVBots-dev\stand\` |
| `Legacy\legacyext\`, `legacyext.dll`, `LegacyEXT.*` | `EVBots-dev\evbots-dll\`, `EVBots.dll`, `EVBots.*` |
| env-флаги `LEGACYEXT_*` | `EVBOTS_*` |
| лог `legacyext.log`, префикс `LegacyEXT:` | `EVBots.log`, `EVBots:` |
| C++: `namespace legacy`, `LegacyLog`, `LegacyEngine*` | `namespace evbots`, `EVBotsLog`, `EVBotsEngine*` |
| `Legacy.Backend.exe`, проект `Legacy.Backend` | `EV Backend.exe`, проект `EVBots.Backend` |
| `%APPDATA%\Legacy\` (сертификаты, `legacy-*.pfx`) | `%APPDATA%\EVBots\` (`evbots-*.pfx`, создаются заново) |
| лаунчер друга `LegacyFriend.exe` (`launcher\src\LegacyFriend`) | `EV Launcher.exe` (`launcher\src\EVLauncher`) |
| установщик `LegacyFriend-Setup.exe` (`LegacySetup`) | `EVBots-Setup.exe` (`launcher\src\EVBotsSetup`) |
| `LegacyLauncher.Core/Gui/Tests` | `EVBots.Core/Gui/Tests` |
| установка друга `<папка>\LegacyFriend\` | `<папка>\EVBots\` |
| правила брандмауэра `Legacy-*` | `EVBots-TCP`, `EVBots-UDP`, `EVBots client`, `EVBots server` |
| нативный комплект `C:\Games\BF3LegacyLan\` | удалён 23.10; бинарники — `C:\Games\backup\2026-10-20\BF3NativeBots-bin\` |
| старый WinForms-лаунчер `Legacy.Launcher` (`Legacy.exe`) | `trash\2026-10-20\Legacy.Launcher-winforms\` |

## Карта папки

    evbots-dll\           рантайм (C++ + Lua, EVBots.dll): src\, build.cmd, build\ (DLL, тест-хост 44/44)
    stand\                стенд разработки: run\ (скрипты), activation\, backend\, profiles\, progress\, logs\, справочные .md
    backend\              бэкенд Blaze (C#, EV Backend.exe); BLASE-FIELDS.md — источник истины по полям протокола
    launcher\             src\EVLauncher (EV Launcher — для друзей), EVBotsSetup (установщик), EVBots.Core/Gui/Tests
    ext\                  документы проекта: HANDOFF, STATE, MODLOG (журнал), EDIT-MAP (хуки и патчи), разборы
    PLAN.md               план работ (верхний блок — текущий)
    VU-Docs\              документация VU; content\vext\ref\ — карта API VeniceEXT (цель EVBots.dll)
    Venice-EBX\, Venice-EBX-JSON\   данные BF3 (EBX) текстом и JSON
    SDKFrostbite\         материалы Frostbite (NavPower, Drake, ContentAPI) — только справочник
    New-Blaze-Emulator\, openBlase\  референсы протокола Blaze (bf3_lan.exe — не публиковать)

Вне папки: игра `C:\Games\Battlefield 3`, сервер `C:\Games\BF-Portable`, вывод реверса `D:\Games-cache\re\`,
установка из установщика `C:\Games\EVBots\` (если поставлена), комплекты — `C:\Games\dist\EVBots-<дата>\`.

С 12.11.2026 `re` физически хранится на D:. Старый путь `C:\Games\re\` сохранён как NTFS junction,
поэтому существующие Ghidra-проекты, скрипты и конфигурацию переписывать не нужно.

## Установщик EVBots-Setup.exe

Кладётся в ту же папку, где лежат `Battlefield 3` и `BF-Portable` (или `Battlefield Portable`). Сам находит обе,
создаёт рядом `EVBots\`:

    EVBots\EV Launcher.exe (+ dll)     лаунчер
    EVBots\activation\                 evbots-loader.exe (свой, с 23.10; в комплектах до него — activation-loader.exe),
                                       activation.dll, EVBots.dll, ebx_index.bin
    EVBots\backend\                    EV Backend.exe (+ dll, BouncyCastle)
    EVBots\license\, progress\         лицензия и каталог статистики
    EVBots\profiles\, logs\            пустые — аккаунты чистые; в logs\ потом: EVBots.log (DLL), fun-bots.log (вывод мода,
                                       с 206), server-loader.log, crash.log (только при падении лаунчера)

Без лишнего от нативных ботов (world, server.config, botlayer, host/join.bat). Мод fun-bots и сервер не трогает.
Одно окно UAC: брандмауэр (порты, bf3.exe, сервер), URL-ACL на порт 8080 (127.0.0.1 и текущий LAN IP; запись `+:8080` не ставить — даёт HTTP 503,
установщик её удаляет), удаление старых
правил `Legacy-*`. Нужен .NET 10 Desktop Runtime (x64) — если нет, установщик скажет и даст ссылку.
Тихий режим для проверки: `EVBots-Setup.exe /quiet` (только файлы).

EV Launcher (20.10): «Мой сервер» — режим, карта, боты, имя, карты по кругу, ★ тикеты/очки для каждого режима (и время для
«Захвата флага»): первая карта — через `Startup.txt`, следующие — через RCON заранее (`RconRules.cs`). Настройки лаунчера —
`EVBots\friend-settings.json` (логин, пароль — открытым текстом, карты, правила режимов). Аккаунты — у хоста в `profiles\`
(формат нативного комплекта + `accounts.json`), у гостя — `progress\<логин>.json` (копия статистики для синхронизации).

Удаление (с 20.10): «Удалить EVBots.exe» в папке `EVBots` или «Установленные приложения» Windows (запись «EV Bots» в
разделе текущего пользователя). Удаляет только папку с меткой `.evbots-install`, ярлык, запись и правила брандмауэра (UAC);
аккаунты и прогресс (`profiles`, `progress`) — по вопросу. Тихо: `/uninstall "<папка>" /quiet [/all]`.

Сборка: лаунчер и бэкенд `dotnet build -c Release` → папка `dist\EVBots-<дата>\EVBots\` → zip папки `EVBots` в
`launcher\src\EVBotsSetup\kit.zip` → `dotnet build EVBotsSetup.csproj -c Release` → `bin\Release\net48\EVBots-Setup.exe`.

## Документы `ext\`

| Файл | О чём |
| --- | --- |
| `HANDOFF.md` | как устроено, как гонять стенд, флаги, что работает и что открыто |
| `STATE.md` | короткая сводка текущего состояния |
| `MODLOG.md` | журнал по сборкам (свежие `### Сборка N` выше старых) |
| `EDIT-MAP-2026-10-11.md` | карта хуков и байтовых патчей (H4 — действующие патчи) |
| `VU-AUDIT.md` | как VU отдаёт данные моду и где мы отличались |
| `ENGINE-ENTITIES.md` | сущности движка (флаги, точки спавна) |
| `STUB-RESEARCH.md`, `API-GAP.md` | заглушки рантайма и разрывы с API VU |
| `SPAWN-RESEARCH.md`, `WEAPONS-RESEARCH.md`, `KIT-RESEARCH.md`, `PROGRESSION-BOTS.md` | разборы (история исследований) |
| `FINDINGS-157.md`, `OUTSIDE-REVIEW-2026-10-13.md`, `BLAZE-CROSSCHECK.md`, `BOT-LOGIN-PLAN.md`, `BACKEND-PATH.md` | отчёты прошлых этапов (история; инструкции по запуску в них устарели — см. HANDOFF) |

## Правила (кратко, полностью — AGENTS.md)
Бэкап перед правкой и деплоем; временное — в `C:\Games\trash\<дата>\`, ничего не удалять; не публиковать игровые файлы и
дампы; PunkBuster — только LAN; код x86.
