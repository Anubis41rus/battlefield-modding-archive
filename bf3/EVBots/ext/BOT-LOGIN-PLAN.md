# BOT-LOGIN-PLAN — как бот должен заходить в игру

Дата 11.10.2026. План составлен по трём источникам, ничего не запускалось:
наш бэкенд, два чужих эмулятора, логи прогонов с реальным клиентом
(`C:\Games\trash\2026-10-11\`).

Сокращения: `наш:` = `C:\Games\Legacy\backend\src\Legacy.Backend\`,
`NBE:` = `C:\Games\Legacy\New-Blaze-Emulator\`,
`bk5.out` / `probe5.out` / `g.out` / `netmon.log` — логи в
`C:\Games\trash\2026-10-11\`.

---

## 0. Ответ на исходный вопрос (коротко)

| Вопрос | Ответ |
|---|---|
| Есть ли в эмуляторах готовое создание игрока? | **Нет.** Ни `ServerPlayerCreate`, ни `createPlayer`, ни «соло», ни списка игроков. Ни одного совпадения по обоим деревьям. Игрок = `Client` + `User` из YAML + запись в `Game.Slots` (`NBE:\Base\Client.cs:13-48`, `NBE:\Base\Configuration.cs:31-38`, `NBE:\GameManager\Game.cs:50`) |
| Полная ли последовательность входа в логах? | **По составу команд — да. По спискам полей — только для выделенного сервера.** Для игрока (`probe5.out`, `g.out`) уровень лога INFO, списков полей нет |
| Что можно сделать без входа в игру? | **Всю Blaze-часть.** У нас уже есть готовый Blaze-клиент без игры — `наш:\Tests\ProtocolRoundTrip.cs:121-341` |
| Нужен ли боту отдельный `login` + `loginPersona`? | **Да, полная сессия.** Но можно либо `Login`+`LoginPersona` (как выделенный сервер), либо один `SilentLogin` (как реальный игрок) |
| Есть ли отдельный барьер? | **Да — игровой протокол на порту 25300.** Из имеющихся источников он не проходится (раздел 6) |

---

## 1. Уровни: где кончается Blaze

```
[1] наш бэкенд (Blaze)          ← ТУТ можно всё отладить без игры
        │  Redirector 42127 → ядро Blaze 10041
        │  ответ клиенту: GAME.HNET = ip:25300
        ▼
[2] ИГРОВОЙ ПРОТОКОЛ, порт 25300   ← ОТДЕЛЬНЫЙ БАРЬЕР, из источников не проходится
        │  клиент шлёт UpdateMeshConnection(TARG, STAT=2)
        ▼
[3] движок выделенного сервера: ServerPlayerManager::createPlayer (RVA 0xA644B0)
```

Уровни 2–3 относятся к живой игре. **План ниже целиком про уровень 1** —
плюс честное описание того, что известно про уровни 2–3.

---

## 2. Что уже готово у нас (не надо изобретать)

| Что | Где | Состояние |
|---|---|---|
| Кодирование/разбор кадра Blaze | `наш:\Blaze\BlazeFrame.cs` | готово, работает в тестах |
| Кодек TDF | `наш:\Tdf\TdfCodec.cs` | готово (счётчики — varint, это верно) |
| Транслитерация имён | `наш:\Tdf\AsciiSafe.cs:51` | готово |
| TLS-сервер Blaze | `наш:\Blaze\BlazeListener.cs`, `BlazeTls.cs` | готово, поднимается сам |
| **TLS-КЛИЕНТ для своих тестов** | `наш:\Tests\ProtocolRoundTrip.cs:127-144` | **готов, переиспользовать** |
| Отправка/разбор запроса в тесте | `наш:\Tests\ProtocolRoundTrip.cs:383-399` (`Expect`), `:263` (`ExpectRaw`) | готов |
| Пропуск уведомлений при чтении | `наш:\Tests\ProtocolRoundTrip.cs:358-380` (`ReadReply`) | готов |
| Обработка `loginPersona` с созданием персоны | `наш:\Blaze\BlazeServices.cs:421-449` | **готов — создаёт профиль по имени (`:430`)** |
| Обработка `joinGame` + выдача слота | `наш:\Blaze\BlazeServices.cs:795-821` | готов |
| Выдача адреса игры | `наш:\Blaze\BlazeServices.cs:522-558` (`TryFlushJoinNotify`), `:983-1026` (`InitiatePayload`), `наш:\Blaze\Payloads.cs:753-764` (`HnetListClient`) | готов |
| Завершение рукопожатия | `наш:\Blaze\BlazeServices.cs:841-911` (`UpdateMeshConnection`) | готов |
| Заведение личности бота по HTTP | `POST /profiles` → `наш:\Http\WebHost.cs:886-940`; `POST /accounts/ensure` → `WebHost.cs:598-601` | **готов** |

**Вывод: писать с нуля нечего.** Нужен только новый сценарий в
`ProtocolRoundTrip.cs` (или отдельная консольная утилита на тех же классах).

---

## 3. Пошаговый план входа бота

Ниже — ровно та последовательность, которую делает **настоящий клиент**,
взят из логов. Ссылки двух видов: `наш:` — где брать формат у нас,
`NBE:` — независимое подтверждение из эмулятора.

### Шаг 0. Завести личность (без сети, без игры)

```http
POST /profiles  {"name":"BOT_1","cloneFrom":234471775}
→ 201 {"personaref":234471793,"name":"BOT_1","hasProgress":true}
```
`наш:\Http\WebHost.cs:886-940`; `cloneFrom` обязателен, иначе клиент не
покажет ранг (комментарий `:916-917`).

Если нужен вход через `SilentLogin` — дополнительно:
```http
POST /accounts/ensure {"login":"BOT_1","password":"...","name":"BOT_1"}
```
`наш:\Http\WebHost.cs:598-601`. **Без этого `SilentLogin` будет отклонён** —
`наш:\Blaze\BlazeServices.cs:396-408` (специально сделано, чтобы не плодить
мусорные профили).

> Если идти через `loginPersona` (шаг 3б), `accounts/ensure` **не нужен**:
> профиль создастся сам при первом запросе.

### Шаг 1. Redirector — `getServerInstance`

Компонент **5** (Redirector), команда **1**. TLS на порт 42127.

Запрос (реальный клиент шлёт 13 полей, `bk5.out:403`):
`BSDK, BTIM, CLNT, CLTP, CSKU, CVER, DSDK, ENV, FPID, LOC, NAME, PLAT, PROF`
— нам достаточно `CLNT` (`наш:\Blaze\BlazeComponents.cs:89`).

Ответ: `ADDR` (union, активный член **`0`** = `XboxClientAddress`) →
`VALU{HOST, IP=0, PORT}`, `AMAP`, `MSGS`, `NMAP` (пустые), `SECU=1`, `XDNS=0`.
Формат: `наш:\Blaze\Payloads.cs:554-567`. Подтверждение:
`NBE:\Commands\RedirectorComponent\GetServerInstanceCommand.cs:15-28`.

Клиент идёт на `HOST:PORT` — то есть на ядро Blaze (10041).

### Шаг 2. `preAuth`

Компонент **9** (Util), команда **7**.

Запрос: `CDAT` (структура: `TYPE`, `SVCN`), `CINF` (структура: `LOC`),
**`FCCR`** — последнее в нашем документе не было, реально приходит
(`bk5.out:428`). Формат: `наш:\Blaze\BlazeServices.cs:168-193`.

Ответ (15 полей, критичны `SVER` и `CIDS`):
`ANON, ASRC, CIDS, CNGN, CONF{CONF}, INST, MINR, NASP, PILD, PLAT, PTAG,
QOSS{BWPS{PSA,PSP,SNA}, LNP, LTPS, SVID}, RSRC, SVER`
— `наш:\Blaze\Payloads.cs` (`PreAuthReply`), подтверждение
`NBE:\Commands\UtilComponent\PreAuthCommand.cs:34-80`.

`SVER = "Blaze 3.15.08.0 (CL# 1060080)"` — `наш:\Blaze\Payloads.cs:38`,
эталон `NBE:\...\PreAuthCommand.cs:79`.

### Шаг 3. Вход — **два варианта**

#### Вариант А. `SilentLogin` (0x32) — как заходит настоящий игрок

Компонент **1** (Authentication), команда **0x32**, запрос — одно поле **`PID`**.

Факт из лога реального игрока: `silentLogin PID=234471788` —
`probe5.out:226`, `g.out:235`. Это **основной путь входа игрока**;
`Login`+`LoginPersona` использует выделенный сервер (`bk5.out:446,455`).

Ответ: `AGUP, LDHT, NTOS, PCTK, PRIV, SESS{BUID,FRST,KEY,LLOG,MAIL,PDTL{…},UID},
SPAM, THST, TSUI, TURI` — `NBE:\Commands\AuthenticationComponent\SilentLoginCommand.cs:18-47`.
Обработка у нас: `наш:\Blaze\BlazeServices.cs:381-419`.

**Плюс:** один запрос вместо двух, `MAIL`/`PASS` не нужны.
**Минус:** персона обязана существовать заранее (шаг 0, `accounts/ensure`).

#### Вариант Б. `Login` (0x28) + `LoginPersona` (0x6E) — как заходит сервер

`Login`, компонент 1, команда **0x28**. Запрос реального клиента — пять полей
`DVID, MAIL, PASS, TOKN, TYPE` (`bk5.out:446`), нам нужен `MAIL`+`PASS`.
Ответ: `LDHT, NTOS, PCTK, PLST[{DSNM,LAST,PID,STAS=2,XREF,XTYP}], PRIV, SKEY,
SPAM, THST, TSUI, TURI, UID` — `NBE:\...\LoginCommand.cs:30-51`.

`LoginPersona`, компонент 1, команда **0x6E**. Запрос — **`PNAM`** (имя солдата).
Ответ: `BUID, FRST, KEY, LLOG, MAIL, PDTL{DSNM,LAST,PID,STAS=2,XREF,XTYP}, UID`
— `NBE:\...\LoginPersonaCommand.cs:15-32`.

> ⚠️ `PNAM` эмулятором **не подтверждён** — он тело запроса не читает
> (`NBE:\...\LoginPersonaCommand.cs:11-13`). Значение взято с живого клиента.
> Наш бэкенд его читает: `наш:\Blaze\BlazeServices.cs:423`.

**Плюс варианта Б:** профиль создаётся сам — `store.Create(name)`
(`наш:\Blaze\BlazeServices.cs:430`), `accounts/ensure` не нужен.
Это уже используется в наших собственных тестах:
`наш:\Tests\ProtocolRoundTrip.cs:235-259`.

### Шаг 4. `UserAdded` + `UserUpdated` — обязательны после входа

Оба варианта обязаны получить от сервера **два уведомления**:

| Уведомление | Компонент/номер | Payload |
|---|---|---|
| UserAdded | UserSessions (0x7802) / **2** | `DATA{…}`, `USER{AID,ALOC,ID,NAME}` |
| UserUpdated | UserSessions / **5** | `FLGS`, `ID` |

Подтверждение из эмулятора: `NBE:\...\LoginPersonaCommand.cs:36-37` шлёт их
сразу после `LoginPersona`, и `SilentLoginCommand.cs:51-52` — после
`SilentLogin`. Наш код: `наш:\Blaze\BlazeServices.cs:564-575`
(`NotifyUserAdded`).

Формат: `наш:\Blaze\Payloads.cs` (`UserAdded`, `UserUpdated`);
эмулятор — `NBE:\Notifications\UserSessionsComponent\UserAddedNotification.cs:13-43`.

> Уведомление **UserSessions 1** (расширенные данные сессии) мы **не шлём**
> вообще: `Payloads.SessionExtended` (`наш:\Blaze\Payloads.cs:531-542`)
> объявлен, но нигде не вызывается. Клиент работает и без него — значит поле
> необязательное.

### Шаг 5. `postAuth`

Компонент 9, команда **8** (не 2! Команда 2 — это `ping`).

Запрос пустой (`bk5.out:470`). Ответ — четыре структуры
`PSS, TELE, TICK, UROP` — `наш:\Blaze\BlazeServices.cs:204-212`,
`наш:\Blaze\Payloads.cs`, эмулятор
`NBE:\Commands\UtilComponent\PostAuthCommand.cs:15-55`.

> ⚠️ Лог `bk5.out:557` печатает `postAuth cmd=2` — это **старая сборка**
> нашего бэкенда, там баг. В `bk5.out:558` на команду 2 уходит 247-байтный
> PostAuth-payload. Эталон (`9` байт на `0009:0002`, `411` на `0009:0008`)
> см. `наш:\BLASE-FIELDS.md:51-55`. Свежий лог `g.out:212` уже без `cmd=`.
> **Не делать выводов о cmd 2 из `bk5.out`.**

### Шаг 6. `joinGame`

Компонент 4 (GameManager), команда **9**. Запрос: `GID` + `GVER`
(реальный клиент: `joinGame GID=1 GVER="67"`, `g.out:237`).

Ответ: `GID` + **`JGS`** (не `SNUM`) — `наш:\Blaze\Payloads.cs:584-588`,
эмулятор `NBE:\Commands\GameManagerComponent\JoinGameCommand.cs:25-26`.

`GID` нужно узнать заранее: он приходит в `createGame` на серверной сессии
(`bk5.out:506-507`) либо виден в `GET /servers` (`наш:\Http\WebHost.cs:277`).

**Слот выдаёт наш бэкенд**, не эмулятор:
`наш:\Blaze\GameRegistry.cs:55-68` (`Attach`). Первый игрок — слот **1**
(хост занимает 0). Независимое подтверждение порядка: у эмулятора список
слотов предсоздан с фиктивным элементом `Slots = { 0 }`
(`NBE:\GameManager\Game.cs:54`), поэтому `FindIndex` первого игрока даёт 1
(`NBE:\...\JoinGameCommand.cs:34-35`).

### Шаг 7. Уведомления входа — адрес игры

Наш бэкенд шлёт их отложенно (`JoinNotifyDue = +20 c`,
`наш:\Blaze\BlazeServices.cs:819`), отправка в `TryFlushJoinNotify`
(`:522-558`). **Бот-клиент обязан их принять и не закрывать соединение.**

| Порядок | Уведомление | Кому | Payload | Где формируется |
|---|---|---|---|---|
| 1 | UserAdded | боту | `USER{…}` про хоста | `BlazeServices.cs:543-546` |
| 2 | UserUpdated | боту | `FLGS`, `ID` | `:547-549` |
| 3 | PlayerJoining **0x15** | хосту | `GID`, `PDAT{…}` | `GameRegistry.cs:134-137` |
| 4 | PlayerClaimingReservation **0x19** | хосту | `GID`, `PDAT{… STAT=2}` | `GameRegistry.cs:144-148` |
| 5 | **InitiateConnections 0x16** | **боту** | `GAME{HNET…}, PROS, QUEU, REAS` | `BlazeServices.cs:556-557`, `Payloads.cs:686-742` |

**Шаг 5 — самый важный: в `GAME.HNET` лежит адрес игрового сервера.**
`наш:\Blaze\Payloads.cs:708` → `HnetListClient(ip, port)` (`:753-764`),
`ip/port` = `cfg.GameServerIp`/`cfg.GameServerPort` (по умолчанию
`127.0.0.1:25300`, `наш:\ServeCommand.cs:185,261`).

Формат `HNET` — тот особый случай TDF: список из одной записи, а на проводе
`02 <EXIP> <INIP> 00`. Наша трактовка: байт `02` — **номер члена union
`IPPairAddress = 2`**, а не стаб (`наш:\Blaze\Payloads.cs:744-752`).
Эмулятор трактует иначе (флаг `Stub`, `NBE:\Blaze\TdfEncoder.cs:163-166`),
но значение совпадает. **Ориентируемся на наше.**

### Шаг 8. Соединение с игрой и возврат

Бот поднимает TCP на адрес из `HNET` и после успешного коннекта шлёт
`UpdateMeshConnection`:

Компонент 4, команда **0x1D**, поля `GID`, `TARG`
(список-структура `[GID, PID, STAT]`), `STAT = 2`.

- Эмулятор: `NBE:\Commands\GameManagerComponent\UpdateMeshConnectionCommand.cs:15-36`.
- Наш разбор: `наш:\Blaze\BlazeServices.cs:841-911`; при `STAT=2` шлём
  `PlayerStateChanged 0x74` + `PlayerJoinCompleted 0x1E` (`:874-878`),
  при `STAT=0` — `PlayerRemoved 0x28` (`:888-910`).

> **Это и есть тот барьер, где нужен игровой протокол.** См. раздел 6.

### Шаги 9+. Что можно пропустить

| Шаг | Обязателен? | Основание |
|---|---|---|
| `fetchClientConfig` (Util 1) | нет | в прогоне игрока не наблюдался (`probe5.out:222-234`) |
| `getTelemetryServer` (Util 5) | нет | там же |
| `updateNetworkInfo` (UserSessions 0x14) | нет | бэкенд терпит отсутствие: `наш:\Blaze\BlazeServices.cs:986-1001` подставит `cfg.GameServerIp` |
| `listUserEntitlements2` (Auth 0x1D) | нет | в прогоне игрока не наблюдался |
| `userSettingsSave` / `LoadAll` (Util 0x0B/0x0C) | нет | там же |
| `getStatGroup` / `getStatsByGroupAsync` (Stats 4 / 0x10) | нет для входа | в прогоне их шлёт сессия **выделенного сервера** (сессия 2), `probe5.out:233-234` |
| `ping` (Util 2) | для живости | каждые 20 с, `bk5.out:556,567,637…` |

---

## 4. Что можно отладить без входа в игру — конкретно

Готовая площадка: `наш:\Tests\ProtocolRoundTrip.cs`. Всё нужное уже есть.

**Предлагаемый сценарий `RunBotSession` (рядом с `RunSession`,
`наш:\Tests\ProtocolRoundTrip.cs:121-341`):**

| Шаг | Что звать | Что проверять | Образец |
|---|---|---|---|
| 1 | `Expect(Redirector, GetServerInstance)` | `ADDR.ACTIVE = 0`, `VALU.PORT > 0` | `:285-306` |
| 2 | `Expect(Util, PreAuth)` | есть `SVER`, `CIDS`, `CONF`, `INST`, `PLAT` | `:173-197` |
| 3 | `Expect(Authentication, LoginPersona, PNAM="BOT_1")` | `BUID`, `MAIL`, `PDTL.DSNM`, `UID` | `:235-259` |
| 4 | прочитать 2 уведомления (UserAdded, UserUpdated) | не пропустить, дождаться обоих | `ReadReply`, `:358-380` |
| 5 | `Expect(Util, PostAuth)` | `PSS`, `TELE`, `TICK`, `UROP` | `:200-214` |
| 6 | `Expect(GameManager, JoinGame, GID, GVER="67")` | ответ `GID`+`JGS` | — |
| 7 | прочитать уведомления до `0x16` | в `GAME.HNET` должен быть `EXIP/INIP` с `PORT=25300` | — |
| 8 | `Expect(GameManager, UpdateMeshConnection, GID, TARG=[[GID,PID,2]])` | дождаться `0x74` + `0x1E` | — |
| 9 | `Expect(Util, Ping)` | `STIM` есть | `:336-339` |

**Что это даст:** полный байт-в-байт эталон цепочки Blaze для бота, без
сборки DLL, без запуска `bf3.exe`, без запуска выделенного сервера.
Это ровно то, ради чего человек просил «не заходить в игру».

**Чего это НЕ даст:** подтверждения, что **настоящий** клиент BF3 примет
имена полей. Наш собственный клиент согласится с любыми именами, которые
понимает наш же бэкенд. Для этого нужен живой клиент — честно.

---

## 5. Порядок работ (что делать дальше)

1. **Вынести клиентскую половину `ProtocolRoundTrip` в отдельный файл**
   (например `наш:\Tests\BlazeClientHarness.cs`), чтобы её можно было
   переиспользовать и в тесте, и в отдельной утилите бота. Сейчас она
   внутри `RunSession` (`наш:\Tests\ProtocolRoundTrip.cs:121-341`) и
   жёстко заточена под `ServeConfig`.
2. **Добавить `RunBotSession`** по таблице из раздела 4.
3. **Прогнать без игры**, убедиться, что слот 1, `HNET` = 25300,
   `UpdateMeshConnection` доезжает.
4. **Только потом** связывать с игрой: бот поднимает соединение на 25300.
5. Отдельно (когда будет живой прогон): проверить `NQOS` против `NQQS`
   (расхождение помечено в `наш:\BLASE-FIELDS.md`, раздел `GAME`), а также
   снять `netmon` в прогоне с игроком, чтобы поймать порты 25300 и
   48377/48378 — в `netmon.log` их не было.

---

## 6. Барьер «игровой протокол (порт 25300)» — честно

**Из имеющихся источников он не проходится.** Что известно и что нет:

| Что | Источник | Состояние |
|---|---|---|
| Клиент после `HNET` идёт на `ip:25300` | `netmon.log` — порта 25300 в окне съёма **нет**; косвенно — `bk5.out:480`, где клиент сам прислал `INIP=10.6.7.1:25300` через `updateNetworkInfo` | адрес известен, трафик не снят |
| После коннекта клиент шлёт `UpdateMeshConnection(TARG, STAT=2)` | эмулятор `NBE:\...\UpdateMeshConnectionCommand.cs:24-36`; наш код `наш:\Blaze\BlazeServices.cs:841-911` | известно из протокола Blaze, но в логах прогона **не наблюдалось** |
| Формат сообщений игрового протокола | — | **не зафиксирован ни в одном источнике из этого прохода** |
| Эмуляторы знают про игровой протокол? | — | **нет**: оба заканчиваются на `InitiateConnections`/`UpdateMeshConnection` |
| `SPAWN-RESEARCH.md` про порт 25300 | — | **ничего**: он описывает код внутри движка (`createPlayer` RVA `0xA644B0`, `addToArray` `0xA6CA00`, `spawnSoldier` `0x9DBBD0`) |

**Что нужно, чтобы закрыть барьер:** отдельный захват трафика 25300 в прогоне
с зашедшим игроком (netmon/Wireshark с фильтром по порту) — иначе формат
придётся восстанавливать так же, как Blaze, с нуля.

---

## 7. Правила (из `наш:\BLASE-FIELDS.md`, повторяю коротко)

1. **Не выдумывать имена полей.** Если поля нет в источниках — искать дальше.
2. **Не брать значения-примеры из `New-Blaze-Emulator` как эталон:**
   у него сломаны счётчики list/map (`NBE:\Blaze\TdfEncoder.cs:161`),
   расширенный размер (`NBE:\Base\Client.cs:59,71`),
   `GetStatGroup` и `GetStatsAsyncNotification` — заглушки
   (`NBE:\...\GetStatGroupCommand.cs:15-20`,
   `NBE:\...\GetStatsAsyncNotification.cs:13-16`), уникальность слотов не
   обеспечена (`NBE:\...\JoinGameCommand.cs:34`).
3. **`openBlase`** годится только как второй источник по **формату кадра**
   (`main.cpp:319-366`) и по чтению счётчика как varint
   (`Tdf\TdfList.cpp:35`). Имена полей и компонентов он не знает вообще.
4. **Всё, что проверяемо без игры, — проверять через `ProtocolRoundTrip`,
   а не заходом в игру.**
