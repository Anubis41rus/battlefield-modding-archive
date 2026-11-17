# DESIGN-LOCAL — навигация, аккаунты в LAN, идеи из BF-Portable, новый бэкенд, проблема активации VU

Дата 2026-10-08. Ответы на четыре вопроса + решения по новому бэкенду.

---

## 1. Навигация: правка. VU карты НЕ печёт

Вы правы, я сформулировал неудачно. Проверено:

- В VU навмеш **вшит в уровень**: `LevelData.pathfindingBlobInfo` →
  `[PathfindingBlob](/vext/ref/fb/pathfindingblob)`
  (`vext\ref\fb\leveldata.md`, строка 30). Его запекает пайплайн DICE и он
  приезжает вместе с картой. В API VU есть и `PathfindingSystemEntityData`,
  `PathfindingBuildSettingsAsset`, `PathfindingReplayMode`, `PathfindingDebugSettings`
  — но это чтение/отладка, не запекание на сервере.
  ⇒ **VU-боты получают навигацию из движка, сервер ничего не печёт.**

- То, что я назвал «пеками», — это **ваша собственная система** в
  `C:\Games\BF3LegacyLan\world\`: 41 файл, 751 МБ, по 20 карт:
  - `<Карта>_<Режим>.grid` — сетка проходимости, магия **`FRGD`**
    (Frostbite GRiD), 6.7 МБ на карту;
  - `<Карта>_<Режим>.cover` — данные укрытий, магия **`OCRV`**, 515 КБ;
  - `<Карта>_<Режим>.edits` — **правки человека поверх выпеченной сетки**.
    Формат `cx cz block|allow`, комментарии с координатами в мм.
    Из заголовка файла: «Пишет и читает панель (Зоны карты), читает мозг при
    подъёме раунда, пишет также `tools/gridview/gridview.py diff`. Перевыпечка
    мира файл не трогает.»

  Это инфраструктура **нативных ботов botlayer**, к VU отношения не имеет.

**Вывод для плана:** для `fun-bots` эта система **не нужна** — Lua-боты берут
навигацию движка (тот же `PathfindingBlob`). Фаза 5 «навигация» из плана
убирается как отдельная задача; `world\` остаётся как артефакт старого
нативного слоя и в Legacy не переносится.

Побочный плюс: не надо ни печь, ни хранить 751 МБ сеток, ни возить их гостям.

---

## 2. Профили, аккаунты, ранги — как синхронизируются в LAN

### 2.1 Модель: один источник истины, а не peer-to-peer

```
            ┌───────────────────────────┐
            │  ХОСТ: Legacy.exe + Legacy.Backend
            │  profiles\<id>\progress.json  ← ЕДИНСТВЕННЫЙ источник истины
            │  progress\bf3-stats-catalog.json
            └───────────┬───────────────┘
                        │ Blaze по LAN (TCP 42127 → 10041 → UDP 25300)
        ┌───────────────┼───────────────┐
     гость 1          гость 2        гость 3
   (свой personaref,   своя статистика
    но ВСЯ статистика
    живёт у хоста)
```

**Никакой синхронизации между клиентами не нужно.** Прогресс — серверное
состояние, ровно как у Zeus. Клиент его не хранит и не рассылает; он его
запрашивает у бэкенда:

```
клиент → getStatGroup("player_mpdefault2")  = определения (лестница рангов)
         ← из progress\bf3-stats-catalog.json (один раз, кэшируется)
клиент → getStatsByGroupAsync(EID=<personaref>) = значения
         ← из profiles\<personaref>\progress.json
```

`personaref` (он же `EID`, `PID`) — это ключ. У гостя он свой, поэтому его
статистика автоматически попадает в свою папку на хосте. Никто ничего не
«синхронизирует» — просто все обращаются к одному бэкенду.

### 2.2 Что хранится где

| Что | Где | Чей |
|---|---|---|
| `progress\bf3-stats-catalog.json` — определения (1432 стата) | хост | общий, только на чтение |
| `profiles\<personaref>\profile.json` — имя солдата | хост | игрок |
| `profiles\<personaref>\progress.json` — **1432 значения** | хост | игрок |
| `profiles\<personaref>\settings\{cust,sdt}.bin` | хост | игрок (кастомизация) |
| `profiles\<personaref>\reports\*.rpt` | хост | игрок |
| Мод-файлы, конфиг сервера | хост, раздаются гостям | общий |
| Локальный кэш клиента, логи, настройки графики | **у гостя** | клиент |

Итого: **прогресс и аккаунты — на хосте; у гостя только локальный кэш и логи.**

### 2.3 У кого играет хост — у того и база

Сейчас `profiles\<id>\` лежат на диске конкретной машины. Это нормально для
LAN-пати: кто создал сервер, у того и валюта. Варианты на будущее:

- **Вынести в SQLite** (однофайловая БД, `Microsoft.Data.Sqlite` уже в .NET) —
  если захотим историю матчей, транзакции, запросы по статистике.
- **Экспорт/импорт профиля** — файл `legacy-profile-<id>.json`, чтобы гость
  принёс свою статистику на другой хост. Простое и полезное.

Решение — **JSON-файлы, как сейчас**. SQLite добавляем только если появится
реальная потребность (матч-история, топы).

### 2.4 Аккаунты

Сейчас: `login MAIL=`, `loginPersona PNAM=`, `silentLogin PID=` — три способа
входа уже есть в бэкенде. Для LAN это достаточно: аккаунт = папка
`profiles\<id>\` + имя солдата. Регистрацию с паролем делаем только если
захотим «настоящие» аккаунты для красоты (в BF-Portable это `register=1`,
`forcePW=1`, `emailDomain=@portable.local` — фейковые почты).

**Синхронизация паролей не нужна** — пароля нет, логин = personaref.

---

## 3. Идеи из `C:\Games\BF-Portable` (это не «хамп», это готовый BattleLog-аналог)

### 3.1 Что там вообще

Полный XAMPP-стек: `apache` + `mysql` + `php` + `phpMyAdmin` + `sendmail`,
и поверх — эмулятор BF3 с настоящей веб-статистикой.

| Компонент | Файл | Что делает |
|---|---|---|
| Blaze-эмулятор | `BF3-EMU\BlazeServer3.exe` (459 КБ, .NET + `MySql.Data.dll`) | сервер Blaze для BF3 |
| Редиректор | `Redirector\gosredirector.ea.com.exe` (964 КБ) + `main_cert.pfx`, `redi_cert.pfx`, `373243-gosprapp356.ea.com.pfx` | перехват редиректора EA с TLS-сертификатами |
| Локальный DNS | `DNS\DnsServer.exe` (434 КБ), `config.ini`: `dns_host=localhost`, `ip_host=127.0.0.1` | EA-домены резолвятся локально, hosts править не надо |
| Клиентский прокси | `Client-Files\dinput8.dll` (171 КБ) + `lanbf3.exe` (33 МБ) | инъекция без админа |
| **Веб-статистика** | `Xampp\htdocs\` | см. ниже |

`htdocs` — это **BattleLog для BF3**: `getServerBrowser.php`,
`getServerInfo.php`, `getSoldierStats.php`, `getSoldierAssignments.php`,
`getSoldierAwards.php`, `getSoldierWeapons.php`, `getSoldierVehicles.php`,
`getSoldierEquipments.php`, `getSoldierVehunlocks.php`, `getdogtags*.php`,
`getemblems*.php`, `saveRankMod.php`, `saveSettings.php`, `saveemblems*.php`,
`savedogtags*.php`, плюс страницы `serverbrowser.php`, `playerstat.php`,
`leaderboard.php`, `register.php`, `login.php`, `settings.php`,
`assignments.php`, `weapons.php`, `vehicles.php`, `vehUnlocks.php`,
`awards.php`, `dogtagsA/B.php`, `adminSettings.php`.
Локальные зеркала доменов EA: `battlelog.battlefield.com\`,
`bf4.gos.ea.com\`, `spring14.gosredirector.ea.com\`.
База данных: `htdocs\includes\connectdb.php`, `config.cfg`
(`user=bf4`, `password=bf4`, `allowWAN=0`, `register=1`, `forcePW=1`,
`emailDomain=@portable.local`, `serverBrowserUpdateInterval=10000`).

### 3.2 Что берём в Legacy, а что нет

| Идея из BF-Portable | Берём? | Как у нас |
|---|---|---|
| **Веб-сервер + JSON-эндпоинты** (`getServerBrowser`, `getServerInfo`) | **да, идея** | `HttpListener` уже есть в бэкенде; сделаем `GET /servers`, `GET /servers/{id}` |
| **Аккаунты с регистрацией** | **да, идея** | personaref + `profile.json`; пароли опционально |
| **Статистика солдата, ранги, лидерборд** | **да, идея** | `progress.json` уже есть; добавим веб-страницу статистики в `Legacy.exe` |
| `statnames.dat` (29.9 КБ) — имена статов | **да** | наша `bf3-stats-catalog.json` уже это содержит (1432 имени + режим) |
| `statsdata*.txt`, `playerstatsunlock.txt` | **да** | то же, каталог прогресса |
| `mapThumbs\*.jpg` — превью карт | **да, для красоты** | взять как референс/ресурс для UI `Legacy.exe` |
| `img\weapons\*.png`, `img\vehicleunlocks\*.png` | **да, для красоты** | то же — UI списка оружия/техники |
| `Xampp` (Apache+MySQL+PHP) | **нет** | мы пишем C#-сервис; MySQL/PHP — лишние зависимости |
| Локальный DNS-сервер | **позже, если нужен** | гость запускает `Legacy.exe`, он и так настраивает редирект ⇒ DNS не нужен |
| `dinput8.dll` прокси + `lanbf3.exe` | **как запасной вариант** | `activation-loader` уже работает и не требует подмены файлов игры; dinput8 оставляем на случай, если гость не может запускать через loader |
| Редиректор + TLS-MITM-сертификаты EA | **у нас уже лучше** | наш бэкенд делает `sinkhole` + `gen-cert` сам |
| Полный `getSoldier*` аналог BattleLog | **не сразу** | это веб-морда, а не то, что нужно для игры |

**Вывод по «хамп»:** `BF-Portable` — не хамп, а готовая референс-реализация
BattleLog. Из неё берём **структуру функций** (список серверов, статистика,
ранги, аккаунты) и **материалы для UI** (превью карт, иконки). Реализацию
делаем на C#, без PHP/MySQL.

---

## 4. Новый бэкенд «Legacy» — решение: пишем свой

Вы правы: чёрный ящик Native AOT без исходников — плохая основа, тем более
что инжекты в него всё равно придётся делать новые. **Пишем свой.**

### 4.1 Основа

| Что | Откуда | Зачем |
|---|---|---|
| TDF-кодек (кодирование/декодирование блоков Blaze) | `Legacy\New-Blaze-Emulator\Blaze.Server\Tdf*.cs` взят как база, **но сверка показала: счётчик list/map пишется сырым байтом вместо varint — НЕВЕРНО**; свой кодек: `Legacy\backend\src\Legacy.Backend\Tdf\TdfCodec.cs`, эталон — `TdfWriter.cs`/`TdfReader.cs` рабочего бэкенда | без этого Blaze не говорим |
| Референс протокола | `Legacy\New-Blaze-Emulator` (78 `.cs`), `Legacy\openBlase` | сверка полей и типов |
| Наблюдаемое поведение текущего бэкенда | `C:\Games\re\backend-utf16.txt` (24 697 строк) | точная картина: что за сервисы, логи, ошибки, формат payload |
| Инвентарь Blaze-команд | строки `[blaze] …` в том же файле | список сервисов, которые надо реализовать |
| Каталог прогресса | `BF3LegacyLan\progress\bf3-stats-catalog.json` + `getStatGroupPayloadBase64` | лестница рангов отдаётся как есть, переписывать не надо |
| Примеры профилей | `BF3LegacyLan\profiles\` | формат `profile.json`, `progress.json`, `cust.bin`, `sdt.bin` |

### 4.2 Что реализуем (по подтверждённым логам текущего бэкенда)

| Слой | Методы | Источник истины |
|---|---|---|
| **Редиректор** | 42127 → адрес бэкенда | `ServerIP.txt`-схема |
| **Blaze-службы** | `preAuth`, `postAuth`, `login(MAIL)`, `loginPersona(PNAM)`, `silentLogin(PID)`, `profile`, `fetchClientConfig`, `listUserEntitlements2`, `userSettingsLoad/Save/LoadAll`, `updateNetworkInfo`, `getStatGroup`, `getStatsByGroupAsync`, `getTelemetryServer`→loopback, `getServerInstance` | подтверждено строками `[blaze]` |
| **Игры** | `createGame`, `finalizeGameCreation`, `advanceGameState`, `setPlayerCapacity`, `updateGameName`, `removePlayer`, `destroyGame`, `replayGame` | там же |
| **Игроки** | `joinGame(GID,GVER)`, `PlayerJoining`, `ClaimingReservation`, `UserAdded`, `UserUpdated`, `JoiningPlayerInitiateConnections` | там же |
| **QoS** | UDP-пробы игроков | `[qos] listening` |
| **HTTP (новое)** | `GET /servers`, `GET /servers/{id}`, `POST /servers/{id}/register`, `GET /profiles/{id}`, `GET /servers/{id}/mods` (раздача модов) | аналог `getServerBrowser.php` из BF-Portable |
| **EA-заглушка** | `sinkhole`, `gen-cert` | уже есть в текущем, переносим |
| **Хранилище** | `profiles\<id>\*.json`, `progress\*.json`; позже SQLite | текущий формат |

### 4.3 Порты (наши, из `server.config` + `README.MD`)

| Порт | Что |
|---|---|
| TCP 42127 | редиректор (адрес, который клиент вводит) |
| TCP 10041 | Blaze-ядро бэкенда |
| UDP 25300 | игровой порт (данные клиент↔сервер) |
| TCP 47900 | RCON |
| TCP 8080 | **новое:** HTTP — список серверов, статистика, раздача модов |

Схема `BF3LegacyLan` использует те же 42127/10041/25300 — гостю ничего
нового запоминать не надо.

### 4.4 Как это вписывается в порядок работ

Новый бэкенд — это **фаза 2 целиком**, а не «дописать». Порядок внутри фазы:

1. TDF-кодек + транспорт + каркас `serve` (из `New-Blaze-Emulator`).
2. Редиректор + QoS.
3. `fetchClientConfig` / `preAuth` / `postAuth` / `login*` — **клиент заходит**.
4. Игры: `createGame`/`joinGame`/`UserAdded`/`JoiningPlayerInitiateConnections` — **матч**.
5. Прогресс: `getStatGroup` (отдать готовый base64) + `getStatsByGroupAsync` — **ранги видны**.
6. HTTP: `/servers`, регистрация сервера, раздача модов.
7. `sinkhole` + `gen-cert`.

Критерий фазы: **существующий сценарий (`host.bat` → `join.bat`) работает
на новом бэкенде**, плюс `GET /servers` отдаёт JSON.

---

## 5. Проблема активации VU — и почему нас она не касается

### 5.1 Что происходит у вас

1. VU при запуске требует **аккаунт veniceunleashed.net** — логин/пароль
   (`general\playing.md`: «Before you can play VU, you will need to register an account»).
2. Затем — **привязка EA/Origin-аккаунта** для проверки владения BF3:
   `general\args.md`: `-activate -lsx` («EA app / Origin, which must be running and
   be logged in to an account that owns BF3»), `-activate -o_mail/-o_pass`,
   `-activate -ea_token <token>`. В игре — кнопка «Create account» и запрос
   Origin-токена.
3. После этого клиент идёт на **Zeus** — облачный бэкенд VU (`-env prod|dev`,
   оба варианта удалённые). Логина нет ⇒ нет подключения ⇒ нет меню серверов.

То есть ваш инжектированный BF3 не может «пройти» не потому, что инъекция
плохая, а потому, что **проверка владения игрой делается через интернет и
Origin-логин**, а не локально.

### 5.2 Можно ли заставить работать именно VU офлайн

Короткий ответ: **это и есть ровно та работа, которую мы делаем как Legacy.**

Чтобы VU заработал без интернета, пришлось бы подменить Zeus, а Zeus — это:

| Функция Zeus | Что надо реализовать | У нас уже есть? |
|---|---|---|
| Аккаунты и логин | API аутентификации по их протоколу | своё — проще |
| Солдаты и прогресс | их формат хранения | своё — проще |
| Реестр серверов | их API регистрации сервера | своё — проще |
| Проверка владения BF3 | перехват проверки Origin/EA | `sinkhole` + `gen-cert` уже делают часть этого |
| Раздача обновлений (`-updateBranch`) | их CDN-протокол | не нужен |
| NAT-детект | их API видимости | не нужен в LAN |
| TLS к Zeus | их сертификаты и pin | не нужен |

Плюс: `veniceext.dll` всё равно цепляется к **нашей** версии BF3 своими
221 адресом движка — эти адреса надо держать в актуальном состоянии, а мы
уже знаем, что они жёсткие.

**Вывод:** пилить VU офлайн = писать Legacy, только с худшим результатом
(чужой протокол + чужие лицензионные ограничения + зависимость от чужого
формата). Поэтому в Legacy эта проблема **устранена по построению**:

| Проблема VU | Как решена у нас |
|---|---|
| Нужен аккаунт на veniceunleashed.net | **не нужен** — аккаунт = папка `profiles\<id>\` |
| Нужно подтвердить владение BF3 через Origin | **не проверяем** — локальный редиректор уже подменяет EA (`sinkhole`, `gen-cert`) |
| Нужен Zeus в интернете | **нет Zeus** — бэкенд наш, в LAN |
| Нужны обновления VU | **не нужны** — BF3 и VU не обновляются |

### 5.3 Практический шаг, если захочется «просто посмотреть VU»

`vu.exe` можно запустить и посмотреть интерфейс — он откроет окно входа и
попросит аккаунт. Дальше без интернета не пройти. Логичный порядок: не
тратить время на обход, а вкладываться в `legacyext.dll` — тогда в том же
окне `Legacy.exe` будет наш логин без пароля и наш список серверов.

---

## 6. Итоговые решения (зафиксированы)

| Вопрос | Решение |
|---|---|
| VU печёт карты? | **Нет.** Навмеш вшит в уровень (`PathfindingBlob`). `world\*.grid/.cover` — наша система для нативных ботов, в Legacy не переносится. |
| Как синхронизируются аккаунты/ранги в LAN? | **Один источник истины** — бэкенд хоста. Клиенты только читают. Peer-to-peer синхронизации нет и не нужна. |
| Идеи из BF-Portable? | Берём структуру (список серверов, статистика, ранги, аккаунты) и материалы для UI. Не берём Apache/MySQL/PHP и TLS-MITM EA. |
| Бэкенд? | **Пишем новый** на C#, основа — `New-Blaze-Emulator` (TDF-кодек). Поведение сверяем по `backend-utf16.txt`. |
| Активация VU? | У Legacy проблемы нет: нет Origin-проверки, нет Zeus, нет пароля. Пилить VU офлайн = делать Legacy с худшим результатом. |
| Фаза 5 «навигация»? | Убрана из плана — Lua-боты берут навигацию движка. |