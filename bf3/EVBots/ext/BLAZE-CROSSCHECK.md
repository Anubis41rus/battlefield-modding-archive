# BLAZE-CROSSCHECK — сверка наших выводов с чужими эмуляторами

Дата 2026-10-11. Сверяли наш документ `Legacy\backend\BLASE-FIELDS.md` и код
`Legacy\backend\src\Legacy.Backend\` с двумя чужими исходниками.

Обозначения путей в тексте:

| Сокращение | Путь |
|---|---|
| `NBE:` | `C:\Games\Legacy\New-Blaze-Emulator\` |
| `OB:` | `C:\Games\Legacy\openBlase\` |
| `наш:` | `C:\Games\Legacy\backend\src\Legacy.Backend\` |

Ничего не правилось, ничего не запускалось — только чтение.

---

## Что проверяли

1. **Имена сервисов, компонентов и числовые ID.** Сверка `BLASE-FIELDS.md`
   и `наш:\Blaze\BlazeComponents.cs` с тем, как чужие коды называют свои
   компоненты и команды.
2. **Кодирование размера сообщения.** Проверка нашего открытия
   «`reply 9B` — это 9 байт, а не hex» против того, как эмуляторы размер
   считают и пишут.
3. **Понятие игрока/сервера/соло и создание игрока на стороне сервера.**
   Ключевой вопрос: нужен ли боту отдельный `login` + `loginPersona`.
4. **Список игроков и слоты** (max clients, player index/slot).

---

## Совпадения

### 1.1. Номера компонентов — совпадают полностью

`NBE:Blaze.Server\Blaze\Blaze.cs:9-20` против `наш:\Blaze\BlazeComponents.cs:13-21`:

| Компонент | Эмулятор | Наш код | |
|---|---|---|---|
| Authentication | 1 | `0x0001` | ✅ |
| GameManager | 4 | `0x0004` | ✅ |
| Redirector | 5 | `0x0005` | ✅ |
| Stats | 7 | `0x0007` | ✅ |
| Util | 9 | `0x0009` | ✅ |
| Clubs | 11 | `0x000B` | ✅ |
| GameReporting | `0x1C` | `0x001C` | ✅ |
| RSP | `0x801` | `0x0801` | ✅ |
| UserSessions | `0x7802` | `0x7802` | ✅ |

Регистр и подчёркивания в именах — как у нас (`Authentication`,
`GameManager`, `UserSessions`), совпадает.

### 1.2. Номера команд — совпадают полностью

| Компонент | Эмулятор (диспетчер) | Наш код |
|---|---|---|
| Util 1/2/5/7/8/0x0B/0x0C/0x16 | `NBE:\Components\UtilComponent.cs:16-46` | `BlazeComponents.cs:59-66` ✅ |
| Auth 0x28/0x32/0x6E/0x1D | `NBE:\Components\AuthenticationComponent.cs:15-28` | `BlazeComponents.cs:72-75` ✅ |
| GameManager 1..0x1D | `NBE:\Components\GameManagerComponent.cs:15-56` | `BlazeComponents.cs:122-131` ✅ |
| Stats 4, 0x10 | `NBE:\Components\StatsComponent.cs:15-20` | `BlazeComponents.cs:81-82` ✅ |
| Redirector 1 | `NBE:\Components\RedirectorComponent.cs:15` | `BlazeComponents.cs:89` ✅ |
| UserSessions 0x14 | `NBE:\Components\UserSessionsComponent.cs:15` | `BlazeComponents.cs:95` ✅ |

### 1.3. Номера уведомлений — совпадают

| Уведомление | Эмулятор (вызов `client.Notify`) | Наш код |
|---|---|---|
| UserSessions 1 (расшир. данные) | `NBE:\Notifications\...\UserSessionExtendedDataUpdateNotification.cs:59,63` | `BlazeComponents.cs:96` ✅ |
| UserSessions 2 (UserAdded) | `NBE:\...\UserAddedNotification.cs:43` | `:97` ✅ |
| UserSessions 5 (UserUpdated) | `NBE:\...\UserUpdatedNotification.cs:19` | `:98` ✅ |
| GameManager 0x14 GameSetup | `NBE:\...\GameSetupNotification.cs:81` | `:107` ✅ |
| GameManager 0x15 PlayerJoining | `NBE:\...\PlayerJoiningNotification.cs:58` | `:108` ✅ |
| GameManager 0x16 InitiateConnections | `NBE:\...\JoiningPlayerInitiateConnectionsNotification.cs:125` | `:109` ✅ |
| GameManager 0x19 PlayerClaimingReservation | `NBE:\...\PlayerClaimingReservationNotification.cs:59` | `:110` ✅ |
| GameManager 0x1E PlayerJoinCompleted | `NBE:\...\PlayerJoinCompletedNotification.cs:24` | `:111` ✅ |
| GameManager 0x28 PlayerRemoved | `NBE:\...\PlayerRemovedNotification.cs:20` | `:112` ✅ |
| GameManager 0x64 GameStateChange | `NBE:\...\GameStateChangeNotification.cs:21` | `:113` ✅ |
| GameManager 0x74 PlayerStateChange | `NBE:\...\GamePlayerStateChangeNotification.cs:25` | `:116` ✅ |
| Stats 0x32 GetStatsAsync | `NBE:\Notifications\StatsComponent\GetStatsAsyncNotification.cs:18` | `BlazeComponents.cs:83` ✅ |

### 1.4. Имена полей — совпадают (выборочно, по нашим самым важным)

- **Login**: запрос `MAIL` — `NBE:\Commands\AuthenticationComponent\LoginCommand.cs:14`.
  Ответ `LDHT, NTOS, PCTK, PLST, PRIV, SKEY, SPAM, THST, TSUI, TURI, UID` —
  `LoginCommand.cs:30-51` — посимвольно как в `BLASE-FIELDS.md:104`.
- **PersonaData (`PLST`/`PDTL`)**: `DSNM, LAST, PID, STAS, XREF, XTYP` —
  `LoginCommand.cs:37-42`, `LoginPersonaCommand.cs:24-29`,
  `SilentLoginCommand.cs:34-39` — ✅ как `BLASE-FIELDS.md:109-110`.
  `STAS = 2` — «активен» (`LoginCommand.cs:40`).
- **LoginPersona**: ответ `BUID, FRST, KEY, LLOG, MAIL, PDTL, UID` —
  `LoginPersonaCommand.cs:17-31` — ✅ `BLASE-FIELDS.md:106`.
- **SilentLogin**: запрос `PID` (`SilentLoginCommand.cs:13`); ответ
  `AGUP, LDHT, NTOS, PCTK, PRIV, SESS{…}, SPAM, THST, TSUI, TURI` —
  `SilentLoginCommand.cs:20-46`; `UID` лежит **внутри** `SESS`
  (`SilentLoginCommand.cs:41`) — ✅ `BLASE-FIELDS.md:105,112`.
- **Element `NLST`**: 16 полей `DEVI, GDAY, GNAM, ID, ISCO, PID, PJID, PRCA,
  PRID, STAT, STRC, TAG, TDAY, TYPE, UCNT, VER` —
  `NBE:\Commands\AuthenticationComponent\ListUserEntitlements2Command.cs:25-40`
  — ✅ `BLASE-FIELDS.md:115-116`.
- **UpdateNetworkInfo**: читает `ADDR` → `VALU` → `INIP` → `IP`, `PORT` —
  `NBE:\Commands\UserSessionsComponent\UpdateNetworkInfoCommand.cs:15-20`
  — ✅ `BLASE-FIELDS.md:184-185`.
- **UserAdded**: `DATA{…}, USER{AID, ALOC, ID, NAME}` —
  `NBE:\Notifications\...\UserAddedNotification.cs:15-41`;
  `USERUpdated`: `FLGS, ID` — `UserUpdatedNotification.cs:15-16` — ✅
  `BLASE-FIELDS.md:191-193`.
- **CreateGame**: запрос `ATTR, GNAM, GSET, PCAP, IGNO, PMAX, NRES, NTOP, VOIP,
  PRES, QCAP` — `NBE:\Commands\GameManagerComponent\CreateGameCommand.cs:13-25`;
  ответ `GID` — `:63` — ✅ `BLASE-FIELDS.md:207-212`. Имя игры в `GNAM`,
  а не `NAME` — подтверждено.
- **JoinGame**: ответ `GID` + `JGS` —
  `NBE:\Commands\GameManagerComponent\JoinGameCommand.cs:25-26` — ✅
  `BLASE-FIELDS.md:219`.
- **`STAT`**: `0` в PlayerJoining (`PlayerJoiningNotification.cs:50`),
  `2` в PlayerClaimingReservation (`:51`) и в `PROS` InitiateConnections
  (`JoiningPlayerInitiateConnectionsNotification.cs:111`) — ✅
  `BLASE-FIELDS.md:226,236`.
- **PreAuth**: все 15 полей ответа совпадают —
  `NBE:\Commands\UtilComponent\PreAuthCommand.cs:36-79` — ✅
  `BLASE-FIELDS.md:69-71`. Внутри `CONF` — ровно те пять ключей
  (`PreAuthCommand.cs:44-48`) — ✅ `BLASE-FIELDS.md:76-77`.
- **PostAuth**: `PSS, TELE, TICK, UROP` —
  `NBE:\Commands\UtilComponent\PostAuthCommand.cs:17-54` — ✅
  `BLASE-FIELDS.md:83-90`.
- **Ping**: `STIM` числом (`NBE:\Commands\UtilComponent\PingCommand.cs:15`) —
  ✅ `BLASE-FIELDS.md:49`.
- **FetchClientConfig**: карта `CONF` с ключами `Achievements` и `WinCodes` —
  `NBE:\Commands\UtilComponent\FetchClientConfigCommand.cs:19-22` — ✅
  `BLASE-FIELDS.md:40-45`.
- **GetTelemetryServer**: плоский ответ без обёртки, 12 полей —
  `NBE:\Commands\UtilComponent\GetTelemetryServerCommand.cs:15-29` — ✅
  `BLASE-FIELDS.md:59-60`.
- **UserSettingsSave**: `DATA` читается как **строка**, не блоб —
  `NBE:\Commands\UtilComponent\UserSettingsSaveCommand.cs:16` — ✅
  `BLASE-FIELDS.md:94`. Ключ `SMAP` — `UserSettingsLoadAllCommand.cs:22`.
- **GetStatsByGroupAsync**: ответ не отправляется, шлётся уведомление
  `Stats 0x32` — `NBE:\Commands\StatsComponent\GetStatsByGroupAsyncCommand.cs:15`
  + `Notifications\StatsComponent\GetStatsAsyncNotification.cs:18` — ✅
  `BLASE-FIELDS.md:147`.
- **Типы TDF**: номера совпадают один в один — `OB:Tdf\Tdf.h:6-22`
  и `NBE:\Blaze\Tdf.cs:10-26` (Integer=0, String=1, Binary=2, Struct=3,
  List=4, Map/DoubleList=5, Variable/IntegerList=7, Vector2D=8, Vector3D=9).
- **Метка TDF**: сжатие `(0x20 | (c & 0x1F))` — `OB:Tdf\Tdf.cpp:53` и
  `NBE:\Blaze\TdfEncoder.cs:38`; распаковка `(j & 0x1F) | 0x40` —
  `OB:Tdf\Tdf.cpp:68` — ✅ `BLASE-FIELDS.md:259`.
- **Varint**: 6 бит в первом байте + продолжение по 7 —
  `OB:Tdf\BlazeInStream.hpp:79-99`, `NBE:\Blaze\TdfEncoder.cs:52-71`,
  `NBE:\Blaze\TdfDecoder.cs:60-82` — ✅ `BLASE-FIELDS.md:260`.

### 1.5. Особый случай `HNET` подтверждён ДВЕМА независимыми реализациями

Наше правило «`HNET` — список из одной записи, но пишется как список из двух
структур: `02 <EXIP> <INIP> 00`» (`BLASE-FIELDS.md:247-249`):

- `NBE:\Blaze\TdfEncoder.cs:163-166` — флаг `Stub` дописывает байт `02` после
  счётчика; используется ровно в `HNET`:
  `NBE:\Notifications\...\GameSetupNotification.cs:33-48`,
  `JoiningPlayerInitiateConnectionsNotification.cs:31-46`.
- `NBE:\Blaze\TdfDecoder.cs:131-141` — «weird Tdf check #1: HNET, for example».
- `OB:Tdf\TdfStruct.cpp:37-38`, `OB:Tdf\TdfList.cpp:64-65`,
  `OB:Tdf\TdfDoubleList.cpp:65-66` — та же проверка `== 2` с комментарием
  `//WTF?!`, написанная независимо на C++.

### 1.6. Слот 1 = первый вошедший — подтверждено

Наш вывод (журнал входа: `joinGame … слот=1`,
`C:\Games\ext\SPAWN-RESEARCH.md:54`, и `:322`) подтверждается устройством
эмулятора:

- `NBE:\GameManager\Game.cs:54` — список слотов создаётся **с одним
  фиктивным элементом**: `Slots = new List<ulong>() { 0 };`
- `NBE:\Commands\GameManagerComponent\JoinGameCommand.cs:34-35` —
  `Slots.Add(userId); slotID = Slots.FindIndex(...)`, `FindIndex` 0-based.

Итог: хост/заглушка занимает индекс 0, **первый реальный игрок получает
SID = 1**, второй — 2. Ровно как у нас: `наш:\Blaze\GameRegistry.cs:55-68`
(`Attach` возвращает индекс, первый — 0, хост, дальше 1, 2, …) и
`наш:\Blaze\GameRegistry.cs:82-83` (комментарий «клиент получает slot=1»).

Это же значение `SID` пишется во всех трёх местах:
`PlayerJoiningNotification.cs:16,48`, `PlayerClaimingReservationNotification.cs:17,49`,
`JoiningPlayerInitiateConnectionsNotification.cs:16,109`.

---

## Расхождения

### 2.1. Размер сообщения: эмуляторы не пишут «N B» вообще — наше чтение подтверждается

- `New-Blaze-Emulator` **не логирует размеры** нигде — в `Logging\Log.cs:42-77`
  нет ни одного вывода длины. **Не проверено с его стороны, как он их пишет.**
- `openBlase` пишет размер **десятичным** `printf("%i")`, без суффикса и без
  hex: `OB:BlaseProxy\main.cpp:293` (`Length: %i`), `:300` (`len: %i`),
  `:379` (`Server sent data! (len: %i)`), `:421`.
  → Ни один из двух не использует hex и не использует суффикс `B`.
  Наше открытие «`reply 9B` = 9 байт, десятичное» логами эталонного бэкенда
  чужим кодом **не опровергается и не подтверждается напрямую** — но код
  обоих эмуляторов показывает, что в самом протоколе размер — это
  **big-endian 16-битное число в байтах 0..1**, ровно как у нас
  (`наш:\Blaze\BlazeFrame.cs:44`).

### 2.2. Заголовок кадра — расхождение в том, какой бит читать

| Что | Наш код | NBE | OB |
|---|---|---|---|
| Байты 0..1 | размер, BE16 (`BlazeFrame.cs:44`) | BE16 (`Base\Client.cs:67`) | BE16, byteswap (`main.cpp:330`) ✅ |
| Байты 2..3 | компонент, BE16 (`:45`) | компонент (`Client.cs:61`) ✅ | компонент (`main.cpp:331`) ✅ |
| Байты 4..5 | команда, BE16 (`:46`) | команда (`Client.cs:62`) ✅ | команда (`main.cpp:332`) ✅ |
| Байты 6..7 | код ошибки, BE16 (`:47`) | errorCode (`Client.cs:63`) ✅ | Error (`main.cpp:333`) ✅ |
| Байт 8 | старший ниббл = тип (`BlazeFrame.cs:48`) | `msgType * 16` (`Client.cs:155`) ✅ | (в поле QType) |
| **Байт 9, бит 0x10** | флаг расширенного размера (`BlazeFrame.cs:70`) | `buffer[9] & 0x10` (`Client.cs:69`) ✅ | **`QType & 0x10` → это бит 4 байта 8** (`main.cpp:336`) ❌ |
| Message ID | 20 бит (`:56`) | 20 бит (`Client.cs:65`) ✅ | **только 16 бит** (`main.cpp:335`) ❌ |

**Расхождение A (openBlase, бит расширенного размера).** `QType` читается как
WORD из байтов 8..9 (`main.cpp:334`), поэтому `QType & 0x10`
(`main.cpp:336,341`) проверяет бит 4 **байта 8**, а не байта 9. Наш вывод
(`BLASE-FIELDS.md:268`) совпадает с `New-Blaze-Emulator` и с нашим кодом;
openBlase здесь неправ. Он — только логгер, на протокол это не влияет, но
как источник истины не годится.

**Расхождение B (openBlase, message ID).** `ID` читается полным 16-битным
WORD из байтов 10..11 (`main.cpp:335`) — верхние 4 бита из байта 9 теряются.
Наш 20-битный разбор (`BLASE-FIELDS.md:272-273`,
`наш:\Blaze\BlazeFrame.cs:56`) совпадает с `NBE:\Base\Client.cs:65`
и `NBE:\Base\Client.cs:156`.

### 2.3. Расширенный размер: openBlase согласен с нами, New-Blaze-Emulator сломан

- **openBlase**: при установленном бите читает **ещё одно 16-битное BE-слово
  сразу после 12-байтового заголовка** (`main.cpp:339-343`), то есть с
  индекса 12. Итог `len = Length + (extLength << 16)` (`main.cpp:343`) —
  ✅ совпадает с нашим правилом «2 байта старшей половины сразу после
  заголовка» (`BLASE-FIELDS.md:267-270`).
- **New-Blaze-Emulator** — **баг**: `payloadSize |= (buffer[13] | (buffer[12] << 8) >> 16)`
  (`NBE:\Base\Client.cs:71`). Из-за приоритета операций `(buffer[12] << 8) >> 16`
  всегда 0, и результат `payloadSize |= buffer[13]` **не сдвигается на 16** —
  старшая половина размера просто вклинивается в младший байт. Плюс
  `headerSize` считается мусорной формулой
  `((buffer[9] >> 3) & 4) + ((buffer[9] >> 3) & 2) + 12`
  (`NBE:\Base\Client.cs:59`) — она реагирует на посторонние биты 0x20 и 0x10
  байта 9. И `Send()` расширенный размер **вообще никогда не пишет**
  (`Client.cs:136-173`): 12 байт заголовка + payload, флаг не выставляется.
  **Вывод: по п.2 New-Blaze-Emulator непригоден как эталон.** openBlase —
  пригоден.

### 2.4. Счётчик list/map: эмуляторы расходятся между собой, openBlase прав

- **openBlase** читает счётчик как **varint**:
  `OB:Tdf\TdfList.cpp:35`, `TdfDoubleList.cpp:36`, `TdfIntegerList.cpp:34`
  — `stream->ReadCompressedInt()`.
- **New-Blaze-Emulator** пишет и читает счётчик **сырым байтом**:
  `NBE:\Blaze\TdfEncoder.cs:161` (`(byte)tdf.List.Count`), `:179` (map),
  `:228` (integer list); `NBE:\Blaze\TdfDecoder.cs:209,226,290`.

Это в точности подтверждает оговорку в `BLASE-FIELDS.md:14-20`: имена полей у
New-Blaze-Emulator годные, а кодек счётчика у него неверный. **Новое
подтверждение:** openBlase, написанный независимо, читает счётчик как varint —
значит правы мы (и эталонный `TdfWriter.WriteVarInt`), а не эмулятор.

### 2.5. Чтение строк: openBlase читает неправильно

- `NBE:\Blaze\TdfEncoder.cs:76-86` пишет `len+1`, затем байты, затем `0x00`;
  `NBE:\Blaze\TdfDecoder.cs:87-98` читает `len-1`, затем `0x00`.
  Наш код идентичен: `наш:\Tdf\TdfCodec.cs:265-267`. ✅
- `OB:Tdf\TdfString.cpp:35-39` читает длину как varint и копирует **ровно
  `len` байт**, без `-1` и **не съедая завершающий `0x00`**. Это off-by-one,
  у openBlase строка «поедет» после первой же. **openBlase как эталон по
  строкам не годится.**

### 2.6. Расхождения в нашем собственном коде / документе (найдены при сверке)

1. **`BLASE-FIELDS.md:170` неверен.** Там написано, что в `GetServerInstance`
   активный член union `ADDR` — `IPAddress (4)`. На самом деле и наш код
   (`наш:\Blaze\Payloads.cs:557` → `AddrMember.XboxClientAddress` = 0,
   `Payloads.cs:26`), и эмулятор
   (`NBE:\Commands\RedirectorComponent\GetServerInstanceCommand.cs:17`)
   используют **`XboxClientAddress = 0`**. Расходится именно документ.
2. **В ответе `GetServerInstance` у нас есть лишние поля**, которых нет ни в
   эмуляторе, ни в документе: `AMAP`, `MSGS`, `NMAP`
   (`наш:\Blaze\Payloads.cs:562-564`). Эмулятор отдаёт только `ADDR`, `SECU`,
   `XDNS` (`GetServerInstanceCommand.cs:15-28`). Комментарий в коде утверждает,
   что пустые списки сняты байт-в-байт с рабочего бэкенда — это остаётся
   непроверенным (эмулятор не подтверждает и не опровергает).
3. **Комментарий в `наш:\Blaze\BlazeComponents.cs:3` устарел:** там написано
   `CL# 944265`, а отправляем мы `CL# 1060080`
   (`наш:\Blaze\Payloads.cs:38`), и именно это значение стоит у эмулятора
   (`NBE:\Commands\UtilComponent\PreAuthCommand.cs:79`). Код прав, комментарий
   врёт.
4. **Запрос `LoginPersona` документом не подтверждён.** `BLASE-FIELDS.md:106`
   утверждает, что запрос — `PNAM`. Эмулятор **вообще не читает тело запроса**
   `LoginPersona` (`NBE:\Commands\AuthenticationComponent\LoginPersonaCommand.cs:11-13`),
   а берёт пользователя, запомненного при `Login`. **Не проверено.**
5. **`SLOT` в `PDAT`/`PROS` эмулятор не заполняет** — жёстко `0`:
   `PlayerJoiningNotification.cs:49`, `PlayerClaimingReservationNotification.cs:50`,
   `JoiningPlayerInitiateConnectionsNotification.cs:110`. Поле есть
   (`BLASE-FIELDS.md:234`), но эталонного значения мы не знаем.
6. **Наш латентный баг в разборе расширенного размера.**
   `наш:\Blaze\BlazeListener.cs:206` даёт `extendedHigh` до `0xFFFF`, а
   `наш:\Blaze\BlazeFrame.cs:52` делает `payload |= extendedHigh << 16` в
   типе `int`. При `extendedHigh >= 0x8000` (payload ≥ 16 МиБ) длина станет
   отрицательной. Для наших 135 КБ (`extendedHigh = 2`) безопасно, но
   ограничение стоит зафиксировать. **Ни один эмулятор этот путь не
   проверяет** — у NBE он сломан (п.2.3), openBlase не сервер.

### 2.7. Уникальность слотов: эмулятор её НЕ обеспечивает

- `NBE:\Commands\GameManagerComponent\JoinGameCommand.cs:34` — `Slots.Add(...)`
  вызывается **безусловно**, проверки «уже есть» нет. Повторный вход даст
  дубль и `FindIndex` вернёт первый индекс (тот же слот у двух игроков).
- Удаление — `NBE:\Commands\GameManagerComponent\UpdateMeshConnectionCommand.cs:42,49`.
- Наш код строже: `наш:\Blaze\GameRegistry.cs:59-63` (`IndexOf` → вернуть
  прежний индекс). **Наше правило «слоты уникальны» оставляем** — эмулятор
  его не подтверждает, но и не опровергает, а ломает.

### 2.8. Лимит игроков эмулятором не проверяется

`PMAX` читается в `CreateGame` (`NBE:\Commands\GameManagerComponent\CreateGameCommand.cs:18`)
и кладётся в `MaxPlayers` (`:38`), отдаётся как `MCAP`
(`GameSetupNotification.cs:50`, `JoiningPlayerInitiateConnectionsNotification.cs:49`).
**Ни в `JoinGame`, ни где-либо ещё проверки «влез ли игрок» нет** — код ошибки
`GameFull` не используется. Проверку лимита обязан делать наш бэкенд сами.

### 2.9. Внутреннее противоречие эмулятора в `UID`

`PlayerJoining` пишет `UID = client.ID` (идентификатор Blaze-соединения) —
`NBE:\Notifications\...\PlayerJoiningNotification.cs:54`, а
`PlayerClaimingReservation` и `PROS` пишут `UID = client.User.ID` (personaref) —
`PlayerClaimingReservationNotification.cs:55`,
`JoiningPlayerInitiateConnectionsNotification.cs:115`.
Это баг эмулятора. Эталонного значения отсюда не получить.

### 2.10. Мелочи, где эмулятор заведомо неправ (наш код прав)

- `GetStatGroup` у эмулятора возвращает **пустой ответ**
  (`NBE:\Commands\StatsComponent\GetStatGroupCommand.cs:15-20`). Вывод
  `BLASE-FIELDS.md:132-141` (ответ — бинарный блок DICE, не TDF) этой
  сверкой **не проверен**.
- `GetStatsAsyncNotification` — пустая заглушка
  (`NBE:\Notifications\StatsComponent\GetStatsAsyncNotification.cs:13-16`).
  Совпадает с оговоркой `BLASE-FIELDS.md:18`.
- `UserSettingsLoadAll` кладёт в `SMAP` значение `userSettings.ToString()`
  = строку `"System.Byte[]"` вместо base64
  (`NBE:\Commands\UtilComponent\UserSettingsLoadAllCommand.cs:24`).
  Наш base64 (`BLASE-FIELDS.md:94-96`) прав; имя ключа `cust` — ✅ совпало.
- `DestroyGameCommand` вообще не отвечает —
  `NBE:\Commands\GameManagerComponent\DestroyGameCommand.cs:21` (закомментировано).
- `Float` в `TdfBaseType` есть (`NBE:\Blaze\Tdf.cs:23`, id `0xA`), но в
  кодировщике ветки нет (`NBE:\Blaze\TdfEncoder.cs:300-302`) — поле молча
  не пишется. В openBlase Float нет вовсе (`OB:Tdf\Tdf.h:6-22`).
  Значит `BLASE-FIELDS.md:262` («Float — единственное соглашение, ещё не
  подтверждённое клиентом») остаётся неподтверждённым.
- Декодер метки выкидывает не-`A-Z` регуляркой
  (`NBE:\Blaze\TdfDecoder.cs:52`) — независимое подтверждение нашей заметки
  `BLASE-FIELDS.md:259` о том, что цифры в метке необратимы.

---

## Новое полезное из эмуляторов

### 3.1. Как устроен «игрок» — прямой ответ на наш вопрос про бота

Ни в одном из двух деревьев **нет** ни `ServerPlayerCreate`, ни `createPlayer`,
ни `spawnSoldier`, ни `AddPlayer` — поиск по обоим деревьям даёт **ноль**
совпадений. Понятия «соло» тоже нет (ноль совпадений). Оба эмулятора
существуют **только на уровне Blaze** и до движка BF3 не дотягиваются.

Игрок в `New-Blaze-Emulator` — это тройка:

1. `Client` — соединение (`NBE:\Base\Client.cs:13-48`): `ID`, `GameID`, `User`,
   `Type`, `Localization`, `Service`, `InternalIP/Port`, `ExternalIP/Port`.
2. `User` — персона (`NBE:\Base\Configuration.cs:31-38`): `ID`, `Email`, `Name`.
   **Список пользователей заранее задан в YAML** —
   `NBE:data\Blaze.Server.yml:1-15` (три записи `ID`/`Email`/`Name`).
   Создать пользователя из протокола нельзя — только редактировать конфиг.
3. Запись в `Game.Slots` (`NBE:\GameManager\Game.cs:50`).

Порядок входа, который делает эмулятор (**наш бот обязан повторить ровно
это — отдельная сессия на каждого бота**):

| Шаг | Команда | Где у эмулятора |
|---|---|---|
| 1 | `PreAuth` (Util 7) — клиент объявляет `CDAT{TYPE, SVCN}`, `CINF{LOC}` | `PreAuthCommand.cs:16-25` |
| 2 | `PostAuth` (Util 8) | `PostAuthCommand.cs:57` |
| 3 | **`Login` (Auth 0x28)** — запрос `MAIL`, поиск пользователя **по email** | `LoginCommand.cs:14-18` |
| 4 | **`LoginPersona` (Auth 0x6E)** — ответ `PDTL` + `BUID/UID` | `LoginPersonaCommand.cs:15-34` |
| 5 | **`UserAdded` + `UserUpdated`** — шлются **сразу после LoginPersona** | `LoginPersonaCommand.cs:36-37` |
| 6 | `ListUserEntitlements2` (Auth 0x1D) | `ListUserEntitlements2Command.cs:12` |
| 7 | `JoinGame` (GameManager 9) — **только здесь игрок получает слот** | `JoinGameCommand.cs:34-35` |

**Ответ на ключевой вопрос: да, боту нужен отдельный полный цикл
`login` + `loginPersona` со своим именем.** Никакого «создать игрока на
стороне сервера» ни в одном эмуляторе нет и быть не может — оба видят
только Blaze-клиентов. Дополнительно:

- Имя игрока (`client.User.Name`) идёт в `PDAT.NAME`
  (`PlayerJoiningNotification.cs:26`), `PROS.NAME`
  (`JoiningPlayerInitiateConnectionsNotification.cs:87`) и `USER.NAME`
  в `UserAdded` (`UserAddedNotification.cs:39`).
- `LoginPersona` **обязательно** шлёт `UserAdded`+`UserUpdated` — это
  независимое подтверждение нашего `BLASE-FIELDS.md:121-122`. То же делает
  `SilentLogin` (`SilentLoginCommand.cs:51-52`).
- У эмулятора есть готовая альтернатива логину: **`SilentLogin` (0x32) по
  `PID`** (`SilentLoginCommand.cs:13`) — возвращает `SESS` с `PDTL` внутри и
  тоже шлёт `UserAdded`+`UserUpdated`. Для бота это самый короткий путь:
  один запрос вместо двух, персону передавать не нужно.

### 3.2. Что ещё полезного

- **`CIDS` (PreAuth) — точный список** компонентов:
  `1, 25, 4, 27, 28, 6, 7, 9, 10, 11, 30720, 30721, 30722, 30723, 20, 30725,
  30726, 2000` — `NBE:\Commands\UtilComponent\PreAuthCommand.cs:32`.
  Наш `наш:\Blaze\Payloads.cs:49-53` — **совпадает byte-for-byte**.
- **Порт-разделение:** `BlazeHubServer` (редиректор) слушает **42127**
  (`NBE:\BlazeHubServer.cs:35`), `BlazeServer` (игровой) — **10041**
  (`NBE:\BlazeServer.cs:35`). `GetServerInstance` отдаёт `PORT = 10041`
  (`GetServerInstanceCommand.cs:23`). Это совпадает с openBlase, который
  слушает 42127 и форвардит на 42127/42129 (`OB:BlaseProxy\main.cpp:407-408,473`).
- **Хостнеймы для редиректа:** `gosredirector.ea.com` (42127) и
  `373244-gosprapp357.ea.com` (`OB:BlaseProxy\main.cpp:407`,
  `NBE:README.md:18-21`, `GetServerInstanceCommand.cs:21`).
  Сертификат-подмена для MITM лежит рядом:
  `OB:BlaseProxy\gosredirector.ea.com.crt/.key`, грузится в
  `OB:SSL\SSLServer.cpp:43,49,55`.
- **Уведомление GameManager `0x6E` (GameSettings)** — его у нас нет в
  `BlazeComponents.cs`. Payload `ATTR` (битовая маска `GSET`) + `GID`:
  `NBE:\Notifications\...\GameSettingsChangeNotification.cs:17-21`.
  Шлётся в ответ на `SetGameSettings` (GameManager 4).
- **`GAME.PHST{HPID,HSLT}` и `GAME.THST{HPID,HSLT}`** — хост и «очередь»,
  поля со значениями. Наш документ их не перечисляет:
  `NBE:\Notifications\...\JoiningPlayerInitiateConnectionsNotification.cs:60-64,70-74`.
- **`ULST` в расширенных данных сессии** — список `ObjectID` (Vector3),
  эмулятор кладёт туда ID игры:
  `NBE:\Notifications\...\UserSessionExtendedDataUpdateNotification.cs:48-51`.
  Наш документ это поле не упоминает.
- **Значения-заглушки в `PDAT`/`PROS`:** `EXID=0`, `TIDX=65535`, `TIME=0`,
  `UGID=Vector3(0,0,0)`, `PATT={"Premium":"False"}`
  (`PlayerJoiningNotification.cs:23-54`). Полезно как образец формы.
- **`TICK.SKEY` — строка формата**
  `{uid},127.0.0.1:8999,battlefield-3-pc,10,50,50,50,50,0,12`
  (`NBE:\Commands\UtilComponent\PostAuthCommand.cs:48`).
- **`TELE.STIM` — строка `"Default"`** (`PostAuthCommand.cs:42`,
  `GetTelemetryServerCommand.cs:29`), тогда как **`Ping.STIM` — число**
  (`PingCommand.cs:15`). Одноимённые поля разного типа.
- **`DATA.BPS` — строка `"ams"`**, а не число
  (`PostAuthCommand.cs:37`, `UserAddedNotification.cs:18`).
- **Номера ошибок Blaze, которые эмулятор реально использует:**
  `0x12D0004` (игра не найдена) — `NBE:\Commands\GameManagerComponent\JoinGameCommand.cs:17`.
  В нашем `BlazeError` (`наш:\Blaze\BlazeComponents.cs:39-49`) есть
  `GameNotFound = 2001` — **формат другой**, стоит завести оба.
- **Список игроков по запросу — отсутствует.** Ни `GetGameList`, ни
  `ListPlayers` нет ни в одном эмуляторе (поиск — ноль совпадений). Список
  игроков существует только как внутренний `Game.Slots`. Значит, наш
  собственный список игроков для UI/мода — наша собственная подсистема,
  аналога в чужих кодах нет.
- **openBlase — не эмулятор протокола, а MITM-прокси**: перехватывает
  TLS-трафик к `gosredirector.ea.com:42127` и `42129`, логирует заголовки и
  печатает TDF-дерево (`OB:BlaseProxy\main.cpp:319-366`). Компонентов,
  команд и имён полей он не знает вообще. Для сверки имён полей
  бесполезен; полезен только как второй источник по **формату кадра**.

---

## Выводы и что делать

1. **Имена полей подтверждены.** Все имена в `BLASE-FIELDS.md`, которые мы
   успели сверить (около 60 меток), совпали с `New-Blaze-Emulator` символ в
   символ. Выдуманных имён среди них нет. Документ можно считать
   проверенным по этим пунктам.
2. **Исправить `BLASE-FIELDS.md`:**
   - строка **170**: активный член union `ADDR` в `GetServerInstance` —
     `XboxClientAddress (0)`, а не `IPAddress (4)` (наш код уже так делает,
     `Payloads.cs:557`).
   - дополнить раздел `GetServerInstance` полями `AMAP`, `MSGS`, `NMAP`,
     которые мы шлём (`Payloads.cs:562-564`) — с пометкой, что это снято с
     рабочего бэкенда, эмулятором не подтверждено.
   - дополнить список уведомлений GameManager числом **`0x6E`**
     (`ATTR`, `GID`) — `GameSettingsChangeNotification.cs:17-21`.
   - в `GAME` добавить `PHST{HPID,HSLT}` и `THST{HPID,HSLT}`.
   - в `DATA` расширенных данных сессии добавить `ULST`.
   - пометить `SLOT` в `PDAT` как «эталонное значение неизвестно».
   - пометить запрос `LoginPersona` (`PNAM`) как «не подтверждено».
   - уточнить, что `STIM` в `TELE` — строка, а в `Ping` — число.
3. **Исправить комментарий** `наш:\Blaze\BlazeComponents.cs:3`: там `CL# 944265`,
   отправляем `CL# 1060080` (`Payloads.cs:38`) — как у эмулятора.
4. **Ограничить разбор расширенного размера.** `BlazeListener.cs:206` +
   `BlazeFrame.cs:52` дадут отрицательную длину при payload ≥ 16 МиБ.
   Добавить проверку/маску. (Правки — в `backend/`, это отдельная задача.)
5. **Зафиксировать в `MODLOG`:** openBlase **подтверждает** два наших решения,
   которые до этого опирались только на рабочий бэкенд — чтение счётчика
   list/map как varint (а не сырой байт) и чтение 2 байт старшей половины
   размера с индекса 12. При этом openBlase **негоден** как эталон по строковым
   TDF (off-by-one, `TdfString.cpp:35-39`) и по message ID / флагу
   расширенного размера (`main.cpp:335-341`).
6. **Для бота — реализовать отдельную Blaze-сессию на каждого бота:**
   `PreAuth` → `PostAuth` → `SilentLogin` (`PID`) или `Login`(`MAIL`) +
   `LoginPersona`(`PNAM`) → `UserAdded`+`UserUpdated` → `JoinGame`.
   Персону (personaref/ID/имя) бота надо завести у себя заранее — ровно так,
   как эмулятор держит список в YAML (`data\Blaze.Server.yml`).
   **`SilentLogin` (0x32) — предпочтительный путь:** он короче (один запрос
   вместо двух), возвращает `SESS` с `PDTL` и сам инициирует
   `UserAdded`/`UserUpdated` (`SilentLoginCommand.cs:51-52`).
7. **Слоты.** Наше «слот 1 = первый вошедший» подтверждено
   (`Game.cs:54` + `JoinGameCommand.cs:34-35`) — менять не нужно.
   Правило уникальности (`GameRegistry.cs:59-63`) оставить: эмулятор его
   не обеспечивает и при повторном входе выдаст дубль.
   Лимит `PMAX`/`MCAP` наш бэкенд обязан проверять сам — эмулятор не проверяет.
8. **Не тратить время на поиск `createPlayer`/`spawnSoldier` в эмуляторах —
   их там нет и не будет.** Путь из `SPAWN-RESEARCH.md` (guard page →
   `FUN_00A644B0` → `FUN_00A6CA00` → `FUN_009DBBD0`) остаётся единственным
   известным; эмуляторы его не подтверждают и не опровергают.

---

## Приложение: что осталось непроверенным

| Вопрос | Статус |
|---|---|
| Логи формата «`9B`» у эмуляторов | не проверено — эмуляторы не логируют размеры вовсе |
| Расширенный размер > 16 МиБ | не проверено (у NBE сломан, openBlase — не сервер) |
| Значение `SLOT` в `PDAT` | не проверено (эмулятор пишет константу 0) |
| Тело запроса `LoginPersona` (`PNAM`) | не проверено (эмулятор тело не читает) |
| Ответ `GetStatGroup` (бинарный блок) | не проверено (эмулятор возвращает пусто) |
| Тип `Float` в TDF | не проверено (эмуляторы не умеют кодировать) |
| Поля `AMAP`/`MSGS`/`NMAP` в `GetServerInstance` | не проверено (эмулятор их не шлёт) |
| `ServerPlayerCreate` / создание игрока на сервере | **нет** ни в одном эмуляторе |
| Понятие «соло» | **нет** ни в одном эмуляторе |
| Команда получения списка игроков | **нет** ни в одном эмуляторе |

---
---

# ЧАСТЬ 2. Что можно узнать без входа в игру

Добавлено 11.10.2026 после запроса «может, какие-то интересные находки уже
были в Blaze, чтоб не постоянно в игру заходить и проверять».

Источники этого раздела:

| Сокращение | Путь |
|---|---|
| `bk5.out` | `C:\Games\trash\2026-10-11\bk5.out` — лог нашего бэкенда, единственный прогон с `DBG`-списками полей |
| `probe5.out` | `C:\Games\trash\2026-10-11\probe5.out` — вход реального игрока |
| `g.out` | `C:\Games\trash\2026-10-11\g.out` — вход реального игрока + работа бота |
| `netmon.log` | `C:\Games\trash\2026-10-11\netmon.log` — сокеты процессов BF3 |
| `наш:` | `C:\Games\Legacy\backend\src\Legacy.Backend\` |

---

## 2.1. Главный ответ: готового создания игрока в эмуляторах НЕТ

Повторная проверка, теперь целиком по обоим деревьям (`ServerPlayerCreate`,
`PlayerCreate`, `createPlayer`, `spawnSoldier`, `AddPlayer`, `ServerPlayer`,
`solo`, `maxclients`, `playerlist`, `GetGameList`) — **ноль совпадений** в
`C:\Games\Legacy\New-Blaze-Emulator` и `C:\Games\Legacy\openBlase`.

Что игрок есть у эмулятора на самом деле — тройка, и ни один пункт не
создаётся из протокола:

1. `Client` — соединение: `NBE:\Base\Client.cs:13-48`
   (`ID`, `GameID`, `User`, `Type`, `Localization`, `InternalIP/Port`,
   `ExternalIP/Port`).
2. `User` — персона с полями `ID`, `Email`, `Name` —
   `NBE:\Base\Configuration.cs:31-38`. Список **заранее задан в YAML**:
   `NBE:data\Blaze.Server.yml:1-15` (три записи). Создать пользователя
   командой нельзя — только правкой конфига.
3. Запись `personaref` в `Game.Slots` — `NBE:\GameManager\Game.cs:50`,
   заполняется в `NBE:\Commands\GameManagerComponent\JoinGameCommand.cs:34`.

**Вывод: оба эмулятора — только серверная сторона Blaze. Ни один не дотягивается
до движка BF3 и до `ServerPlayer`.** Способ «создать бота мимо входа в игру»
в эмуляторах отсутствует как класс.

## 2.2. Но весь вход игрока делается БЕЗ игрового кода — это чистый Blaze

И это главная находка: **барьер «Blaze» можно пройти целиком, не запуская игру.**

- **Наш бэкенд уже умеет завести персону по имени без аккаунта и без
  предыдущего `Login`.** `наш:\Blaze\BlazeServices.cs:421-449`:
  `LoginPersona` читает `PNAM` (`:423`), и если профиля нет — **создаёт
  его**: `profile = store.Create(name);` (`:430`). Ответ `BUID/MAIL/PDTL/UID`
  (`:443-445`), следом `UserAdded` + `UserUpdated` (`:447`).
  Значит боту достаточно одного запроса `loginPersona PNAM="BOT_1"`.
- **У нас уже есть готовый Blaze-КЛИЕНТ, который работает без игры** —
  `наш:\Tests\ProtocolRoundTrip.cs`. Он поднимает слушатели
  (`:50-56`), подключается по TLS с проверкой сертификата
  `RemoteCertificateValidationCallback = … => true` (`:131-138`),
  строит кадры `BlazeFrame.Build(...)` (`:386-387`) и разбирает ответы
  `ReadReply` с пропуском уведомлений (`:358-380`). Он уже прогоняет
  **11 команд**: fetchClientConfig, preAuth, postAuth, getTelemetryServer,
  **loginPersona**, getStatGroup, getStatsByGroupAsync, getServerInstance,
  userSettingsSave/LoadAll, listUserEntitlements2, неизвестную команду,
  ping — `ProtocolRoundTrip.cs:148-339`. Комментарий на `:8-9` прямо
  признаёт: «она не подтверждает, что набор и имена полей совпадают с тем,
  что ждёт клиент BF3».
  **Это и есть готовая площадка, чтобы гонять бота без игры.**
- **Идентичность бота заводится по HTTP, тоже без игры:**
  `POST /profiles {"name":"BOT_1","cloneFrom":234471775}` →
  `наш:\Http\WebHost.cs:886-940`, внутри `store.CreateBot(name, cloneFrom)`
  (`:919`); `POST /accounts/ensure {"login","password","name"}` →
  `WebHost.cs:598-601`. Ровно это уже делает `join.bat:31` перед запуском
  игры.

Итого: **собрать «клиента-бота», который проходит всю цепочку Blaze, можно
сегодня, не заходя в игру ни разу.**

## 2.3. Реальная последовательность настоящего клиента (приоритет 2)

### Таблица A. Выделенный сервер (`bf3_lan.exe`) — единственный прогон с полями

Источник `bk5.out` (сессия 8, `MAIL="bf3_1"`). В скобках — размер ответа
в байтах из заголовка кадра.

| # | Компонент | Cmd | msgId | Поля запроса | Что это | Строка |
|---|---|---|---|---|---|---|
| 1 | 5 Redirector | 1 | 0 | `BSDK, BTIM, CLNT, CLTP, CSKU, CVER, DSDK, ENV, FPID, LOC, NAME, PLAT, PROF` | `getServerInstance` — **13 полей**, не одно `CLNT` | `bk5.out:403` |
| 2 | 9 Util | 7 | 0 | `CDAT, CINF, FCCR` | `preAuth` — **`FCCR` нет в нашем документе** | `bk5.out:428` |
| 3 | 9 Util | 2 | 1 | — | `ping` (пустой) | `bk5.out:437` |
| 4 | 1 Authentication | 40 (0x28) | 2 | `DVID, MAIL, PASS, TOKN, TYPE` | `login` — **5 полей**, не одно `MAIL` | `bk5.out:446` |
| 5 | 1 Authentication | 110 (0x6E) | 3 | `PNAM` | `loginPersona` | `bk5.out:455` |
| — | — | — | — | сервер шлёт `UserSessions 2` (UserAdded) | 114 байт | `bk5.out:460` |
| — | — | — | — | сервер шлёт `UserSessions 5` (UserUpdated) | 14 байт | `bk5.out:463` |
| 6 | 9 Util | 8 | 4 | — | `postAuth` | `bk5.out:470` |
| 7 | 30722 UserSessions | 20 (0x14) | 5 | `ADDR, NQOS` | `updateNetworkInfo`, `INIP=10.6.7.1:25300` | `bk5.out:479-480` |
| 8 | 9 Util | 1 | 6 | `CFID` | `fetchClientConfig`, `CFID="GOSAchievements"` | `bk5.out:488-489` |
| 9 | 9 Util | 5 | 7 | `CMAC, SNAM` | `getTelemetryServer` | `bk5.out:497` |
| 10 | 4 GameManager | 1 | 8 | `ATTR, BTPL, GCTR, GENT, GNAM, GSET, GTYP, GURL, HNET, IGNO, NRES, NTOP, PCAP, PGID, PGSC, PMAX, PRES, QCAP, RGID, SLOT, TCAP, TIDX, VOIP, VSTR` | `createGame` — **24 поля** | `bk5.out:506` |
| 11 | 4 GameManager | 15 (0x0F) | 9 | `GID, XNNC, XSES` | `finalizeGameCreation` | `bk5.out:521` |
| 12 | 4 GameManager | 3 | 10 | `GID, GSTA` | `advanceGameState GSTA=130` (PreGame) | `bk5.out:530-531` |
| 13 | 4 GameManager | 3 | 11 | `GID, GSTA` | `advanceGameState GSTA=131` (InGame) | `bk5.out:542-543` |
| 14 | 9 Util | 2 | 12,13,14… | — | `ping` каждые **20 секунд** | `bk5.out:556,567,637,648,659,670,681` |

Значения состояний из эмулятора: `GameState.PreGame = 0x82 = 130`,
`InGame = 0x83 = 131` — `NBE:\Blaze\Blaze.cs:58-59`. **Наши 130/131 верны.**

Новые поля запросов, которых нет в `BLASE-FIELDS.md`:
`FCCR` (PreAuth), `CMAC`+`SNAM` (GetTelemetryServer), `DVID`+`PASS`+`TOKN`+`TYPE`
(Login), `BTPL, GCTR, GENT, GURL, PGSC, RGID, SLOT, TIDX` (CreateGame),
`XNNC`, `XSES` (FinalizeGameCreation), `NQOS` (UpdateNetworkInfo).

> ⚠️ **Важно про `bk5.out`:** этот прогон сделан **старой сборкой** нашего
> бэкенда. В нём `Util 2` логируется как `postAuth cmd=2` (`bk5.out:557`) и
> на него уходит полный PostAuth-payload в 247 байт (`bk5.out:558`).
> В текущем коде `UtilCmd.Ping = 2` отвечает `STIM`-ом
> (`наш:\Blaze\BlazeServices.cs:196-202`), а `postAuth` — команда 8 (`:204`).
> Свежий лог `g.out:212` печатает `[blaze] postAuth (сессия 2)` уже без `cmd=`.
> **Вывод «cmd 2 = PostAuth» из `bk5.out` делать нельзя** — это был баг сборки,
> а не протокол. Эталон (`9` байт на `0009:0002`, `411` на `0009:0008`)
> остаётся в силе: `BLASE-FIELDS.md:51-55`.

### Таблица B. Игрок (`bf3.exe`) — полный цикл входа

Источник `probe5.out:222-234` (тот же ход в `g.out:231-243`).
Уровень лога INFO, поэтому **списков полей здесь нет** — только состав команд.

| # | Время | Событие | Строка |
|---|---|---|---|
| 1 | 13:27:13.459 | `redirector: connection 3` | `probe5.out:222` |
| 2 | 13:27:13.493 | `getServerInstance CLNT=0 -> 127.0.0.1:10041` | `:223` |
| 3 | 13:27:13.731 | `Blaze: connection 4` | `:224` |
| 4 | 13:27:13.760 | `preAuth SVCN="battlefield-3-pc"` | `:225` |
| 5 | 13:27:13.898 | **`silentLogin PID=234471788`** | `:226` |
| 6 | 13:27:13.965 | `postAuth` | `:227` |
| 7 | 13:27:16.716 | **`joinGame GID=1 GVER="67" игрок=KOTuk1 p=234471788 слот=1`** (через 2.75 с) | `:228` |
| 8 | 13:27:39.981 | `join-уведомления: клиент готов, отправляю (GID=1, слот=1)` (через **23 с**) | `:231` |
| 9 | 13:27:43.326 | `getStatGroup NAME="player_mpdefault2"` (сессия **2** — это сессия выделенного сервера) | `:233` |
| 10 | 13:27:46.364 | `getStatsByGroupAsync NAME=… VID=1 EID=[234471788] -> пустой реплай + notify 0x32 [234471788:95]` | `:234` |

**Ответы на вопросы по таблице B:**

- **Игрок заходит через `SilentLogin` (0x32), а не через `Login`+`LoginPersona`.**
  Выделенный сервер — да, через `Login`+`LoginPersona` (таблица A, шаги 4–5).
  Значит обе ветки надо поддерживать, и `SilentLogin` — основная для игроков.
- **`ListUserEntitlements2` в этом прогоне не присутствует** вообще.
  Значит клиент BF3 без него обходится — **для бота можно не слать**.
- **`UserSettingsSave`/`UserSettingsLoadAll` не присутствуют** — тоже необязательны.
- **`UpdateNetworkInfo` от игрока не залогирован** (наш лог для него `DBG`,
  `наш:\Blaze\BlazeServices.cs` обработчик → строка `[blaze] updateNetworkInfo`).
  Бэкенд это терпит: `наш:\Blaze\BlazeServices.cs:986-1001` — если
  `s.InternalIp == 0`, адрес игрока берётся из `cfg.GameServerIp`.
- **Пауза 23 с между `joinGame` и уведомлениями — наша**, а не клиентская:
  `s.JoinNotifyDue = DateTime.UtcNow.AddSeconds(20)`
  (`наш:\Blaze\BlazeServices.cs:819`), отправка в `TryFlushJoinNotify`
  (`:522-558`). Это привязка к загрузке карты.
- **`joinGame` несёт `GVER="67"`** — поле читаем
  (`наш:\Blaze\BlazeServices.cs:810`).

### Чего в логах НЕТ (честно)

- Списка полей для `silentLogin`, `joinGame`, `updateNetworkInfo` **от игрока**:
  в прогонах с игроком `DBG`-строка `запрос comp=` не включалась
  (в `bk5/bk4/bk3` она есть только для выделенного сервера).
- Ни одного кадра `JoinGame`/`SilentLogin` в hex — только разобранные имена.
- `GetStatGroup` идёт по **сессии 2** (выделенный сервер), а не по сессии
  игрока — похоже, клиент_stats-запрос делает процесс сервера.

## 2.4. Как клиент узнаёт адрес игрового сервера (следующий барьер)

**Что делает сервер (это в логах):**

| Что | Где | Значение в прогоне |
|---|---|---|
| Клиент шлёт свой игровой адрес | `UserSessions 30722 / cmd 0x14 / updateNetworkInfo`, поля `ADDR`, `NQOS` (`bk5.out:479`) | `INIP=10.6.7.1:25300` (`bk5.out:480`) |
| Сервер отдаёт адрес игры клиенту | `GameManager 0x16 InitiateConnections` → `GAME.HNET` — `наш:\Blaze\BlazeServices.cs:556-557`, `наш:\Blaze\Payloads.cs:708` | `HnetListClient(ip, port)` (`Payloads.cs:753-764`) |
| Конец рукопожатия | `GameManager 0x1D UpdateMeshConnection`, поля `GID`, `TARG` — `наш:\Blaze\BlazeServices.cs:841-911` | при `STAT=2` шлём `0x74`+`0x1E` (`:874-878`), при `STAT=0` — `PlayerRemoved` (`:888-910`) |

**Важно про `HNET`:** наш комментарий (`наш:\Blaze\Payloads.cs:744-752`)
объясняет, что байт `02` — это **не стаб**, а номер члена union `IPPairAddress = 2`.
Эмулятор объясняет то же самое иначе: флаг `Stub`, пишущий `02` после
счётчика (`NBE:\Blaze\TdfEncoder.cs:163-166`), и активный член
`NetworkAddressMember.IPPAirAddress` (=2) — `PlayerJoiningNotification.cs:32`.
**Значение совпадает, трактовка — нет.** Наша версия снята с рабочего
бэкенда и сверена байт-в-байт с `createGame` реального сервера
(`bk5.out:505`, там `HNET` в кадре `createGame`) — оставляем нашу.

**Что делает клиент (по `netmon.log`) — сверил все 125 строк:**

| Порт | Сколько раз | Вид |
|---|---|---|
| `127.0.0.1:42127` | 18 | TCP ESTABLISHED — редиректор |
| `127.0.0.1:10041` | 18 | TCP ESTABLISHED — ядро Blaze |
| `127.0.0.1:48377` | 18 | TCP **SYN_SENT** (зависшие, заблокированы) |
| `127.0.0.1:48378` | 18 | TCP **SYN_SENT** (зависшие, заблокированы) |
| `0.0.0.0:3659` | 16 | UDP (слушающий) |
| UDP эфемеритные | — | по 2 на запуск |

Повторяемость 18/18/18/18 говорит, что это **обязательная** часть старта
процесса BF3: ровно одна связь с редиректором, одна — с ядром Blaze и
**две зависшие попытки к 48377/48378**. Порт **25300 в этом окне не
зафиксирован** — netmon снимался в прогонах без зашедшего игрока, либо
соединение делает другой процесс. **Отдельно проверить нельзя без нового
прогона.**

Про 48377/48378: назначение **не установлено** — гипотеза «телеметрия/список
серверов EA» не проверялась.

## 2.5. Гостевого / анонимного входа нет

`ANON` встречается только как **поле ответа**, и всегда равно 0:
`NBE:\Commands\UtilComponent\PreAuthCommand.cs:36`,
`PostAuthCommand.cs:29`, `GetTelemetryServerCommand.cs:16`. Команды входа
без пароля/без пользователя нет ни в одном дереве.
Ближайшее к «гостю» — `SilentLogin(0x32)`: по одному `PID`, без `MAIL`
и без `PASS`. Персона при этом **должна существовать заранее** —
иначе отказ: `наш:\Blaze\BlazeServices.cs:396-408` (и сделано это специально,
чтобы не плодить мусорные профили).

## 2.6. Мёртвый код, найденный при разборе

- `Payloads.SessionExtended` (`наш:\Blaze\Payloads.cs:531-542`) —
  **объявлен, но ни разу не вызывается**. Мы **не шлём** уведомление
  `UserSessions 1`. Эмулятор шлёт его с флагом `joining` хосту
  (`NBE:\Notifications\UserSessionsComponent\UserSessionExtendedDataUpdateNotification.cs:54-64`).
  Клиент работает и без него — значит поле необязательное.
- `Payloads.GameServerAddress` / `GameServerIp` / `GameServerPort`
  (`наш:\ServeCommand.cs:185,261,281`) — используются, ок.
- В `GameSetupNotification` у эмулятора нет `GSET` (TODO,
  `NBE:\...\GameSetupNotification.cs:29`), у нас он есть и стоит **перед**
  `GSID` из-за строгого алфавитного порядка (`наш:\Blaze\Payloads.cs:703-705`).

## 2.7. Что можно проверить дальше, не заходя в игру

1. **Дописать в `ProtocolRoundTrip` сценарий «бот-игрок»**: `getServerInstance →
   preAuth → loginPersona → joinGame → (ловим 0x16) → updateMeshConnection(0x1D)`.
   Всё нужное уже есть в файле: клиентский каркас (`:121-341`), `Expect`
   (`:383`), `ExpectRaw` (`:263`), чтение уведомлений (`:358`).
   Это закроет барьер Blaze целиком и даст эталонные байты без игры.
2. **Проверить `NQOS` против `NQQS`** (см. правку в `BLASE-FIELDS.md`).
   Сделать можно только живым клиентом — наш собственный бот-клиент тут
   ничего не докажет (он согласится с обоими именами).
3. **Ответить на вопрос про 48377/48378** — снять `netmon.log` в прогоне
   с зашедшим игроком; там же должен появиться 25300.
4. **Не трогать** `Login`/`SilentLogin`/`JoinGame`/`UpdateMeshConnection`
   в эмуляторах как в эталон по *значениям* — там баги (`:34` без проверки
   дубля, `PlayerRemoved` со значением причины по умолчанию 1,
   `GetStatGroup` пустой, `GetStatsAsyncNotification` пустой).

## 2.8. Ответ на исходный вопрос про барьер «игровой протокол (порт 25300)»

**Честно: это отдельный барьер, и из перечисленных источников он не
проходится.**

- Эмуляторы Blaze **ничего не знают** про игровой протокол: они заканчиваются
  на `InitiateConnections`/`UpdateMeshConnection`.
- `SPAWN-RESEARCH.md` описывает **внутри движка** (`ServerPlayerManager`,
  `createPlayer` RVA `0xA644B0`, `addToArray` `0xA6CA00`, `spawnSoldier`
  `0x9DBBD0`) — это код выделенного сервера, а не бот-клиент.
- Из логов известно только, что клиент, получив `GAME.HNET`, поднимает
  соединение на 25300 и возвращается с `UpdateMeshConnection(TARG, STAT=2)`.
  **Формат сообщений этого соединения в `bk5`/`probe5`/`g.out`/`netmon.log`
  не зафиксирован** — бэкенд его не видит, netmon в тех прогонах его не
  поймал.

Практический вывод: **Blaze-часть бота можно довести и отладить полностью
без игры** (через `ProtocolRoundTrip`). Дальше нужен либо второй
инструмент-захват трафика 25300, либо — как сейчас — живой вход.

## 2.9. Что исправлено в нашем коде и документах (приоритет 3)

Правки внесены 11.10.2026. **Сборка НЕ выполнялась** (`dotnet`/`msbuild`
запускать запрещено) — правки только очевидно верные, компиляция не проверена.

### Код

| Файл:строка | Было | Стало |
|---|---|---|
| `наш:\Blaze\BlazeComponents.cs:3-8` | комментарий `CL# 944265`, «в нашем бэкенде» | `CL# 1060080`, источник — `New-Blaze-Emulator\...\PreAuthCommand.cs:79`; отмечено, что отправляемое значение совпадает (`Payloads.cs:38`) |
| `наш:\Blaze\BlazeComponents.cs:112` | уведомления `0x6E` не было | добавлено `public const ushort GameSettingsChange = 0x6E;` между `PlayerRemoved` и `GameStateChange` |
| `наш:\Blaze\BlazeFrame.cs:50-58` | `payload \|= extendedHigh << 16;` — при `extendedHigh >= 0x8000` (payload ≥ 16 МиБ) длина становилась **отрицательной** | сложение в `long` + ограничение `int.MaxValue`; маска `& 0xFFFF` |

Больше в коде ничего не менялось: остальное из списка «расхождений» оказалось
не расхождением (см. ниже).

### Документ `наш:\BLASE-FIELDS.md`

| Что | Где | Суть |
|---|---|---|
| Активный член union `ADDR` в `GetServerInstance` | раздел Redirector | `IPAddress (4)` → **`XboxClientAddress` (0)**, с ссылками на оба источника |
| `AMAP`, `MSGS`, `NMAP` | там же | добавлены в таблицу ответа + пометка, что эмулятор их не шлёт и подтверждения нет |
| Запрос `GetServerInstance` | там же | добавлен реальный состав из 13 полей (`bk5.out:403`) |
| Уведомление `0x6E` | UserSessions | добавлено в таблицу уведомлений |
| `ULST`, тип `BPS`, `PSLM` | UserSessions | добавлены в описание `DATA` |
| `PHST`/`THST`, `REAS`/`VALU` в GameSetup | GameManager | добавлены; отмечено, что `PHST`/`THST` мы **уже шлём** (`Payloads.cs:722-732`) — править код не нужно |
| `UpdateMeshConnection (0x1D)` | GameManager | добавлен целиком: `GID`, `TARG`, `STAT=2/0`, ссылки на эмулятор и наш код |
| Уведомления `0x1E`/`0x74` | GameManager | добавлены в порядок после `joinGame` |
| `SID` | `PDAT`/`PROS` | добавлено обоснование «слот 1 = первый» через `Game.cs:54` |
| `SLOT` | `PDAT`/`PROS` | помечено «эталонное значение неизвестно» |
| `NQQS` vs `NQOS` | `GAME` | помечено как **непроверенное** расхождение |
| `PNAM` в `LoginPersona` | Authentication | помечено ⚠️ «не подтверждено эмулятором» + реальный состав `Login` из 5 полей |
| `ListUserEntitlements2` | Authentication | описано поведение «`ETAG=ONLINE_ACCESS` → пусто», что совпадает с нашим кодом |
| `STIM` | Ping / GetTelemetryServer | добавлено предупреждение: в `Ping` число, в `TELE` строка |
| Формат `Stats 0x32` | GetStatsByGroupAsync | помечено, что старая таблица описывает заглушку эмулятора; реальный формат — в `BlazeServices.cs:605-613` |
| `HNET` | `GAME` | добавлена ссылка на **второе** независимое подтверждение (`openBlase`) и разбор расхождения трактовок |
| Типы TDF | таблица правил | добавлено: `Float = 0xA` есть в enum, но кодировщик его не пишет — остаётся неподтверждённым |

### Что из «расхождений» оказалось НЕ расхождением

- **`PHST`/`THST`** мы уже шлём (`наш:\Blaze\Payloads.cs:722-732`) — документ
  просто отставал.
- **`STIM` как строка** в `TELE` — так и в эталоне, у нас так же
  (`наш:\Blaze\Payloads.cs` — отправляем `"Default"`).
- **`NQOS`** пришлось оставить как есть: расхождение с эмулятором есть, но
  наше имя снято с рабочего бэкенда, а менять его без живого клиента —
  означает рискнуть рабочим входом в игру. Помечено как непроверенное.


