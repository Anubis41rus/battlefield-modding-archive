# EV — свой протокол EVBots: Blaze + функции Zeus в одном сервисе (23.10.2026)

> Решение пользователя 23.10: два протокола (Blaze и Zeus) «объединить правильно» и назвать **EV**; бэкенд обновить под это;
> всё локально (LAN, без интернета); **проверок лицензий нет**. Этот документ — решения и план работ. Источник ID сообщений
> Zeus — проект EV пользователя (`%LOCALAPPDATA%\VeniceUnleashed\client\EV\docs\ZEUS-MESSAGES.md`, `ZEUS-PROTOCOL.md`),
> только чтение и со сверкой (EV — «косячно», брать нужное).

## 1. Что есть что

| | Blaze | Zeus (у VU) | EV (у нас) |
|---|---|---|---|
| Кто говорит | сама BF3 (клиент и сервер), протокол EA | `vu-core.dll` ↔ облако VU | `EVBots.dll` (клиент и сервер) и EV Launcher ↔ EV Backend |
| Зачем | вход в игру, персоны, создание/поиск игры, статистика, отчёты | аккаунт VU, браузер серверов, ключ сервера, допуск игрока, обновления | аккаунт EV, браузер, личность сервера, допуск игрока, раздача модов, уведомления |
| Менять можно? | **нет** — его говорит exe игры, совместимость байт в байт (`BLASE-FIELDS.md`) | — | да, наш |

**Факт:** Blaze заменить нельзя — его говорит `bf3.exe`/`bf3_server_Final.exe`. «Объединить» значит: **один сервис, одна база
аккаунтов и одна сессия игрока на оба протокола**, а не один формат на проводе.

## 2. Решения (принято 23.10, пользователь дал добро решать самому)

1. **Один процесс — EV Backend** (`EV Backend.exe`, проект `EVBots.Backend`). В нём три «лица»:
   - **Blaze** (как сейчас: редиректор 42127, ядро 10041, QoS 17554) — без изменений протокола;
   - **EV-канал** (новый, TCP, порт **7828** — тот же, что у Zeus) — функции Zeus для `EVBots.dll` и лаунчера;
   - **HTTP** (8080, как сейчас) — браузер, профили, раздача модов (`ModServer.cs`), панель.
2. **Одна сессия на оба протокола.** Лаунчер входит в EV (`Authentication.Login`) → токен (`AccountsStore.IssueToken`, уже
   есть) → тот же токен уходит в BF3 как токен входа Blaze → Blaze-сессия и EV-сессия — один аккаунт/персона. Сейчас
   лаунчер уже входит по personaref; добавляем только EV-сессию поверх.
3. **Формат EV-канала** — модули и ID сообщений Zeus (`module_id`, `type_id` из `ZEUS-MESSAGES.md`), чтобы смысл совпадал с VU;
   кадр — `u32 LE длина | u16 module_id | u16 type_id | u32 request_id | тело JSON (UTF-8)`. **Без шифрования и без DH**
   (LAN; у Zeus DH2+AES-GCM — для интернета). **Не совместим по проводу с `vu.exe`** — нашему клиенту (BF3 + `EVBots.dll`)
   это не нужно. Если когда-нибудь понадобится настоящий `vu.exe` — отдельный адаптер по `ZEUS-PROTOCOL.md`.
4. **Лицензий нет.** `GetOriginEntitlements`, проверки владения DLC, `server.key` — не делаем: всё «разрешено». Сервер
   определяется своим `serverid.dat`/GUID из каталога инстанса (`--server-id`), ключ не нужен. В реестр расхождений — «намеренно».
5. **Игровой канал клиент↔сервер (К4, аналог Harmony)** — отдельно и напрямую между `EVBots.dll` клиента и сервера (UDP),
   не через бэкенд: так у VU (Harmony RakNet, UDP 25200), и так быстрее. EV Backend только сообщает клиенту адрес/порт канала
   сервера (`Server.JoinServerResponse`).

## 3. Модули Zeus → EV: что делаем

| Модуль Zeus | Что делает у VU | У EV Backend сейчас | В EV |
|---|---|---|---|
| Utility (0xC57E) | InitSession, Ping, ReconfigureCrypto, OnScreenMessage | — | **InitSession, Ping** — да; Crypto — нет (намеренно); OnScreenMessage — да (сообщение игроку) |
| Manager (0xFC57) | RegisterSession/ResumeSession | — | да: регистрация процесса (`client`/`server`), возобновление после обрыва |
| Authentication (0x6B7F) | Login, TokenLogin, CreateAccount, Logout, CheckLink, SetupPassword | аккаунты, токены (`AccountsStore`), вход по personaref | **Login, TokenLogin, CreateAccount, Logout** — да; CheckLink/SetupPassword (Origin) — нет |
| Account (0x3763) | избранное, хранилище аккаунта, Origin-привязка | профили | избранные серверы, хранилище ключ-значение — да; Origin — нет |
| Player (0x81BF) | вход игрока на сервер (по имени/GUID), инфо, удаление | персоны/солдаты | **LoginByGUID/ByName, GetPlayerInfo*, GetPlayers, RemovePlayer*** — да (это `Player:Authenticated` на сервере) |
| Server (0x4B69) | ServerLogin, ключи, FetchServers, JoinServer, Allow/DenyPlayer, Join*Notification, PlayerLeft, переменные игры | `/servers`, `GameRegistry` | **ServerLogin** (без ключа), **FetchServers/GetServerInfo/ServerInfo**, **JoinServer** (+ адрес К4 и список модов), **PlayerJoiningNotification → Allow/DenyPlayer** (допуск как «Player disallowed … by an extension»), **PlayerLeftNotification**, **Set/Update/RemoveGameVariables**; ключи сервера — нет |
| Update (0x49DC) | ревизии клиента VU, блобы | — | нет (обновления VU не нужны); раздача **модов** — через HTTP `ModServer` + хэш-манифест в `JoinServerResponse` |

## 4. Поток входа игрока (цель)

1. Лаунчер: EV `Login` → токен. Запуск BF3 через `evbots-loader.exe` с токеном (Blaze) и адресом EV.
2. `EVBots.dll` клиента: EV `RegisterSession(client, токен)`.
3. `EVBots.dll` сервера при старте: EV `RegisterSession(server)` + `ServerLogin(serverGuid)` + список модов (имена, версии, хэши).
4. Игрок выбирает сервер (браузер = `FetchServers`) → `JoinServer` → бэкенд шлёт серверу `PlayerJoiningNotification`;
   Lua сервера может отказать (`Player:Joining`/хук допуска) → `DenyPlayer` → клиент `JoinDeniedNotification`; иначе
   `AllowPlayer` → `JoinAllowedNotification` с адресом канала К4 и манифестом модов.
5. Клиент забирает недостающие моды (`GET /mods/...` с хэшами), поднимает `ext\client`+`ext\shared`, входит в Blaze-игру.
6. Выход/вылет: `PlayerLeftNotification` серверу; обрыв EV-сессии = выход.

## 5. План работ (по порядку)

1. ✅ **E1 — каркас EV-канала в бэкенде (23.10 ≈19:50):** `Ev\EvFrame.cs` (кадр §2.3, тело UTF-8 без экранирования),
   `Ev\EvService.cs` (Utility.InitSession(version=1)/Ping, Manager.RegisterSession(client|server|launcher),
   Authentication.Login/TokenLogin/Logout на общем `AccountsStore`; до InitSession — отказ; ошибка = type `0xFFFF`
   `{code, message, type}`), `Ev\EvListener.cs` (TCP, `--ev-port`, по умолчанию 7828, 0 — выкл). Самопроверка
   `EV Backend ev-selftest` — всё прошло (кадр байт в байт, порядок сессии, вход по паролю/токену через настоящий TCP,
   токен EV понимает база Blaze); `tdf-selftest` и `protocol-selftest` — прошли. На стенд **не ставилось** (стенд — копия
   `stand\backend`, запущена).
2. ✅ **E2 — Server (23.10 ≈20:00):** ServerLogin (только role=server, GUID инстанса, без ключа; name/address/channelPort/mods),
   FetchServers/GetServerInfo (зарегистрированные по EV серверы), JoinServer (нужен вход; серверу —
   PlayerJoiningNotification{joinId, login, name, personaref}, клиенту сразу {joinId, pending}), AllowPlayer/DenyPlayer{joinId,
   reason} → клиенту JoinAllowedNotification{server} / JoinDeniedNotification{reason}, LeaveServer и обрыв → серверу
   PlayerLeftNotification{reason}, обрыв сервера → игрокам RemovedFromServerNotification. Уведомления — request_id 0.
   `ev-selftest` 29/29. Связь со списком Blaze (`GameRegistry`, `/servers`) — ещё нет (в E3, когда сервер начнёт входить в EV).
3. **E3 — клиент EV в `EVBots.dll`** (общий код для клиента и сервера): соединение, RegisterSession, на сервере — ServerLogin,
   допуск игрока через Lua-события VU.
4. **E4 — лаунчер:** вход через EV, токен в BF3.
5. **E5 — моды по манифесту** (вместо `EVBOTS_CLIENT_MODS`, реестр В10) — вместе с К4.

Связь с клиентской частью: К4 (канал) и E2–E5 идут вместе; К5 (тик/события/лучи клиента) от EV не зависит.
