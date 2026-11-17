# BLASE-FIELDS — настоящие имена полей протокола Blaze

Дата 2026-10-08. Это спецификация ответов и уведомлений нашего бэкенда
`Legacy.Backend`. Каждое поле взято из двух независимых источников, ничего
не выдумано.

## Источники

| Источник | Что даёт |
|---|---|
| `Legacy\New-Blaze-Emulator\Commands\*` и `Notifications\*` | полные payload'ы с настоящими именами полей (публичная реализация протокола Blaze 3.x) |
| `C:\Games\re\backend-utf16.txt` — строки рабочего `BF3LegacyLan.Backend.exe` | подтверждение: реальные метки видны в строках лога |
| `C:\Games\trash\2026-10-11\bk5.out`, `probe5.out`, `g.out` | запросы **настоящего** клиента BF3 с полными списками полей (`bk5.out:403,428,446,455,479,497,506,521,530,542`) |
| `Legacy\ext\BLAZE-CROSSCHECK.md`, `BOT-LOGIN-PLAN.md` | сверка с `New-Blaze-Emulator` и `openBlase` от 11.10.2026 |

**Обновлено 11.10.2026** — сверка с двумя чужими эмуляторами. Итог:
все проверенные имена полей совпали, а найденные расхождения внесены ниже
как явные пометки. Полный разбор — `Legacy\ext\BLAZE-CROSSCHECK.md`.

**Оговорка по New-Blaze-Emulator:** имена полей — источник годный, а его
TDF-кодек и часть хендлеров **неверны**: счётчик list/map пишется сырым
байтом (`(byte)tdf.List.Count`) вместо varint (при ≥128 элементах данные
съедаются и всё смещается — наш баг 1432→344 был именно об этом, см.
MODLOG 09.10), а `GetStatsAsyncNotification` там — пустая заглушка.
Эталон по кодеку — `C:\Games\re\appsrc\BF3LegacyLan.Backend.Tdf\TdfWriter.cs` /
`TdfReader.cs` рабочего бэкенда (счётчик = `WriteVarInt`/`ReadVarInt`).

Подтверждённые рабочим бэкендом метки (совпали при разборе его строк):
`ADDR, ADMN, AGUP, ARC4, ASRC, ATTR, AUTH, BLOB, CFID, CIDS, CINF, CLNT,
CNGN, CNTX, CONF, DATA, EID, ETAG, GID, GSTA, GVER, HNET, HOST, NAME, PID,
PNAM, PORT, SVCN, TELE, TICK, TYPE, VALU`.

**Важно:** первый набор ответов был написан «по памяти», и 14 меток в нём
оказались выдуманными (`SVR`, `SEC`, `BAS`, `LURI`, `EVAL`, `LOCL`, `TCL`,
`OPTO`, `SETTINGS`, `STATS`, `SNAM`, `SNUM`, `IPAD`, `IPV`). Клиент по ним
ответ не примет. Все заменены на настоящие — см. ниже.

---

## Util

### FetchClientConfig (команда 1)

Запрос: `CFID` (строка).

Ответ — карта `CONF` (String → String) с двумя ключами:

| Ключ | Содержимое |
|---|---|
| `Achievements` | коды достижений через запятую: `ACH32_00,…,XP5ACH05_00` |
| `WinCodes` | коды условий победы через запятую: `r01_00,…,xp5ras_00` |

### Ping (команда 2)

Ответ: `STIM` (число, время сервера в мс).

> **Важно про чтение размеров в логах.** Строка вида `reply 9B` — это
> **десятичные** байты с суффиксом `B`, а не hex: `9B` = 9 байт, `411B` = 411.
> Ошибка чтения как hex дала ложный вывод «cmd 2 = PostAuth» и правку,
> которая была откачена. Сверяться с эталонным бэкендом
> `BF3LegacyLan\backend\BF3LegacyLan.Backend.exe` (лог `lfl.out`).

### GetTelemetryServer (команда 5)

Запрос реального клиента — два поля: **`CMAC`, `SNAM`**
(`C:\Games\trash\2026-10-11\bk5.out:497`). В документе они не были указаны;
мы их не читаем и на работу не влияет.

Ответ **плоский**, без обёртки: `ADRS`, `ANON`, `DISA`, `FILT`, `LOC`,
`NOOK`, `PORT`, `SDLY`, `SESS`, `SKEY`, `SPCT`, `STIM`.

Мы отдаём `ADRS = 127.0.0.1`, пустые `DISA`/`FILT`/`SESS`/`SKEY` — телеметрия
EA не нужна.

> ⚠️ **Осторожно: `STIM` бывает двух типов.** В `Ping` — **число**
> (`New-Blaze-Emulator\...\Commands\UtilComponent\PingCommand.cs:15`), а в
> `TELE` (GetTelemetryServer и PostAuth) — **строка** `"Default"`
> (`GetTelemetryServerCommand.cs:29`, `PostAuthCommand.cs:42`).

### PreAuth (команда 7)

Запрос: `CDAT` (структура `TYPE`, `SVCN`) и `CINF` (структура `LOC`),
плюс **`FCCR`** — реальный клиент шлёт третье поле
(`C:\Games\trash\2026-10-11\bk5.out:428`). Мы его не читаем.

Ответ: `ANON`, `ASRC`, `CIDS` (список номеров), `CNGN`, `CONF` (структура с
вложенной картой `CONF` таймаутов), `INST`, `MINR`, `NASP`, `PILD`, `PLAT`,
`PTAG`, `QOSS`, `RSRC`, `SVER`.

Критичные поля: **`SVER`** (версия протокола, клиент сверяет) и **`CIDS`**
(список поддерживаемых компонентов). Без них клиент не пойдёт дальше.

`CONF` внутри: `connIdleTimeout`, `defaultRequestTimeout`, `pingPeriod`,
`voipHeadsetUpdateRate`, `xlspConnectionIdleTimeout`.

`QOSS` внутри: `BWPS`{`PSA`,`PSP`,`SNA`}, `LNP`, `LTPS` (карта), `SVID`.

### PostAuth (команда 8)

Ответ — четыре структуры: `PSS`, `TELE`, `TICK`, `UROP`.

| Структура | Поля |
|---|---|
| `PSS` | `ADRS`, `CSIG` (блоб), `PJID`, `PORT`, `RPRT`, `TIID` |
| `TELE` | как в GetTelemetryServer |
| `TICK` | `ADRS`, `PORT`, `SKEY` |
| `UROP` | `TMOP`, `UID` |

### UserSettingsSave (0x0B) / UserSettingsLoadAll (0x0C)

**`DATA` приходит строкой, а не блобом.** Ответ на LoadAll — карта
`SMAP` (String → String), ключи соответствуют файлам `settings\*.bin`
(`cust`, `sdt`), значение — base64 содержимого.

---

## Authentication

| Команда | Запрос | Ответ |
|---|---|---|
| Login (0x28) | `MAIL` + `PASS` (реальный клиент: `DVID, MAIL, PASS, TOKN, TYPE`) | `LDHT`, `NTOS`, `PCTK`, `PLST` (список солдат), `PRIV`, `SKEY`, `SPAM`, `THST`, `TSUI`, `TURI`, `UID` |
| SilentLogin (0x32) | `PID` | `AGUP`, `LDHT`, `NTOS`, `PCTK`, `PRIV`, `SESS`{…}, `SPAM`, `THST`, `TSUI`, `TURI` |
| LoginPersona (0x6E) | `PNAM` ⚠️ | `BUID`, `FRST`, `KEY`, `LLOG`, `MAIL`, `PDTL`{…}, `UID` |
| ListUserEntitlements2 (0x1D) | `ETAG` | `NLST` — список структур |

> ⚠️ **`PNAM` не подтверждён эмулятором.**
> `New-Blaze-Emulator\...\LoginPersonaCommand.cs:11-13` **вообще не читает тело
> запроса** — берёт пользователя, запомненного при `Login`. Наш же бэкенд
> `PNAM` читает и работает (`BlazeServices.cs:423`). Значение взято с
> работающего клиента, не из протокола.
>
> Реальный клиент шлёт в Login пять полей:
> `DVID, MAIL, PASS, TOKN, TYPE` —
> `C:\Games\trash\2026-10-11\bk5.out:446`.
>
> `ListUserEntitlements2`: при `ETAG == "ONLINE_ACCESS"` эмулятор отдаёт
> **пустой** ответ, при любом другом `ETAG` — полный `NLST`
> (`ListUserEntitlements2Command.cs:17-20,265-270`). Наш код ведёт себя так же
> (`BlazeServices.cs:457-473`) — это не баг, а поведение эталона.

**PersonaData (`PDTL`, и элементы `PLST`):** `DSNM` (имя), `LAST`, `PID`,
`STAS` (состояние), `XREF`, `XTYP`.

**`SESS` (в SilentLogin):** `BUID`, `FRST`, `KEY`, `LLOG`, `MAIL`,
`PDTL`{…}, `UID`.

**Элемент `NLST`:** `DEVI`, `GDAY`, `GNAM`, `ID`, `ISCO`, `PID`, `PJID`,
`PRCA`, `PRID`, `STAT`, `STRC`, **`TAG`**, `TDAY`, `TYPE`, `UCNT`, `VER`.

**`TAG = "ONLINE_ACCESS"`** — право зайти на сервер без проверки EA.
Остальные права (DLC) пустые. Список приходит **на любой `ETAG`, кроме
`ONLINE_ACCESS`**; на сам `ONLINE_ACCESS` ответ пустой (см. выше), то есть
клиент этим запросом проверяет «нужен ли мне Online Pass» и получает «нет».
Состав `NLST` у эмулятора — 13 записей с тегами `ONLINE_ACCESS`,
`PROJECT10_CODE_CONSUMED_LE1`, `BF3:PREMIUM_ACCESS`, `BF3:PC:B2K_PURCHASE`,
`BF3:PC:XPACK2_PURCHASE` … `XPACK5_PURCHASE`, `BF3:SHORTCUT:COOP`
(`ListUserEntitlements2Command.cs:25-262`).

**После входа** обязательно шлём уведомления `UserAdded` и `UserUpdated`,
иначе клиент не считает сессию established.

---

## Stats

### GetStatGroup (команда 4)

Запрос: `NAME` (имя группы, например `player_mpdefault2`).

**Ответ — не TDF.** Это бинарный блок DICE
`statgroups/bf3/bf3_player_mpdefault2.bin` (135 690 байт в нашем каталоге),
отдаётся байт в байт. Строки внутри лежат как
`<id:3 байта> 01 <длина> <ASCII> 00`, без меток-символов TDF.

Это подтверждается и ошибкой в строках рабочего бэкенда:
`getStatGroup payload is not TDF`.

Разбирать блок не нужно. Но если он больше 64 КБ (а он именно такой),
кадр идёт с **расширенным размером** — см. ниже.

### GetStatsByGroupAsync (команда 0x10)

Запрос: `NAME`, список идентификаторов `EID`.

Ответ приходит **уведомлением** `Stats 0x32`, а не ответом на запрос:
номер подтверждён `New-Blaze-Emulator\Notifications\StatsComponent\GetStatsAsyncNotification.cs:18`
(там **пустая заглушка** — источником быть не может).

> ⚠️ **Таблица ниже устарела** — она описывает заглушку эмулятора
> (`STATS`/`SNAM`/`EID`). Реальный формат снят с рабочего бэкенда и живёт
> в нашем коде `BlazeServices.cs:605-613`:
> `{GRNM, KEY="No_Scope_Defined", LAST=1, STS{STAT: []entity}, VID}`,
> где `entity = {EID, ETYP=ObjectType(30722,1), POFF=0, STAT: ["F2", …]}`.
> Прежний формат клиент игнорировал, из-за чего не показывался ранг.

| Поле | Смысл |
|---|---|
| `STATS` | карта `String → Float`: имя стата и значение |
| `SNAM` | имя группы |
| `EID` | идентификатор солдата |

`EID` — это `personaref`. **По нему клиент берёт ранг.** У кого нет записи
прогресса, у того ранга не будет.

---

## Redirector

### GetServerInstance (команда 1)

Запрос: `CLNT` (тип клиента). Реальный клиент BF3 шлёт **13** полей, а не
одно: `BSDK, BTIM, CLNT, CLTP, CSKU, CVER, DSDK, ENV, FPID, LOC, NAME,
PLAT, PROF` — `C:\Games\trash\2026-10-11\bk5.out:403`. Мы читаем только
`CLNT` и это достаточно.

Ответ:

| Поле | Тип |
|---|---|
| `ADDR` | **union NetworkAddress**, активный член **`XboxClientAddress` (0)** |
| `AMAP` | пустой список структур |
| `MSGS` | пустой список строк |
| `NMAP` | пустой список структур |
| `SECU` | целое |
| `XDNS` | целое |

Внутри `ADDR`: структура `VALU` с полями `HOST`, `IP`, `PORT`.

Клиент идёт по этому адресу за остальными службами Blaze.

> **Исправление 11.10.2026.** Здесь раньше было написано `IPAddress` (4) —
> это было неверно. Активный член **0** подтверждён двумя независимыми
> источниками: наш код `src\Legacy.Backend\Blaze\Payloads.cs:557` и
> `New-Blaze-Emulator\Blaze.Server\Commands\RedirectorComponent\GetServerInstanceCommand.cs:17`.
>
> Про `AMAP`/`MSGS`/`NMAP`: `New-Blaze-Emulator` их **не шлёт**
> (`GetServerInstanceCommand.cs:15-28` — только `ADDR`, `SECU`, `XDNS`).
> Поля добавлены нами по следам рабочего бэкенда (`Payloads.cs:562-564`),
> эмулятором **не проверено**. Пустыми их делать нельзя — снято с рабочего
> байт-в-байт.

---

## UserSessions

### UpdateNetworkInfo (0x14)

Запрос: `ADDR` (union) → `VALU` (структура) → `INIP` (структура)
→ `IP`, `PORT`.

### Уведомления

| Уведомление | Номер | Payload |
|---|---|---|
| Расширенные данные сессии | **1** | `DATA`{…}, `USID` |
| UserAdded | **2** | `DATA`{…}, `USER`{`AID`,`ALOC`,`ID`,`NAME`} |
| UserUpdated | **5** | `FLGS`, `ID` |
| GameSettingsChange | **0x6E** | `ATTR` (битовая маска `GSET`), `GID` |

`DATA` внутри: `ADDR` (union), `BPS`, `CTY`, `DMAP` (карта), `HWFG`, `QDAT`,
`UATT`, `PSLM` (список).

> Дополнения 11.10.2026 (источник — `New-Blaze-Emulator`):
> - Уведомление **0x6E** (`ATTR`, `GID`) шлём в ответ на `SetGameSettings` —
>   `Notifications\GameManagerComponent\GameSettingsChangeNotification.cs:17-21`.
>   Номер добавлен в `BlazeComponents.cs` (`GameSettingsChange`);
>   **само уведомление мы пока не отправляем** — команда `SetGameSettings`
>   у нас только принимается (`BlazeServices.cs:833-839`).
> - В `DATA` расширенных данных сессии бывает необязательный список **`ULST`**
>   (`TDF_TYPE_BLAZE_OBJECT_ID`, эмулятор кладёт туда Vector3 с ID игры) —
>   `Notifications\UserSessionsComponent\UserSessionExtendedDataUpdateNotification.cs:48-51`.
>   Мы его **не шлём** (наш `Payloads.SessionExtended`, `Payloads.cs:531-542`).
> - `BPS` — **строка** (`"ams"`), а не число:
>   `UserAddedNotification.cs:18`, `PostAuthCommand.cs:37`.
> - В `DATA` уведомления 1 есть `PSLM` из пяти одинаковых `268374015` —
>   `UserSessionExtendedDataUpdateNotification.cs:14-15`.
>
> **Отдельно (устарело):** раньше уведомление **1** мы не слали вообще. Сейчас его получает игровой сервер
> при входе игрока (`BlazeServices.TryFlushJoinNotify`), а с 20.10.2026 перед ним — UserAdded/UserUpdated о
> входящем игроке: обработчик уведомления 1 в сервере (`0x0151D580`) пишет данные только уже известному
> пользователю, иначе ассерт «Joined player has no blaze extended data» (разбор — MODLOG 20.10 ≈02:07).

**Номера уведомлений отличаются от номеров команд-запросов** — это частая
ошибка.

---

## GameManager

### CreateGame (команда 1)

Запрос: `ATTR` (карта строк: `level`, `levellocation`, `gameMode`, …),
**`GNAM`** (имя игры — не `NAME`!), `PCAP` (список), `PMAX`, `NRES`, `NTOP`,
`VOIP`, `PRES`, `QCAP`, `IGNO`, `GSET`.

Реальный клиент шлёт **24** поля:
`ATTR, BTPL, GCTR, GENT, GNAM, GSET, GTYP, GURL, HNET, IGNO, NRES, NTOP, PCAP,
PGID, PGSC, PMAX, PRES, QCAP, RGID, SLOT, TCAP, TIDX, VOIP, VSTR` —
`C:\Games\trash\2026-10-11\bk5.out:506`.

Ответ: `GID`.

Затем клиент ждёт **два уведомления**: смену состояния игры и `GameSetup`.

### FinalizeGameCreation (0x0F) → `GID`

### JoinGame (команда 9)

Запрос: `GID` плюс `GVER` (наш лог реального клиента:
`joinGame GID=1 GVER="67"` — `C:\Games\trash\2026-10-11\g.out:237`).
Ответ: `GID` и **`JGS`** (не `SNUM`).

Затем клиент получает уведомления в таком порядке:

| Уведомление | Номер | Payload |
|---|---|---|
| PlayerJoining | **0x15** | `GID`, `PDAT`{…} — хост узнаёт о входящем |
| PlayerClaimingReservation | **0x19** | `GID`, `PDAT`{…} `STAT=2` — игрок занял слот |
| InitiateConnections | **0x16** | `GAME`{…}, `PROS`, `QUEU`, `REAS`(union) — **здесь адрес игры** |
| PlayerJoinCompleted | **0x1E** | `GID`, `PID` — после `UpdateMeshConnection` |
| PlayerStateChanged | **0x74** | `GID`, `PID`, `STAT` |

`InitiateConnections`: у эмулятора `REAS = 0` и отдельная структура
`VALU = {DCTX = 3}` (`JoiningPlayerInitiateConnectionsNotification.cs:118-122`).
Мы `REAS` кодируем как **union** с `VALU{DCTX = 3}` внутри
(`Payloads.cs:740-741`) и добавляем пустой список `QUEU` — формально другое,
но клиент принимает (проверено живым входом).

Порядок и паузы из реального прогона: `joinGame` → **23 с** → уведомления
(`probe5.out:228,231`). Пауза **наша**: `JoinNotifyDue = +20 c`
(`BlazeServices.cs:819`), отправка в `TryFlushJoinNotify` (`:522-558`).

### UpdateMeshConnection (0x1D) — конец рукопожатия Blaze → игра

Запрос: `GID`, `TARG` (список-структура: `[GID, PID, STAT]`).
`STAT = 2` — игрок подключился к игровому порту; `STAT = 0` — отвалился.

- `New-Blaze-Emulator\...\GameManagerComponent\UpdateMeshConnectionCommand.cs:15-53`
  (при `STAT=2` шлёт `0x74` и `0x1E`; при `STAT=0` — `PlayerRemoved`).
- Наш код — `BlazeServices.cs:841-911`, сделан как зеркало `HandleUpdateMeshConnection`
  рабочего бэкенда.

### AdvanceGameState (3) → пустой ответ, уведомление `GID`+`GSTA` (**0x64**)

### Данные игрока `PDAT` / `PROS`

`BLOB`, `EXID`, `GID`, `LOC`, `NAME`, `PATT` (карта), `PID`, `PNET` (union),
`SID`, `SLOT`, `STAT`, `TIDX`, `TIME`, `UGID`, `UID`.

**`STAT`:** `0` — входит, `2` — занял слот.

**`PNET`** — union с `VALU`{`EXIP`{`IP`,`PORT`}, `INIP`{`IP`,`PORT`}}.
Активный член эмулятор ставит `IPPAirAddress = 2`
(`PlayerJoiningNotification.cs:32`); наш код — то же значение, но называет его
`IPPAddress` (`Payloads.cs:28,117`). Число одно, имя — нет.

**`SID`** — номер слота, начиная с **1** у первого игрока. Подтверждено
независимо: у эмулятора список слотов предсоздан с одним фиктивным
элементом `Slots = new List<ulong>() { 0 }` (`GameManager\Game.cs:54`),
поэтому `FindIndex` первого реального игрока даёт 1
(`JoinGameCommand.cs:34-35`).

**`SLOT`** — поле есть, но эталонного значения мы **не знаем**: эмулятор
пишет константу `0` (`PlayerJoiningNotification.cs:49`,
`PlayerClaimingReservationNotification.cs:50`,
`JoiningPlayerInitiateConnectionsNotification.cs:110`). Мы пишем `0` —
расхождения нет, но и подтверждения нет.

Значения-заглушки у эмулятора (полезно как образец формы):
`EXID=0`, `TIDX=65535`, `TIME=0`, `UGID=Vector3(0,0,0)`,
`PATT={"Premium":"False"}` (`PlayerJoiningNotification.cs:23-54`).

`UID` у эмулятора противоречив: в `PlayerJoining` — это id Blaze-соединения
(`:54`), в `PlayerClaimingReservation` и `PROS` — personaref
(`PlayerClaimingReservationNotification.cs:55`,
`JoiningPlayerInitiateConnectionsNotification.cs:115`). Баг эмулятора,
эталонного значения отсюда нет.

### `GAME` в InitiateConnections и GameSetup

`ADMN`, `ATTR`, `CAP`, `GID`, `GNAM`, `GPVH`, `GSID`, `GSTA`, `GTYP`,
**`HNET`**, `HSES`, `IGNO`, `MCAP`, `NQQS`, `NRES`, `NTOP`, `PGID`, `PGSR`,
`PHST`, `PRES`, `PSAS`, `QCAP`, `SEED`, `TCAP`, `THST`, `UUID`, `VOIP`,
`VSTR`.

Дополнение 11.10.2026 (источник — `New-Blaze-Emulator`):

- **`PHST`** — структура `{HPID, HSLT}`: хост игры и его слот.
  `JoiningPlayerInitiateConnectionsNotification.cs:60-64`.
  **Мы их уже шлём** (`Payloads.cs:722-732`) — документ просто не
  перечислял; код менять не нужно.
- **`THST`** — структура `{HPID, HSLT}`: «очередь»; у эмулятора
  `HPID = GID`, `HSLT = 0` — там же `:70-74`. У нас `HPID = hostPid`.
- В `GameSetup` (0x14) дополнительно бывают **`REAS`** (union) и
  **`VALU`** = `{DCTX = 0}` — `GameSetupNotification.cs:63-67`.
  Мы шлём `REAS` как union-обёртку с `VALU{DCTX}` — `Payloads.cs:740-741`.
- В `GameSetup` у эмулятора **нет** `IGNO`, `NQQS` и `GSET` (последнее
  закомментировано как TODO — `GameSetupNotification.cs:29`), зато они есть
  в `InitiateConnections`. Мы шлём единый набор — расхождений с клиентом
  не зафиксировано.

> ⚠️ **`NQQS` против `NQOS`.** Эмулятор пишет `NQQS`
> (`JoiningPlayerInitiateConnectionsNotification.cs:50`), мы — `NQOS`
> (`Payloads.cs:713`). Наше имя снято с рабочего бэкенда. Расхождение
> **не проверено**: клиент принимает оба (он до этого поля не доходит —
> игра работает). По аналогии с `SESS`/`SKEY` из `TELE` есть основания
> считать, что правы мы, но это нужно проверить отдельным заходом.

**`HNET`** — список из одной записи, но пишется **как список из двух
структур**: `02 <EXIP> <INIP> 00`. Это тот особый случай в декодере TDF.
Каждая структура: `IP`, `PORT`.

Подтверждено **двумя независимыми** реализациями: `New-Blaze-Emulator`
(флаг `Stub` — `Blaze\TdfEncoder.cs:163-166`, «weird Tdf check #1: HNET» —
`Blaze\TdfDecoder.cs:131-141`) и `openBlase`
(`Tdf\TdfStruct.cpp:37-38`, `Tdf\TdfList.cpp:64-65` — та же проверка `== 2`
с комментарием `//WTF?!`).

`GTYP` = `frostbite_multiplayer`.

---

## Формат TDF — что нужно помнить

| Правило | Следствие |
|---|---|
| Метка — только буквы: символ = `(c & 0x1F) \| 0x40` | цифры в метке не обратимы, в протоколе метки всегда заглавные буквы |
| Varint: первый байт `< 0x40` — значение напрямую, иначе 6 бит + продолжение по 7 | не protobuf; 64-битному значению нужно до 10 байт |
| Строки — байтовые, без Unicode | кириллицу надо транслитерировать (`Tdf/AsciiSafe.cs`) |
| Float — IEEE 754 LE, 4 байта | единственное соглашение, ещё не подтверждённое клиентом |
| Union: байт активного члена, затем значение; `0x7F` = Unset | без записи union ответ не кодируется вовсе |

## Кадр Blaze

Заголовок — **12 байт**. Если payload больше 64 КБ, бит 4 байта 9
устанавливается и **сразу после заголовка** идут ещё **2 байта** старшей
половины размера, потом payload. Читать их из заголовка нельзя — индекса 12
в 12-байтовом массиве нет.

`Message ID` — **20 бит**: 12 в байтах 10..11 плюс 4 в младшей тетраде
байта 9.

**Клиент шлёт уведомления вперемешку с ответами** — их надо пропускать.
Настоящий Blaze ведёт себя так же.

## Правило проекта

**Не выдумывать имена полей.** Если поле не найдено в источниках выше —
считать, что его нет, и искать дальше, а не придумывать правдоподобное
имя. Ошибка в имени поля не видна в тестах, но делает команду бесполезной
для клиента.