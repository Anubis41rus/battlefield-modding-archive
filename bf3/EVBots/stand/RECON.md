# RECON — отчёт разведки стека LegacyBF3

Сделано 2026-10-08. Цель: снизить риск на старте фаз 2–4 (запуск сервера/игру
пока **заблокировано** — ничего не запускать).

---

## 1. Как устроен BF3LegacyLan (текущий комплект)

### Лаунчер (host.bat → запускает всё)
1. Читает `server.config` (пути к игре, серверу, `activation-loader`, `botlayer.dll`).
2. Создаёт/чекает `profiles\`, `progress\`, `world\` — хранит прогресс команд.
3. Запускает **backend** `BF3LegacyLan.Backend.exe serve --redirector-port 42127
   --blaze-port 10041 --redirector-bind-endpoint ...` и ждёт готовности.
4. Запускает **activation-loader --dedicated --game `<bf3.exe>` --dll
   `<botlayer.dll>`** (инъекция бот-слоя) → загрузка `bf3_server_Final.exe`.
5. `join.bat` делает то же самое для клиента: `--client`, `requestState
   State_ClaimReservation` с `serveraddr=<backend:42127>`, `gameid=<gameid>`,
   `personaref=<personaref>` → тогда клиент линкуется на backend Blaze + к серверу 25300.

### Backend — не скрипт, а .NET (self-contained)
`BF3LegacyLan.Backend.exe` = 71.4 МБ, версия `1.0.0+<commit>` → **самостоятельно
собранный .NET-консольник** (проект `BF3LegacyLan.Backend`, библиотека .dll,
`FileVersionRaw 1.0.0.0`). Внутри есть целые слои:

| Тип       | Класс / событие              | Назначение                            | В коде |
|-----------|------------------------------|---------------------------------------|--------|
| listener  | `BlazeListener` (`HandleClientAsync`, `DisposeAsync`) | приём клиентских Blaze-пакетов | есть |
| listener  | `QosProbeListener` (`ReceiveLoopAsync`) | QoS-пробы игроков               | есть |
| router    | `BlazeRouter` (`DispatchAsync`) | маршрутизация Blaze-команд            | есть |
| conn      | `BlazeConnection` (`SendAsync`)| отправка в клиента                    | есть |
| game      | **GameManagerService** (`HandleJoinGame` и `<HandleJoinGame>b__0`) | ждёт join-пакет, делает GID | есть |
| game      | `<>c__DisplayClass31_0+<<HandleJoinGame>b__0>d__` | логика handling join          | есть |
| blaze     | `UserSettingsService` (`HandleLoad/HandleSave`) | настройки клиента            | есть |
| net       | `BlazeConnection` (SendAsync)  | pushing + клиенты                     | есть |

**Логи backend-консоли (несколько строк):**
- `[blaze] fetchClientConfig CFID="..."` — отдаёт клиентскую конфигурацию
- `[blaze] GAME GID=...` — создаётся игра с GID
- `[blaze] joinGame GID=... GVER=...` — join-пакет от клиента
- `[blaze] notify PlayerJoining/ClaimingReservation (host), UserAdded/UserUpdated + JoiningPlayerInitiateConnections HNET=` — широковещательные уведомления
- `[blaze] getStatGroup NAME=...`, `getTelemetryServer -> loopback, opt-out` — телеметрия в локалку

**Инференция:** backend уже реализует flow вроде зевсовского:
узнать клиента (`fetchClientConfig`), создать игру (`GAME GID`), обработать join
(`joinGame`, `ClaimingReservation`, `UserAdded`), инициировать соединения
(`JoiningPlayerInitiateConnections`). В строках **нет HTTP-роутов вроде `/servers`
или `list`** — HTTP v0.9 HttpListener есть (`HttpListener: {0} HttpListenerRequest: {1}`),
но маршрутный слой не локализовался, а явной строки `expected an address like
127.0.0.1:25300` — это значение первичного адреса подключения клиентов.
**GET /servers (серверный браузер) = этап 2, критично — ставится как "дописать",
а не "с нуля".**

### Роли портов (из host/stop/README)
- `42127` — redirector-служба backend (адрес, который клиенты используют)
- `10041` — Blaze ядро backend (между клиентом и сервером)
- `25300` — GamePort игры: RCON + данные клиент-сервер
- `47900` — RCON
- UDP 7948 (Harmony), 25200 (Frostbite) — как у обычного BF3

### Лаунчер (host.bat → запускает всё)
1. Читает `server.config` (пути к игре, серверу, `activation-loader`, `botlayer.dll`).
2. Создаёт/чекает `profiles\`, `progress\`, `world\` — хранит прогресс команд.
3. Запускает **backend** `BF3LegacyLan.Backend.exe serve --redirector-port 42127
   --blaze-port 10041 --redirector-bind-endpoint ...` и ждёт готовности.
4. Запускает **activation-loader --dedicated --game `<bf3.exe>` --dll
   `<botlayer.dll>`** (инъекция бот-слоя) → загрузка `bf3_server_Final.exe`.
5. `join.bat` делает то же самое для клиента: `--client`, `requestState
   State_ClaimReservation` с `serveraddr=<backend:42127>`, `gameid=<gameid>`,
   `personaref=<personaref>` → тогда клиент линкуется на backend Blaze + к серверу 25300.

### Backend — не скрипт, а .NET (self-contained)
`BF3LegacyLan.Backend.exe` = 71.4 МБ, версия `1.0.0+<commit>` → **самостоятельно
собранный .NET-консольник** (проект `BF3LegacyLan.Backend`, библиотека .dll,
`FileVersionRaw 1.0.0.0`). Внутри есть целые слои:

| Тип       | Класс / событие              | Назначение                            | В коде |
|-----------|------------------------------|---------------------------------------|--------|
| listener  | `BlazeListener` (`HandleClientAsync`, `DisposeAsync`) | приём клиентских Blaze-пакетов | есть |
| listener  | `QosProbeListener` (`ReceiveLoopAsync`) | QoS-пробы игроков               | есть |
| router    | `BlazeRouter` (`DispatchAsync`) | маршрутизация Blaze-команд            | есть |
| conn      | `BlazeConnection` (`SendAsync`)| отправка в клиента                    | есть |
| game      | **GameManagerService** (`HandleJoinGame` и `<HandleJoinGame>b__0`) | ждёт join-пакет, делает GID | есть |
| game      | `<>c__DisplayClass31_0+<<HandleJoinGame>b__0>d__` | логика handling join          | есть |
| blaze     | `UserSettingsService` (`HandleLoad/HandleSave`) | настройки клиента            | есть |
| net       | `BlazeConnection` (SendAsync)  | pushing + клиенты                     | есть |

**Логи backend-консоли (несколько строк):**
- `[blaze] fetchClientConfig CFID="..."` — отдаёт клиентскую конфигурацию
- `[blaze] GAME GID=...` — создаётся игра с GID
- `[blaze] joinGame GID=... GVER=...` — join-пакет от клиента
- `[blaze] notify PlayerJoining/ClaimingReservation (host), UserAdded/UserUpdated + JoiningPlayerInitiateConnections HNET=` — широковещательные уведомления
- `[blaze] getStatGroup NAME=...`, `getTelemetryServer -> loopback, opt-out` — телеметрия в локалку

**Инференция:** backend уже реализует flow вроде зевсовского:
узнать клиента (`fetchClientConfig`), создать игру (`GAME GID`), обработать join
(`joinGame`, `ClaimingReservation`, `UserAdded`), инициировать соединения
(`JoiningPlayerInitiateConnections`). В строках **нет HTTP-роутов вроде `/servers`
или `list`** — HTTP v0.9 HttpListener есть (`HttpListener: {0} HttpListenerRequest: {1}`),
но маршрутный слой не локализовался, а явной строки `expected an address like
127.0.0.1:25300` — это значение первичного адреса подключения клиентов.
**GET /servers (серверный браузер) = этап 2, критично — ставится как "дописать",
а не "с нуля".**

### Роли портов (из host/stop/README)
- `42127` — redirector-служба backend (адрес, который клиенты используют)
- `10041` — Blaze ядро backend (между клиентом и сервером)
- `25300` — GamePort игры: RCON + данные клиент-сервер
- `47900` — RCON
- UDP 7948 (Harmony), 25200 (Frostbite) — как у обычного BF3
