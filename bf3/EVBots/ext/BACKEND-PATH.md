# BACKEND-PATH — что именно нужно, чтобы игровой сервер создал настоящего игрока

Дата 12.10.2026. Исследование, ничего не запускалось и не менялось.

**Главный вывод (кратко):** настоящего игрока создаёт **TCP-соединение клиента с
игровым портом и игровое рукопожатие поверх него**. Не сообщение бэкенда.
Бэкенд не может создать игрока без клиента — доказательства в разделе 2.
Нужен headless-клиент, но объём его — не Blaze, а **игровой протокол
(CANA/DirtyCast + Spike/Frostbite)**, а это на порядок больше Blaze.

Источники истины по именам полей — `backend\BLASE-FIELDS.md`. Всё, чего в
логах и коде нет, помечено «не найдено».

---

## 1. Кто что шлёт при входе игрока

### 1.1 Полная последовательность из живого прогона

Источник: `RuntimeLog_DESKTOP-0RFO867_server.log:899-1028` (UTC 12.10
02:35:08–02:35:12) — это повторяемый шаблон, в 19 прогонах картина
одинакова. В скобках — время UTC из лога сервера.

| # | Кто → кому | Компонент | Команда | Имя | Ключевое поле | Что делает |
|---|---|---|---|---|---|---|
| 1 | клиент → **бэкенд** | 0x0005 Redirector | 0x01 | getServerInstance | `CLNT` | клиент узнаёт адрес ядра Blaze |
| 2 | клиент → **бэкенд** | 0x0009 Util | 0x07 | preAuth | `CDAT{TYPE,SVCN}`, `CINF{LOC}`, `FCCR` | ответ `SVER`+`CIDS` — дальше можно идти |
| 3 | клиент → **бэкенд** | 0x0001 Auth | 0x32 | silentLogin | `PID` | логин игрока (у сервера — 0x28+0x6E) |
| 4 | **бэкенд** → клиент | 0x7802 UserSessions | 0x02 | UserAdded | `USER{AID,ALOC,ID,NAME}` | обязателен, иначе сессия не established |
| 5 | **бэкенд** → клиент | 0x7802 UserSessions | 0x05 | UserUpdated | `FLGS`,`ID` | ditto |
| 6 | клиент → **бэкенд** | 0x0009 Util | 0x08 | postAuth | — | ответ `PSS/TELE/TICK/UROP` |
| 7 | клиент → **бэкенд** | 0x0004 GameManager | 0x09 | joinGame | `GID`, `GVER="67"` | ответ `GID`+`JGS` |
| 8 | **бэкенд** → клиент | 0x7802 / 0x02, 0x05 | | UserAdded+UserUpdated о **хосте** | | `TryFlushJoinNotify`, `BlazeServices.cs:543-549` |
| 9 | **бэкенд** → **игровой сервер** | 0x0004 | **0x15** | **NotifyPlayerJoining** | `GID`, `PDAT{...STAT=RESERVED(0)}` | сервер: `Adding external player(234471788:KOTuk1) in state(RESERVED) to game(1) roster in slot(1)` |
| 10 | **бэкенд** → **игровой сервер** | 0x0004 | **0x19** | **NotifyPlayerClaimingReservation** | `PDAT{... STAT=ACTIVE_CONNECTING(2)}` | сервер: `RESERVED Player claiming reservation in game(1:LegacyLan) in slot(1)` |
| 11 | **бэкенд** → клиент | 0x0004 | **0x16** | **InitiateConnections** | `GAME{HNET…}`, `PROS`, `QUEU`, `REAS` | клиенту — адрес игры |
| 12 | **игровой сервер** → **бэкенд** | 0x7802 | 0x14 | updateNetworkInfo | `ADDR→VALU→INIP{IP,PORT}` | сервер публикует свой адрес (`BlazeServices.cs:696-716`) |
| 13 | **КЛИЕНТ** → **ИГРОВОЙ СЕРВЕР** | **не Blaze** | — | **CANA-туннель + рукопожатие Frostbite** | битовый протокол | `Game channel[1] for Player(2463881553:KOTuk1) CONNECTING` → `CONNECTED` |
| 14 | **игровой сервер** → **бэкенд** | 0x0004 | **0x1D** | **updateMeshConnection** | `GID`, `TARG=[{FLGS=0,PID,STAT=CONNECTED(2)}]` | **сервер шлёт это, а НЕ бэкенд** |
| 15 | **бэкенд** → игровой сервер | 0x0004 | 0x74 | NotifyGamePlayerStateChange | `GID`,`PID`,`STAT=ACTIVE_CONNECTED(4)` | ответ на 0x1D |
| 16 | **бэкенд** → игровой сервер | 0x0004 | 0x1E | NotifyPlayerJoinCompleted | `GID`,`PID` | сервер: `Player join completed` |
| 17 | игровой сервер (внутренне) | — | — | | | `Player(234471788:KOTuk1) has joined game(1)'s peer network mesh` |
| 18 | игровой сервер (внутренне) | — | — | | | `VeniceServerAuth.cpp(143): Allowing player [Id:'KOTuk1', Native:234471788] to join` |
| 19 | игровой сервер (внутренне) | — | — | | | `ServerConnection.cpp(839): Client [6C C1 F9 0D …] connected! (Id: 1)` |
| 20 | игровой сервер (внутренне) | — | — | | | `timeSync (Id: 1)` — игрок живой |

Отдельно: `BlazeServerUserInfoManager.cpp(159): Assert: (extendedData) Joined
player has no blaze extended data` — следствие того, что уведомление
UserSessions **1** (расширенные данные сессии) наш бэкенд не шлёт
(`BLASE-FIELDS.md:278-280`). Не фатально, но стоит чинить.

### 1.2 Ключевое: кто инициирует соединение с игровым портом

**Игровой сервер.** Не клиент.

```
02:35:08.970  <- async: ID[0], GameManager::NotifyPlayerJoining [0x0004::0x0015]   ← бэкенд
02:35:09.004  <- async: ID[0], GameManager::NotifyPlayerClaimingReservation [0x0019] ← бэкенд
02:35:09.03x  [GAME] Initiating c/s host connection for local player(0:n/a) and joining player(234471788:KOTuk1).
02:35:09.03x  [CANA] connectToEndpoint(0, F8D6xxxx)
02:35:09.038  [CANA] Game channel[1] for Player(2463881553:KOTuk1) CONNECTING
02:35:09.637  [CANA] Game channel[1] for Player(2463881553:KOTuk1) CONNECTED
02:35:09.637  -> req: ID[..], GameManager::updateMeshConnection [0x0004::0x001D]    ← СЕРВЕР → БЭКЕНД
```

Адрес, к которому сервер подключается, он берёт **из `PNET` в `PDAT`** —
то есть из поля, которое заполняет **наш бэкенд** по данным
`updateNetworkInfo` клиента: `192.168.0.10:3659`
(`BlazeServerGameManager.cpp(2754): Player direct IP address & port`).
`3659` — это порт CANA-туннеля клиента, он приходит из
`updateNetworkInfo INIP=3232235530:3659` (`backend-live-08.log:308`).

**Важное уточнение против прежней догадки** (`BOT-LOGIN-PLAN.md:241-254`
утверждал «бот поднимает TCP на адрес из HNET»): это **неверно**.
`HNET` в `InitiateConnections` — это адрес сервера, который клиент
использует для своей части. Реальный игровой канал открывает **сервер**
к клиенту, по адресу из `PNET`.

### 1.3 Что делает `GameManager::updateMeshConnection` [0x0004::0x001D]

Направление: **игровой сервер → бэкенд**. Это **запрос** (`-> req:`),
а не уведомление. Отправляется ровно дважды за прогон:

| Момент | `TARG.STAT` | Лог сервера |
|---|---|---|
| после `Game channel CONNECTED` | `CONNECTED (2)` | `01:43:38.886` (`.1.log:996`), `02:35:09.637` |
| после `Game channel DISCONNECTED` | `DISCONNECTED (0)` | `01:34:59.393` (`.2.log:20999`) |

Наш обработчик `BlazeServices.cs:841-925` на `STAT=2` шлёт отправителю
`0x74 {GID,PID,STAT=4}` и `0x1E {GID,PID}` — это подтверждено логом
(`NotifyGamePlayerStateChange STAT=ACTIVE_CONNECTED(4)` в 02:35:09.771,
`NotifyPlayerJoinCompleted` следом).

На `STAT=0` — рассылка `PlayerRemoved 0x28` остальным
(`BlazeServices.cs:881-914`).

### 1.4 Что именно появляется в сервере в момент «Connected players: 16»

Разобрано по `RuntimeLog_DESKTOP-0RFO867_server.log` (пик 01:58:42–01:58:52)
и `.8.log` (18:06:55–18:07:25, тот же сценарий).

```
01:58:42.319  ServerPlayer.cpp(2106): Attaching soldier to player KOTuk1          ← 1 настоящий
01:58:42.949  ServerPlayer.cpp(2106): Attaching soldier to player BOT_Cottatley650
              … ещё 14 BOT_* …                                                  ← 15 модовых
01:58:43.033  ServerTransactionMessageListener.cpp(1488): Assert: (!player) Unknown player: BOT_Landon (onlineId: 234471683)
01:58:52.644  ServerPlayerManager.cpp(592): Connected players: 16, spectators: 0
```

**«16» = 1 настоящий сетевой игрок + 15 игроков, созданных модом
внутри движка.** Это ровно те 15 `BOT_*`, что создаёт `legacyext.dll`
(`g.out:279-353`, «Bridge: очередь — BOT_… создан в движке»).

Контрольная проверка в `.7.log`: `PlayerJoining=0`, `JoinCompleted=0`,
а `Connected players: 15` — 111 строк подряд. То есть **15 игроков в
счётчике при полном отсутствии входа по Blaze** — это чисто модовые
`BOT_*`.

Отсюда же видно, почему клиент их не рисует: наша DLL создаёт
`ServerPlayer`, но **не** порождает сетевого соединения, поэтому
`PointSystem` о них не знает (`PointSystem.cpp(2327): No player info
found for victim: BOT_Landon`) и в клиент они не реплицируются.
Счётчик `Connected players` — это про `ServerPlayerManager`, а не про
сеть. **Он не является доказательством сетевого пути.**

Дополнительно: при уходе настоящего игрока счётчик падает 16 → 15
(`.8.log:21095`, `01:58`-прогон: `02:00:52.643 Connected players: 15`),
то есть 15 — это ровно модовые боты без сетевого клиента.

---

## 2. Может ли бэкенд сам создать настоящего игрока — доказательства

**Ответ: нет. Не может. Обязателен реальный клиент, соединяющийся с
игровым портом.**

### Доказательство 1 — уведомление без соединения не даёт игрока

`NotifyPlayerJoining 0x15` и `NotifyPlayerClaimingReservation 0x19`
сервер обрабатывает мгновенно (`BlazeServerGameManager.cpp(2641)`), но
это только **запись в ростер**. Реальный игрок появляется позже:

```
02:35:08.974  Player joining game. Name: KOTuk1 …        ← запись в ростер
02:35:09.637  Game channel[1] CONNECTED                   ← сеть (клиент)
02:35:09.771  Player join completed. Name: KOTuk1         ← вот теперь игрок
```

Между 08.974 и 09.771 проходит **~0.8 с**, и ровно в этом окне логин
показывает `Game channel[1] CONNECTING → CONNECTED`. Уведомление само по
себе не создаёт `ServerConnection`.

### Доказательство 2 — счётчик игроков не зависит от Blaze

`.7.log`: `PlayerJoining=0`, `JoinCompleted=0`, но `Connected players: 15`
111 строк подряд. Blaze-уведомлений не было — игроки есть. Значит
`ServerPlayerManager` пополняется **не** через Blaze, а через внутренние
вызовы (наша DLL). Сетевого пути у них нет, клиент их не видит.

Обратное: `.13.log` и `.18.log` — `PlayerJoining=0`, `JoinCompleted=0`,
`Connected players` = 0. Полная корреляция Blaze-входа и появления
настоящего игрока.

### Доказательство 3 — `ServerConnection` ровно один

Во всём прогоне с игроком — **единственное** соединение с игрой:

```
ServerConnection.cpp(828): ServerConnection : Connecting... (Id: 1)
ServerConnection.cpp(839): Client [6C C1 F9 0D 00 00 00 00 ] connected! (Id: 1)
```

Один `Id: 1` на одного настоящего игрока. 15 модовых ботов не дают ни
одного `ServerConnection` — у них нет `Connection`, только объект игрока.

### Доказательство 4 — сетевой путь проходит через отдельные слои

В логе видно три разных слоя, и Blaze-уведомления их не заменяют:

| Слой | Чем реализован | Что в логе |
|---|---|---|
| Blaze-меш (ростер, слоты) | CANA/DirtyCast, `0x15`/`0x19`/`0x1D` | `NotifyPlayerJoining`, `updateMeshConnection` |
| Управление игроком | `BlazeServerGameManager` | `VeniceServerAuth.cpp(143) Allowing player … to join` |
| Игровая сущность | `ServerPeer` / `ServerConnection` / `Spike` | `Client [… ] connected! (Id: 1)`, `timeSync` |

`BlazeServerUserInfoManager.cpp(159)` даже ругается ассертом
(`Joined player has no blaze extended data`) — потому что Blaze-часть
у нас неполна, а игру это не сломало. То есть Blaze — необходимое, но
**не достаточное** условие.

### Доказательство 5 — бэкенд физически не имеет сокета к игре

В коде бэкенда **единственный** `Socket` — слушатель Blaze
(`BlazeListener.cs:38`). Ни `TcpClient`, ни `Connect()` в
`Blaze/`, `Data/`, `Http/`, `ServeCommand.cs`, `Program.cs` — **не
найдено**. Бэкенд ни к порту 25300, ни к CANA-туннелям не подключается
и не может: он не реализует игровой протокол.

### Доказательство 6 — обратное направление не работает

Из `UpdateMeshConnectionCommand` эмулятора и нашего кода: `0x1D` —
**запрос от сервера**. Если бы бэкенд сам отправлял `0x1D` со `STAT=2`,
это был бы подлог: сервер всё равно ждёт реального `Game channel`.
Ни в одном из 19 логов нет ни одного входящего `0x1D` от бэкенда — есть
только исходящий `-> req:` от сервера.

### Итог по разделу 2

```
Бэкенд → Blaze-уведомления   ⇒  запись в ростер (слот, имя, PID)
Клиент  → CANA+Spike+Frostbite ⇒  ServerConnection ⇒ ServerPlayer ⇒ видимость
```

Первое без второго даёт «игрока в ростере, которого никто не видит» —
ровно то, что мы уже производим модом.

---

## 3. Если нужен headless-клиент: объём и что уже готово

### 3.1 Главное: это две разные работы, и вторая в разы больше

| Слой | Что нужно | Оценка |
|---|---|---|
| **A. Blaze-клиент** | заговорить с нашим бэкендом по Blaze | **мало, в основном готово** |
| **B. Игровой клиент** | CANA-туннель + Spike/Secure + Frostbite (StreamControl, сжатие, level-load, timeSync) | **очень много, ничего не готово** |

Слой B — это по сути `bf3.exe` без рендера. Оценка: **тысячи строк и
недели реверса протокола**, формат которого **не зафиксирован ни в одном
источнике проекта** (`BOT-LOGIN-PLAN.md:317-331` это уже фиксировал и
ничего не изменилось).

### 3.2 Слой A — Blaze-клиент: что уже есть в репозитории

| Что | Где | Состояние |
|---|---|---|
| Сборка/разбор кадра Blaze | `Blaze\BlazeFrame.cs` (6.8 КБ) | **готов** |
| Кодек TDF (encode/decode) | `Tdf\TdfCodec.cs` (17 КБ), `Tdf\Tdf.cs` (10 КБ) | **готов** |
| Транслитерация имён | `Tdf\AsciiSafe.cs` | **готов** |
| **TLS-клиент** | `Tests\ProtocolRoundTrip.cs:127-144` (`SslStream` + `AuthenticateAsClient`) | **готов, переиспользовать** |
| Отправка запроса + чтение ответа | `ProtocolRoundTrip.cs:383-399` (`Expect`), `:263` (`ExpectRaw`) | **готов** |
| Пропуск уведомлений в потоке | `ProtocolRoundTrip.cs:358-380` (`ReadReply`, до 16 пропусков) | **готов** |
| Логин персоны | `ProtocolRoundTrip.cs:235-259` (`loginPersona PNAM=`) | **готов, работает** |
| Номера компонентов и команд | `Blaze\BlazeComponents.cs` | **готов, полон** |
| Формы payload'ов | `Blaze\Payloads.cs` (33 КБ) | серверные; клиентские придётся писать заново |
| Заведение личности бота по HTTP | `POST /profiles`, `POST /accounts/ensure` (`Http\WebHost.cs:886-940`, `:598-601`) | **готов** |

**Вывод по слою A: писать с нуля нечего.** Нужен новый сценарий
(например `RunBotSession` рядом с `RunSession`), который повторит
шаги 1–11 из таблицы раздела 1.1. Это **дни работы**, не недели.

### 3.3 Слой B — игровой клиент: чего нет

| Что | Статус |
|---|---|
| CANA (Connectivity API) — туннель, `ConnApiAddClient2`, handshake | **не найдено** ни в одном источнике проекта |
| DirtyCast / DirtySock — `DirtySockSocketManager` на сервере есть, клиентской стороны нет | **не найдено** |
| Spike `Secure` — `onAcceptConfirmation`, `RemoteSessionKey` | в логе есть только серверная строка, формат **не найдено** |
| Frostbite `StreamControl` / `StreamManagerMove` / сжатие | `Compression info confirmed. 16 base lines` в логе есть, формат **не найдено** |
| `cmdLinkLevel` / `onAckLevelLinked` / `timeSync` | имена в логе есть (`ServerConnection.cpp:1525/1683/1552`), формат **не найдено** |
| Трафик порта 25300 | **не снят** — в `netmon.log` (593 строки, окно ~26 ч) порта 25300 нет вообще |

Про `netmon.log` важное наблюдение: клиент **не открывает** соединение к
25300 — ни TCP, ни UDP. Он открывает только `127.0.0.1:42127` (redirector)
и `127.0.0.1:10041` (Blaze) — плюс `192.168.0.1:52869` (последний — не
игровой сервер; сокет появляется ещё до входа и висит `CLOSE_WAIT`).
Это **косвенное подтверждение** вывода раздела 1.2: игровой канал
открывает сервер к клиенту, поэтому в netstat клиента его и нет.

### 3.4 Что нужно, чтобы закрыть слой B

1. Захват трафика CANA-туннеля. Инструменты уже есть в проекте:
   `trash\2026-10-11\connect_trace.ps1` (ломает `ws2_32.connect` через
   x64dbg/WinDbg), `netmon.ps1` (netstat-сканер). Нужен **tcpdump/Wireshark
   на loopback** с фильтром по порту 3659 (порт CANA-туннеля клиента) —
   это единственный способ снять байты, которых нет ни в одном логе.
2. Реверс клиентской стороны CANA в `bf3.exe`.
3. Реверс Spike/Frostbite поверх туннеля.

Пока шаг 1 не сделан, любые оценки по слою B — гадание.

---

## 4. Что можно сделать в `Legacy\backend` как первый шаг

Рекомендация: **не трогать `UpdateMeshConnection` и не пытаться «создать
игрока» с серверной стороны.** Это ловушка: `0x1D` — запрос сервера, и
наш обработчик уже зеркалит эталон. Единственное изменение с
наибольшим эффектом — **сделать Blaze-часть настолько надёжной, что
headless-клиент можно будет писать и отлаживать без игры**.

### Шаг 1 (рекомендуемый): вынести Blaze-клиента в переиспользуемый класс

**Что:** перенести клиентскую половину `ProtocolRoundTrip.cs`
(строки 121–341) в отдельный файл `Blaze\Client\BlazeClient.cs`
с публичным API: `ConnectAsync(host, port)`, `ExpectAsync(comp, cmd,
request)`, `ReadNotificationAsync()`. Убрать зависимость от `ServeConfig`.

**Почему это самое важное и самое маленькое:**
- дублирования нет — код уже написан и работает;
- без этого шага слой B нечем проверять: любой баг в Blaze-клиенте
  неотличим от бага в CANA;
- даёт инструмент для всех будущих экспериментов.

**Размер:** перенос существующего кода, ~1 день. Риск близок к нулю —
это рефакторинг без изменения поведения.

### Шаг 2: добавить сценарий `RunBotSession`

Полный путь шагов 1–11 из таблицы 1.1: redirector → preAuth →
silentLogin → UserAdded/UserUpdated → postAuth → joinGame → принять
5 уведомлений (`UserAdded`, `UserUpdated`, и дождаться `0x16`) →
убедиться, что в `GAME.HNET` валидный `IP`/`PORT`.

**Проверка, которая важнее всего:** `SilentLogin` должен вернуть
`SESS.PDTL.PID`, и этот PID обязан совпасть с тем, что потом придёт
в `PlayerClaimingReservation`. Расхождение PID — самая частая причина
того, что сервер не сопоставляет mesh-подключение с игроком.

### Шаг 3 (небольшое, чинит ассерт в логе сервера): начать слать UserSessions-1

`BlazeServerUserInfoManager.cpp(159): Assert: (extendedData) Joined
player has no blaze extended data`. Причина названа в
`BLASE-FIELDS.md:278-280`: `Payloads.SessionExtended`
(`Payloads.cs:531-542`) объявлен, но **нигде не вызывается**.

Формат подтверждена источниками: `DATA{ADDR,BPS,CTY,DMAP,HWFG,QDAT,
UATT,PSLM}`, `USID`; `PSLM` — список из пяти одинаковых `268374015`
(`New-Blaze-Emulator\Notifications\UserSessionsComponent\UserSessionExtendedDataUpdateNotification.cs:14-15,48-51`).
Непротиворечиво и `ULST` (`TDF_TYPE_BLAZE_OBJECT_ID`, Vector3 с ID игры) —
этого поля у нас нет.

**Размер:** ~15 строк. **Эффект:** уходит ассерт из лога сервера и
появляется `BlazeServerUserInfoManager`-запись игрока — то есть ещё
один слой Blaze, который сейчас отсутствует. Не создаст игрока, но
убирает самый заметный «недоделанный» след.

⚠️ Риск: клиент сейчас работает без этого уведомления. Сначала —
внести с флагом, откатать одним ключом.

### Чего делать **не** надо

| Не делать | Почему |
|---|---|
| Отправлять `updateMeshConnection` от бэкенда | `0x1D` — запрос сервера; подделка не создаст `ServerConnection` (доказательство 6) |
| Пытаться добавить игрока кодом на стороне `BlazeServices` | Уже доказано: без сетевого соединения — только запись в ростер (доказательства 1–3) |
| Считать `Connected players` признаком успеха | Счётчик включает модовых игроков без сети (раздел 1.4) |
| Пытаться «дописать» Blaze-поля, которых нет в `BLASE-FIELDS.md` | Правило проекта №10; 14 выдуманных меток уже стоили сутки |

---

## 5. Честный статус

| Вопрос | Ответ | Обоснование |
|---|---|---|
| Может ли бэкенд создать игрока без клиента? | **Нет** | раздел 2, 6 доказательств |
| Кто открывает игровой канал? | **Игровой сервер**, к клиенту по `PNET` | 1.2, `CANA connectToEndpoint` |
| Что делает `updateMeshConnection 0x1D`? | Запрос **сервер → бэкенд**; `STAT=2` → бэкенд шлёт `0x74`+`0x1E` | 1.3 |
| Что значит «Connected players: 16»? | 1 сетевой + 15 модовых, сети у ботов нет | 1.4 |
| Нужен ли headless-клиент? | **Да** | 2 |
| Сколько это? | Blaze — дни (готово ~80%); игра — недели, нужен захват трафика | 3 |
| Первый шаг в `backend` | Вынести Blaze-клиента в класс + `RunBotSession` + UserSessions-1 | 4 |

**Оговорка о надёжности.** Слой B не оценён точнее «очень много»
намеренно: формат CANA/Spike/Frostbite **не найдено** ни в одном
источнике проекта. Любая более точная цифра была бы выдумкой.