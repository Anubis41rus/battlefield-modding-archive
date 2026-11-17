# VU-HOWITWORKS — как на самом деле работает Venice Unleashed и что из этого у нас уже есть

Дата 2026-10-08. Источники: `VU-Docs\content\` (документация VU) + бинарный
анализ установленного `%LOCALAPPDATA%\VeniceUnleashed\` + строки нашего
`BF3LegacyLan.Backend.exe`.

---

## 1. VU = четыре слоя, а не один мод

```
┌──────────────────────────────────────────────────────────────────┐
│  bf3.exe  (ваша установка BF3, VU НЕ поставляет свой)            │
│    ▲ -gamepath <путь>                                           │
│    │                                                            │
│  vu.exe  (129 КБ, NativeAOT F#, 32-бит) — ЛАУНЧЕР              │
│    │  • парсит аргументы (CommandLineToArgvW)                    │
│    │  • запускает bf3.exe / bf3_server_Final.exe                 │
│    │  • импорты инжекции: VirtualProtect, FlushInstructionCache, │
│    │    AddVectoredExceptionHandler, LoadLibraryA                │
│    ▼                                                            │
│  vu-core.dll (9.4 МБ) — ИНЖЕКТОР + ядро                         │
│    CreateProcessA/W, SuspendThread, ResumeThread,               │
│    VirtualAllocEx, WriteProcessMemory, CreateRemoteThread,      │
│    QueueUserAPC, SetThreadContext,                              │
│    Wow64DisableWow64FsRedirection, SetDllDirectoryW             │
│    → стартует bf3 ПОДВЕШЕННЫМ, заливает veniceext.dll,          │
│      меняет EIP, снимает suspend                               │
│    ▼                                                            │
│  veniceext.dll (149 МБ) — VENICEEXT = мод-система               │
│    1 экспорт GetModdingInterface() → объект, vtable из 5 методов │
│    Lua 5.4.2 (lua.dll, 238 КБ) + SOL3 (7069 RTTI-строк)         │
│    при init пишет абсолютные адреса движка в таблицы            │
│    0x18E74Cxx..0x18E753xx                                       │
│    v8.dll + cohtml = WebUI (ui.vuic)                            │
└──────────────────────────────────────────────────────────────────┘
        ▲ Blaze/Harmony (UDP 25200 / 7948), RCON 47200
        ▼
   Z E U S  — онлайн-бэкенд (облако VU)
     • аккаунты, логин по почте
     • СОЛДАТЫ (профиль + прогресс/ранг/XP)
     • реестр серверов → серверный браузер
     • server.key = идентичность сервера (GUID)
     • выдача обновлений (-updateBranch prod|dev)
     • NAT/IP-детект (-joinaddr, "backend auto-detect IP")
     • живая связь с сервером: "игроки отваливались при потере связи с Zeus"
```

**Пять выводов, которые всё упрощают:**

1. **Инъекция VU и наша — одно и то же.** `vu-core.dll` импортирует ровно тот
   набор API, который использует наш `activation-loader`. Изобретать нечего.
2. **VU не берёт наш бинарь — он берёт НАШ `bf3.exe`** (`-gamepath`). Значит и мы
   можем оставить `bf3.exe`/`bf3_server_Final.exe` как есть и только инъектировать.
3. **`GetModdingInterface` — контракт «игра ↔ мод-система».** У нас нет
   пропатченной игры, которая его зовёт, поэтому вызывать будем сами из
   `legacyext.dll` при загрузке. Формально тот же контракт, без патча движка.
4. **Ни один из 8 адресов botlayer в veniceext не встречается** (проверено
   сканером: 221 адрес движка, 0 совпадений). Пересекаться незачем: botlayer хукает
   логику бота, VU — моддинг/UI/данные. Нам нужен **механизм**, а не чужие адреса.
5. **server.key держится только на Zeus.** Нет Zeus — нет и проверки.

---

## 2. Роли сервера: инстанс-каталог и моды

Из `hosting\config.md`, `hosting\mods.md`, `modding\structure.md`:

```
Documents\Battlefield 3\Server\         ← инстанс сервера (-serverInstancePath)
    server.key                           ← GUID сервера (у нас: свой идентификатор)
    Admin\Startup.txt                     ← vars.serverName "…", vars.gamePassword "…"
    Admin\MapList.txt                     ← MP_Subway ConquestLarge0 1
    Admin\ModList.txt                     ← список модов, по имени папки
    Admin\Mods\<мод>\mod.json
             \ext\server\__init__.lua     ← единственный авто-запускаемый
             \ext\client\__init__.lua
             \ext\shared\__init__.lua
             \ui.vuic                    ← WebUI (опционально)
```

Ключевые правила (важно для совместимости модов):

- Мод запускает **только** `__init__.lua` в каждом из трёх контекстов.
- Остальные файлы — через `require('путь')`. Путь **относительно** `ext/server`
  (или `client`), т.е. `require` внутри `ext/server/somefolder/f.lua` ищет
  `ext/server/f.lua`, **а не** `ext/server/somefolder/f.lua`.
- Shared-файлы требуют префикс `__shared/`: `require('__shared/x.lua')`.
- `.lua` расширение опционально, другое расширение — нельзя.
- Скрипт исполняется **один раз**; `return` из скрипта возвращается в `require`.
- **Моды живут на сервере и сами скачиваются клиентами при входе**
  (`modding\structure.md`: «All VU mods are hosted on the server-side and
  automatically downloaded by joining clients»).
- Серверные команды `modList.*` (`Add/Remove/Clear/List/ListRunning/
  ReloadExtensions/Available/Debug`) пишут `ModList.txt`.
- `modList.Debug true` — «скрипты будут собраны с отладочными символами»,
  т.е. VU **компилирует** Lua в байт-код и отдаёт клиентам скомпилированным.
  Нам это не нужно: раздаём `.lua` как есть.
- `-skipChecksum` — отключает проверку контрольной суммы уровня. Полезно и нам.

Lua-контур (`vext\_index.md`): Lua **5.4**; доступны `table, string, math, os,
coroutine, package, utf8`; **вырезаны** `dofile, load, loadfile, string.dump,
package.config, package.cpath, package.loadlib, package.path,
package.searchpath, os.execute, os.exit, os.getenv, os.remove, os.rename,
os.setlocale, os.tmpname`; **добавлены** `string.starts`, `string.ends`,
`json.encode`, `json.decode`.

---

## 3. ★ Что у нас уже закрыто (и это больше, чем казалось)

`BF3LegacyLan.Backend.exe` — Native AOT .NET, 71 МБ. **Исходников нет**, ILSpy
не берёт (`PE file does not contain any managed metadata`) — поэтому его
поверхность снята строками (`C:\Games\re\backend-utf16.txt`, 24 697 строк).
Его собственный `--help`:

```
serve [--seconds N] [--player-name NAME] [--player-id PID]
      [--game-server-address IP:PORT] [--redirector-port N] [--blaze-port N]
      [--profiles-dir PATH] [--progress on|off] [--stats-catalog FILE]
    | sinkhole status
    | gen-cert <dir>
```

### 3.1 Роль Zeus у нас реализована на ~75%

| Функция Zeus | Есть ли в нашем бэкенде | Чем подтверждено |
|---|---|---|
| Аккаунт / логин | **да** | `[blaze] login MAIL=`, `loginPersona PNAM=`, `silentLogin PID=` |
| Солдат (профиль) | **да** | `[blaze] profile `, `profiles\<id>\profile.json` (`name`) |
| Прогресс: определения | **да** | `[blaze] getStatGroup NAME=` + `progress\bf3-stats-catalog.json` |
| Прогресс: значения | **да** | `[blaze] getStatsByGroupAsync EID=/NAME=`, `profiles\<id>\progress.json` (1432 стата) |
| Права/DLC | **да** | `[blaze] listUserEntitlements2 ETAG=` |
| Настройки игрока | **да** | `userSettingsLoad/Save/LoadAll` → `settings\cust.bin`, `sdt.bin` |
| Жизненный цикл игры | **да** | `createGame`, `finalizeGameCreation`, `advanceGameState`, `setPlayerCapacity`, `updateGameName`, `removePlayer`, `destroyGame`, `replayGame` |
| Вход игрока | **да** | `fetchClientConfig`, `fetchClientConfig`, `preAuth`, `postAuth`, `joinGame GID=/GVER=`, `PlayerJoining/ClaimingReservation`, `UserAdded/UserUpdated`, `JoiningPlayerInitiateConnections HNET=` |
| Сеть | **да** | `updateNetworkInfo`, `[qos] listening`, `getTelemetryServer -> loopback, opt-out` |
| Заглушка EA-сервисов | **да** | `sinkhole status`, `# BEGIN BF3LegacyLan sinkhole` |
| TLS-сертификаты | **да** | `gen-cert <dir>`, `cacert.pem` |

Каталог прогресса (`progress\bf3-stats-catalog.json`, 271 КБ) — это и есть
**лестница рангов**: `group = "player_mpdefault2"`, **1432 стата**
(`{name, mode}`), плюс готовый `getStatGroupPayloadBase64` (180 920 симв.) —
это payload, который бэкенд и отдаёт клиенту.

### 3.2 Чего у нас нет (это и есть работа)

| Чего нет | Как у VU | Что делать |
|---|---|---|
| Реестр серверов + браузер | `connect <guid>`, `vu://join/<guid>`, вкладка «Servers», `-unlisted`, `-joinaddr/-joinhost` | Свой GUID сервера + `getServerInstance`-подобный список + вкладка в лаунчере |
| Раздача модов клиентам | моды качаются автоматически | Раздавать `Admin\Mods` клиенту при входе (локально — можно и просто положить на диск) |
| WebUI (ui.vuic / cohtml) | отдельный UI-слой | **не повторяем** первым; `HasWebUI: true` у fun-bots — опционально |
| Обновления | `-updateBranch`, `-env prod\|dev` | не нужны |
| NAT-детект | «backend auto-detect IP» | LAN-автодетект в своём бэкенде |

---

## 4. ★ Почему у ботов не показывается уровень — причина найдена

Механизм прогресса в BF3 такой:

```
клиент ──Blaze──► бэкенд: getStatGroup("player_mpdefault2")
                   = ОПРЕДЕЛЕНИЯ (лестница рангов, что за что открывается)
клиент ──Blaze──► бэкенд: getStatsByGroupAsync(EID=..., NAME=...)
                   = ЗНАЧЕНИЯ конкретного игрока (kills, score, XP...)
                   ← читаются из profiles\<personaref>\progress.json
```

`EID`/`personaref` — это идентификатор солдата. **У нативных ботов botlayer
идентификатора нет**: бот создаётся прямо в движке, бэкенд о нём не знает,
`progress.json` для него не существует ⇒ клиент не знает ранг ⇒ в таблице
уровень/значок не отображаются.

**Значит лечится это не в движке, а в данных.** Нужно, чтобы у бота был:
1. свой `persona`/EID,
2. запись прогресса в `profiles\<bot-id>\progress.json` (можно скопировать
   шаблон реального игрока — набор из 1432 стат),
3. вход в игру через `loginPersona`/`silentLogin`, как у обычного игрока.

В VU это сделано тем же способом — солдат есть и у бота, созданного через
`PlayerManager:CreatePlayer(name, team, squad)`.

Дополнительно у fun-bots есть `CUSTOMIZE`/`SOLDIER`-флаги у botlayer — то есть
прецедент в коде уже есть.

---

## 5. ★ Что и откуда берём: сводка

| Слой | У VU | Наш аналог | Готовность |
|---|---|---|---|
| Лаунчер (клиент+сервер в одном бинаре) | `vu.exe` + `vu.com` | **`Legacy.exe`** (свой, модель `vu.exe`) | строим |
| Бэкенд (Zeus) | Zeus, облако | **«Legacy»** = наш бэкенд, дописанный | **~75% готово** |
| Инжектор | `vu-core.dll` | `activation-loader.exe` | **готово** |
| Мод-система | `veniceext.dll` (Lua 5.4 + SOL3) | **`legacyext.dll`** | строим |
| Прогресс/профили/статистика | Zeus | наш бэкенд + `profiles\`, `progress\` | **готово** |
| Боты | Lua-моды (fun-bots 3.0.0) | те же моды | строим обёртки |
| Навигация ботов | навмеши VU | `world\<Карта>_<Режим>.grid/.cover` (уже испечены) | **готово** |
| Свой бинарь игры | патченный Frostbite | не патчим, только инъекция | — |

Инструменты сборки на месте: **.NET SDK 8/10**, **Visual Studio 2026 (C++
x86)**, Python 3.13, ilspycmd 10.1 (не берёт NativeAOT, но нужен для других
сборок). Исходники Blaze-сервера для референса и базы:
`Legacy\New-Blaze-Emulator` (78 `.cs`, полный TDF-кодек) и `Legacy\openBlaze`.

---

## 6. Ответы на прямые вопросы

**«Свой лаунчер, как у VU, всё в одном»** — да: `Legacy.exe` с режимами
`Legacy.exe` (клиент), `Legacy.exe --server --dedicated` (сервер с окном
вывода), `--headless`, `-serverInstancePath <path>`, `--listen`, `--unlisted`,
`--joinaddr`. Плюс вкладка «Серверы» с входом по клику. Сейчас лаунчер —
панель без списка серверов, её заменяем.

**«Профили, аккаунты, статистика»** — **уже есть**: `profiles\<id>\profile.json`
(имя солдата), `progress.json` (1432 стата), `settings\{cust,sdt}.bin`,
`reports\*.rpt`, логин по `MAIL`, по `PNAM`, по `PID`. Дорабатываем интерфейс,
не переписываем.

**«Своя база данных, если надо»** — сейчас JSON на диске (`profiles\`,
`progress\bf3-stats-catalog.json`). Для LAN-пати на 5–10 человек этого
**достаточно**; SQLite имеет смысл только если захотим историю матчей.
Решение — позже, по факту.

**«Вместо зевса и блейза — легаси»** — да: наш бэкенд и есть «Legacy».
Он уже говорит на Blaze-протоколе и обслуживает аккаунты/прогресс.

**«Пеки карт»** — да, `world\` уже испечён и лежит на месте; для fun-bots
навигация будет брать оттуда.

**«Как инжектится»** — так же, как у нас уже работает:
`activation-loader --dll legacyext.dll`. У VU технически другой носитель
(`vu-core.dll` через APC/SetThreadContext), но API тот же.

---

## 7. Что НЕ повторяем

Zeus-облако, TLS/`cacert.pem`, asmjit-JIT (у нас Capstone/SOL3 достаточно),
CoHTML/V8 WebUI (пока), раздача обновлений, `server.key` как понятие,
`-env prod|dev`, NAT-детект в интернет, RCON-совместимость с BattleCon целиком
(работает через RCON-порт 47900 — достаточно).