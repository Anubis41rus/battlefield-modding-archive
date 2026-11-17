# Аудит внешних обращений (офлайн) — 17.10.2026

Метод: только чтение. Исходники, конфиги, скрипты; строки, импорты и дизассемблер собранных DLL (pefile/capstone, ничего не запускалось); локальные таблицы ОС (сокеты, DNS-кэш, правила брандмауэра, hosts). Сетевых запросов не слал, файлов проекта не менял.
Не проверялось: поведение `bf3.exe`, `bf3_server_Final.exe`, PunkBuster и EA app (вне области), UDP-трафик (нужен перехват пакетов), бинарники бэкенда. Исходников активатора в дереве нет — разбор по строкам, дизассемблеру и логам.

## Итог
- Код проекта (три лаунчера, LegacySetup, backend, legacyext, `run\*.ps1`, `*.bat`) **сам наружу не ходит**: в backend и legacyext нет исходящего сетевого кода, лаунчеры говорят только с `127.0.0.1` / LAN-IP и шлют UDP-broadcast 47990.
- Единственный запрос «в интернет» на уровне Lua — проверка обновлений fun-bots (`api.github.com`); он гасится заглушкой `Net` (callback(nil)), пакет не уходит.
- Живая проверка стенда (17:18): у `Legacy.Backend`, `bf3_server_Final`, `activation-loader` нет ни одного TCP-соединения не на `127.0.0.1`.
- Что реально грозит утечкой: (1) DNS-подмена активатора узкая и fail-open; (2) входящие порты открыты всем профилям, `/admin/*` без авторизации; (3) `BF-Portable\_StartServer*.bat` запускают сервер без подмены; (4) службы ОС (PunkBuster, EA app) ходят в сеть сами.

## 1. Таблица находок
Вердикт: **локально** / **исправить** (внешний вызов или лишнее расширение доступа) / **неясно**. Pn — пункт из §4.

| № | Место | Адрес / порт | Что делает и когда | Вердикт |
|---|---|---|---|---|
| 1 | `Ports.cs:31`, `Plans.cs:76,174`, `HostRunner.cs:83`, `MainWindow4.cs:63`, `ServerHost.cs:357`, `JoinService.cs:108`, `run\common.ps1:29`, `run\join.ps1:30`, `host.bat:19`, `join.bat:48`, `ext\start-server.ps1:6`, `server.config` (`backend.sinkholeHosts`) | `gosredirector.ea.com`, `.online.`, `.scert.`, `.stest.` | Список `BF3ACT_DNSREDIR`: на старте клиента и сервера активатор подменяет эти имена на локальный IP (§2) | локально, пока подмена жива (P1) |
| 2 | `join.ps1:31`; `Plans.cs:175`, `MainWindow4.cs:74`, `JoinService.cs:112`, `join.bat:52`; сервер: не задан (`Plans.cs:78`, `common.ps1`) | `BF3ACT_DNSREDIR_IP` = 127.0.0.1 или IP хоста; у сервера по умолчанию 127.0.0.1 | Куда резолвятся имена из п.1 | локально |
| 3 | `Payloads.cs:554-567`, `BlazeServices.cs:690-698` | HOST `gosredirector.ea.com`, IP=0, порт 10041 | Ответ редиректора: клиент идёт на Blaze по ИМЕНИ, значит всё держится на подмене | локально, зависит от п.1 |
| 4 | `Payloads.cs:176-192`, `:243-247` | QoS `<IP хоста>:17554`, метка `ams`; ticker на IP хоста | PreAuth/PostAuth отдают клиенту наши QoS и ticker | локально |
| 5 | `Payloads.cs:203-217`, `:223-242` | `127.0.0.1:9988` (телеметрия), `127.0.0.1:8443` (PSS) | Адреса для клиента и сервера: их собственная петля, слушателя нет | локально |
| 6 | `Plans.cs:120-123`, `HostRunner.cs:108-109`, `start.ps1:89-90`, `host.bat:79` | `-PingSite ams -Region EU -Login bf3_1 -Password password` | Метки QoS-сайта и учётка сервера в нашем Blaze; адресов нет | локально |
| 7 | `BlazeListener.cs:42`, `QosListener.cs:20`, `WebHost.cs:52-56` | TCP 42127 и 10041, UDP 17554 на `0.0.0.0`; HTTP 8080 на 127.0.0.1 и каждом IPv4 (192.168.0.10) | Входящие. Доступны из LAN даже при `--advertise-ip 127.0.0.1`: «только этот ПК» сокетами не обеспечен | исправить (P1) |
| 8 | `WebHost.cs:304-310` | `/admin/*`, `/rcon`, DELETE аккаунтов и солдат | Нет токена и проверки loopback: с любого достижимого адреса правятся Startup/MapList/ModList (в т.ч. `admin.password`) | исправить (P1) |
| 9 | backend `src` целиком | — | `HttpClient/WebClient/Connect` нет (только тест на loopback `ProtocolRoundTrip.cs:128`); `Overview()` объявляет `internet=false` | локально |
| 10 | `ServeCommand.cs:222-254`, `CertificateTool.cs:159` | автодетект IP, `Dns.GetHostName()` | Без `--advertise-ip` берётся первый не-loopback IPv4 (VPN/публичный); DNS своего имени — только в `gen-cert`. `ext\start-backend.ps1` флаг не передаёт | локально (P3) |
| 11 | `CertificateTool.cs:50-57`, `Otg3Cert.cs` | `gosredirector.ea.com`, `spring14.gosredirector.ea.com`, `373243-gosprapp356.ea.com`, `373244-gosprapp357.ea.com` | Имена в самоподписанных TLS-сертификатах, не резолвятся | локально |
| 12 | `LegacyLauncher.Core\BackendClient.cs:28,34`, `Legacy.Launcher\BackendClient.cs:60` | `http://{host}:{port}`, `UseProxy=false` | HTTP-API бэкенда: серверы, аккаунты, startup/maplist | локально |
| 13 | LegacyFriend: `MainWindowAuth.cs:61,121,215`, `MainWindow2.cs:40`, `MainWindow3.cs:70`, `MainWindowLan.cs:66`, `MainWindow4.cs:31-47` | `http://{ip}:8080/accounts/login`, `/accounts/ensure`, `/servers`, `/` | Вход гостя: логин и пароль по HTTP открытым текстом. `ip` = 127.0.0.1, хост из LAN-списка или введённый руками без проверки диапазона | локально (P3) |
| 14 | `Discovery.cs:89-106,149-153,349-361` | UDP 47990; 255.255.255.255 и broadcast каждого «Up» адаптера, в т.ч. `happ-default-tun` 10.6.7.1/30 | Каждые 3 с JSON (имя ПК, LAN-IP, серверы); слушает чужие | локально; на TUN слать не надо (P3) |
| 15 | `MainWindow4.cs:110-113`; `Gui MainWindow.xaml.cs:1145-1148`; `Legacy.Launcher SettingsTab.cs:177-214` | TCP 8080/42127/10041, UDP 25300/17554/47990 | Кнопки «Фаервол»: LegacyFriend и старый лаунчер без `remoteip`/`profile` (Gui — `profile=private`, без `remoteip`) | исправить (P1) |
| 16 | `LegacySetup\Program.cs:173`, `FileCheck.cs:79`, `install.ps1:102`, `README.txt:6` | `https://dotnet.microsoft.com/download/dotnet/10.0` | Только текст подсказки: ссылка не открывается и ничего не качает | локально |
| 17 | `Legacy.Backend.csproj:11` | NuGet `Portable.BouncyCastle 1.9.0` (nuget.org) | Restore при сборке идёт наружу, если пакета нет в кэше; в рантайме не нужен | исправить при желании (P3) |
| 18 | `*.xaml`, `LegacyEXT.vcxproj:3` | `schemas.microsoft.com` | Идентификаторы XML-пространств имён, не запрашиваются | локально |
| 19 | `legacyext\src` целиком; `legacyext.dll` | — | Нет winsock/WinHTTP/wininet/curl; импорт только KERNEL32 (158 функций). `http`, `socket`, `Zeus`, `battlelog`: заглушки `VextNet.cpp`, комментарий `ModdingInterface.cpp:196`, строка `MiddleClass.cpp:28`. `connect` — только имена enum | локально |
| 20 | `VextNet.cpp:31-144` + `fun-bots\ext\Server\UpdateCheck.lua:4-6,181` (вызов `__init__.lua:85`, Engine:Init) | `https://api.github.com/repos/Joe91/fun-bots/releases/latest?per_page=1` (+ `/releases`, `/tags`) | Мод просит `Net:GetHTTPAsync` на каждом старте. Заглушка зовёт callback(nil); в `legacyext.log`: «Net: запроса нет», «[UPDATE] Failed to check for an update» | локально (нейтрализовано); P3 до подключения WinHTTP (`VextNet.cpp:12-13`) |
| 21 | `LuaRuntime.cpp:393-466` | `io.popen`, `io.tmpfile`, `io.open` | Песочница убирает `os.execute/getenv`, `loadlib`, `load/dofile`, но не `io`; в Lua 5.4.2 для Windows `_popen` включён (`luaconf.h:50`, `liolib.c:68`) — мод может запустить `curl` | неясно: активного вызова нет, дыра есть (P2) |
| 22 | fun-bots: `README.md`, `mod.json`, `WebUI` (`BotEditor.js:16`), `fun-bots-helper\...\gets.py:9` | github, discord, paypal, `import requests` | Ссылки и метаданные; helper на стенде не запускается; вызовов `requests.` нет | локально |
| 23 | `BF-Portable\Battlefield_3_Server\_StartServer.bat` (+2,3,4) | — | Запускают `bf3_server_Final.exe` без `activation-loader`: подмены нет, сервер идёт к настоящему `gosredirector.ea.com` с `-Login bf3_1 -Password password` | исправить (P2) |
| 24 | `BF3LegacyLan\server.config:102-105`, `Legacy\LegacyBF3\server.config:33-35`, `probe_lua_door.bat` | `10071`, `127.0.0.1:25400` | Ключи `blazeServerPort`, `engineSocket`, `inProcessRedirect`, `keepSinkhole` ни лаунчеры, ни Legacy.Backend не читают (старый botlayer/бэкенд — не проверялось) | локально; убрать (P4) |
| 25 | `BF3LegacyLan\join.bat:12,31,53` | `http://%HIP%:8080/servers`, `/accounts/ensure` | `HIP` вводит человек, он же уходит в `BF3ACT_DNSREDIR_IP`; имя хоста дало бы обычный DNS | локально (P3) |
| 26 | `ServerOptions.cfg` (BF-Portable, dist), `*.json`/`*.config` комплектов | — | Адресов нет. `server.config` в BF-Portable отсутствует (есть в BF3LegacyLan и Legacy\LegacyBF3). dist: бинарники совпадают с рабочими | локально |
| 27 | живая ОС: `hosts`, сокеты, DNS-кэш | `master7.evenbalance.com` (72.167.223.241), `service-aggregation-layer.juno.ea.com` | В `hosts` про EA записей нет. В DNS-кэше имена PunkBuster и EA app; службы `PnkBstrA` (Auto) и `EABackgroundService` запущены, `PnkBstrA` слушает только `127.0.0.1:44301`. Кто резолвил — не доказано | неясно, вне кода проекта (P4) |
| 28 | `bf3.exe`, `bf3_server_Final.exe`, `EACoreServer.exe`, PB-клиент | Battlelog (`-webMode MP`), Origin/EA app, PB, UPnP/NAT-PMP | Не анализировались; UDP не наблюдался | неясно; страхуется только брандмауэром (P1) |

Не сеть: `legacy-launcher.json` хранит `lastPassword`, `FriendSettings.Password` лежит открытым текстом, RCON/админ-пароль `change-this-rcon-password` (RCON только на `127.0.0.1:47900`).

## 2. DNS-перенаправление активатора (`activation.dll`)
- Механизм: после того как упаковщик awc связал импорты, активатор подменяет в `.idata` процесса слот #52 ws2_32 (`gethostbyname`): клиент rva `0x1fbe018`, дедик `0x27de1c0`. `hosts` и права администратора не нужны.
- `BF3ACT_DNSREDIR`: разделители `,` `;` пробел, до 16 имён по 127 символов, всего до 1023. Сравнение имени целиком, без учёта регистра; масок (`*.ea.com`) нет.
- `BF3ACT_DNSREDIR_IP` (до 63 символов): по умолчанию литерал `127.0.0.1`. Совпавшее имя резолвится вызовом оригинального `gethostbyname` с этой СТРОКОЙ: IP-литерал DNS-запроса не создаёт, имя хоста вместо IP дало бы настоящий запрос.
- Что перенаправляется: ровно 4 имени из п.1 (во всех местах задания список одинаков). Назначение только `127.0.0.1` или IP хоста; наружных адресов в перенаправлении нет.
- По журналам: 31 клиентский прогон, везде «4 имён», 60 перехватов, все `gosredirector.ea.com` (50 → 127.0.0.1, 10 → 192.168.0.10), ни одного `⛔`. Дедик (старый `botlayer.log`): 4 имён на 127.0.0.1. Имена `.online/.scert/.stest` ни разу не запрашивались. Сейчас сервер держит `127.0.0.1 → :10041`, значит у сервера подмена работает.
- Ограничения (fail-open): (а) подменён только слот образа процесса: `gethostbyname` из других модулей, `getaddrinfo`, `GetAddrInfoW`, `DnsQuery` и прошитые IP не перехватываются; (б) имена вне списка идут в оригинальный `gethostbyname`, то есть в обычный DNS, и не логируются; (в) если слот не найден или не заполнился, активатор пишет «⛔ … имена EA резолвятся как есть … запасного пути нет (ADR 0022)» и клиент идёт к настоящему EA (MODLOG: без переменных клиент «молча умирает через ~37 с: ищет серверы EA»); (г) без `BF3ACT_DNSREDIR` подмена не ставится вообще.
- Сам активатор не ходит в сеть: `activation.dll` импортирует только KERNEL32, `activation-loader.exe` — KERNEL32 и ADVAPI32 (`OpenProcessToken`, `GetTokenInformation`). Файлы в `BF3LegacyLan`, `Legacy\LegacyBF3`, dist совпадают побайтно.

## 3. Брандмауэр и сеть ПК (`netsh show rule`, только чтение)
- Входящие Allow для Domain/Private/Public, RemoteIP Any: `Legacy-TCP`, `Legacy-UDP`, `Legacy LAN TCP/UDP …`, `BF3LAN-blaze`, `BF3LAN-game`. Программные правила (Private+Public): Legacy.Backend, LegacyFriend, LegacyLauncher(.Tests), bf3_server_Final, bf3.exe, BlazeServer, PnkBstrA/B. Исходящих блокировок для игры нет.
- Адаптеры: Ethernet 192.168.0.10/24 и TUN `happ-default-tun` 10.6.7.1/30. Всё, что подмена не перехватила, уйдёт в интернет через VPN. Публичного IPv4 на адаптерах нет: извне порты достижимы только через проброс или UPnP.

## 4. Рекомендуемые правки (код и конфиги не менял)
- P1. Fail-closed: исходящий Block для `bf3.exe` и `bf3_server_Final.exe` на всё, кроме 127/8, 10/8, 172.16/12, 192.168/16. Block сильнее Allow, поэтому исключения (подсеть VPN-друзей) вшивать в RemoteAddress блока. Пример для админа, не выполнялся: `New-NetFirewallRule -DisplayName 'Legacy block bf3' -Direction Outbound -Program 'C:\Games\Battlefield 3\bf3.exe' -RemoteAddress Internet -Action Block`. Включить `netsh advfirewall set allprofiles logging droppedconnections enable` и после прогона прочитать `pfirewall.log`: выяснится, что ещё просят игра и сервер (UDP, PB, UPnP).
- P1. Входящие: `profile=private remoteip=localsubnet` (плюс подсеть VPN-гостей) в `MainWindow4.cs:110-113` и `SettingsTab.cs:177-214`, поправить README BF3LegacyLan; лишние правила `Legacy LAN *`, `BF3LAN-*` убрать (нужен админ).
- P1. Бэкенд: `/admin/*`, `/rcon`, DELETE пускать только с loopback или по токену; при выключенном «открыть для LAN» привязывать Blaze, QoS и HTTP к 127.0.0.1 (`BlazeListener.cs:42`, `QosListener.cs:20`, `WebHost.cs:52-56`).
- P1. В `run\status.ps1` и `tests.ps1` проверять строку `dnsslot: слот #52 заполнился` (клиент — `activation-*.log`, сервер — файл `--log`) и отсутствие `⛔`; без неё прогон не считать офлайн.
- P2. `BF-Portable\Battlefield_3_Server\_StartServer*.bat` переименовать или убрать (нужно слово пользователя: BF-Portable без просьбы не трогаем).
- P2. Песочница Lua: обнулить `io.popen`, `io.tmpfile`, а `io.open` ограничить чтением/папкой мода (`applySandbox`, `LuaRuntime.cpp:427`); fun-bots `io.` и `os.execute` не использует.
- P3. fun-bots: убрать `require('UpdateCheck')` (`Server\__init__.lua:85`) или записать в `VextNet.cpp`, что настоящий WinHTTP без белого списка адресов подключать нельзя.
- P3. Ввод адреса хоста (LegacyFriend `AuthIpFromUi`, Gui, `join.bat`): принимать loopback, частные, link-local и 100.64/10, об остальном предупреждать. Discovery слать только на `Preferred`-адаптеры, без TUN (`Discovery.cs:351-361`). Всегда передавать `--advertise-ip`.
- P3. Сборка: локальный NuGet-источник для `Portable.BouncyCastle`, `DOTNET_CLI_TELEMETRY_OPTOUT=1`.
- P4. Убрать из `server.config` и README устаревшие ключи/порты 10071, 25400; пароль из `legacy-launcher.json`.
- P4. Вне проекта, решает пользователь: `EABackgroundService` и `PnkBstrA` дают внешний DNS/трафик с этого ПК; для чистого офлайн-прогона можно остановить (правило 5 AGENTS не нарушать).
