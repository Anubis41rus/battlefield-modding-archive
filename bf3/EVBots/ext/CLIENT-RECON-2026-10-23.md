# Клиентская часть EVBots — разведка (23.10.2026, шаг 1, только чтение)

Вопрос: как VU поднимает клиентский рантайм (клиентский VEXT) и как нам попасть в клиентский `bf3.exe`.
Ничего не правилось, стенд и клиент не запускались. Формат: **факт** (чем проверено) / **гипотеза** (не проверено).

## 1. Как это устроено у VU

**Факт (SEQUENCE-REPORT.md, шапка).** Один `veniceext.dll` работает и в клиенте, и на сервере. Сервер VU — это клиентский
`bf3.exe` в серверном режиме. Таблица движковых функций VU (`0x18E74Cxx..0x18E753xx`, ≈450 слотов) заполняется адресами
`bf3.exe`. Список слотов → адресов: `re\temp-archive\19a41ba6-bae4-453f-abd4-727985919b45\table.txt` (без имён).
Расшифрованный клиент для статики — `Battlefield 3\zbf3.exe` (тот же timestamp `0x511C9356`, база `0x400000`).
Значит, **адреса клиентских функций движка у VU уже есть** — в адресах нашего `bf3.exe`, а не `bf3_server_Final.exe`.

**Факт (строки `re\vu\veniceext.strings.txt` 28662–28714).** Сервер VU сам компилирует клиентские и shared-скрипты мода
(`Compiling client module...`, `Compiling client script '{}'...`), сжимает их (`Compressed client scripts from {} to {} bytes`)
и держит у себя. В `require` клиента ищутся `/ext/Client/` и `/ext/client/`, бинарные чанки запрещены.

**Факт (строки `re\vuic\strings\vu-core.dll.ascii.txt` 7865–7867, 8353–8409, 25150–25210, 25460–25462).** У VU есть
**свой сетевой канал рядом с игровым** — «Harmony»: RakNet (`RakPeer`, `ReplicaManager3`, `FileListTransfer`), порт
`0.0.0.0:25200` (`-mHarmonyPort`, `-listen`). Строки клиента: `Connecting to VU server with ID *{}* at *{}:{}*`,
`Received VeniceEXT module '{}'`, `Received truncated/malformed scripts…`, `Module '{}' has no compressed scripts. Falling back
to bytecode`, статусы UI `DOWNLOADING` → `CONNECTED` / `DOWNLOAD_FAILED`, потом `Connecting to frostbite server at {}:{}`.
Сервер: `Player '{}' is requesting to join the server as a {}`, `Player '{}' disallowed from joining the server by an extension`,
`Player '{}' has established a connection to the server`.
**Гипотеза (по порядку строк и статусам UI):** клиент VU сначала входит в Harmony сервера, получает модули (скрипты client+shared,
ui.vuic), и только потом подключается к игровому серверу Frostbite.

**Факт (декомпилят `re\vu\decomp\chunk00.decomp.txt:695917`, `FUN_1072e190`).** Рассылка NetEvent в veniceext идёт по
игрокам (`[pm+0x7c..0x80]`, `[pm+0x8c..0x90]`) и вызывает интерфейс, который отдаёт колбэк `[this+0x9c]`:
`vfunc+4(event, [игрок+0x20], [игрок+0x24], данные)`. То есть **сетевой транспорт NetEvents не в veniceext, а у хоста**
(vu-core; veniceext ищет в нём `GetConsoleLogger` через `GetModuleHandleA("vu-core.dll")`).
**Гипотеза:** этот транспорт — тот же Harmony/RakNet, а не игровое соединение движка. Проверить по `re\vu\pyghidra_mcp\vu.gpr`
(в нём `vu-core.dll`), когда Ghidra MCP поднят.

## 2. Что есть у нас

**Факт (`stand\run\join.ps1`, `common.ps1`).** Клиент запускается нашим `stand\activation\activation-loader.exe --client --game
"C:\Games\Battlefield 3" --drm-strict --license-artifact …`, без `--dll`.

**Факт (`stand\activation\logs\activation-20261023-043511.log`).** В клиенте работает `activation.dll`: режим `client`,
обход DRM strict, подмена DNS через слот `.idata` #52, «второй этап: ждём расшифровки кода» — код расшифрован через 4438 мс
(проба по `0x0074d3c0`), «патчи: таблица пуста». Наших хуков в клиенте нет.

**Факт (строка usage в `activation-loader.exe`).** `activation-loader (--client|--dedicated) --game <dir> [--exe <name>]
[--dll <name>] …` — `--dll` есть для обоих режимов.
**Факт (дизассемблер `activation-loader.exe`, 23.10, К1):** загрузчик внедряет **ровно одну** DLL. По умолчанию (`0x401980`–`0x401997`):
режим 1 → `botlayer.dll` и `bf3_server_Final.exe`, иначе → `activation.dll` и `bf3.exe`; `--dll` заменяет это значение. Внедрение
одно: `CreateProcessA` (подвешенный) → `VirtualAllocEx` + `WriteProcessMemory` пути → `CreateRemoteThread(LoadLibraryA)`
(`0x401DA7`–`0x401E04`, единственный вызов `CreateRemoteThread`) → ожидание → `ResumeThread` (`0x401F0B`). DLL ищется рядом с
загрузчиком («%s not next to the loader»). **Значит, `--client --dll EVBots.dll` заменит `activation.dll`: в клиенте не будет обхода
DRM и подмены DNS — клиент не войдёт.** (Вывод по коду; клиент не запускался.) Исходников `activation-loader.exe` и `activation.dll`
в `C:\Games` нет — это бинарники старого комплекта (BF3LegacyLan/BF3NativeBots).

**Факт (`evbots-dll\src\ModdingInterface.cpp:573-603`, `EngineHooks.cpp:355`).** `EVBots.dll` запускает мод-систему, только
если в процессе есть `bf3_server_Final.exe`; все адреса хуков — для `bf3_server_Final.exe`. В клиенте она бы загрузилась и
ничего не делала (по коду, не прогонялось).

**Факт (`evbots-dll\src\VextNetEvents.cpp:13-16`).** Серверные `NetEvents:Send*/Broadcast*` только пишут в журнал.
**Факт (`VextPlayerManager.cpp:3278-3560`).** Мост на сервере уже подменяет клиентские лучи: перехватывает `CheckBotBotAttack`
и сам отдаёт моду `Botmanager:RaycastResults`. Поэтому поведение ботов сейчас от клиента не зависит; клиентская часть нужна
для меню, редактора путей, `Client:RequestSettings`/`RequestEnterVehicle` и чтобы лучи считал клиент, как у VU.

**Факт (папка `Battlefield 3`).** Рядом с `bf3.exe` лежат `dinput8.dll` (7 КБ, упакован, строка `ZLOFENIX!`, грузит `LoadLibraryA`)
и `Zlo.dll` — прокси эмулятора ZLO. `z.log` («Z1 not patched in activation.dll») обновлён 23.10 04:35:11 — в ту же секунду,
когда стартовал клиент. **Гипотеза (сильная):** прокси ZLO грузится в наш клиент при каждом запуске. Проверить списком модулей
живого клиента. Для нас это и риск (чужой код в процессе), и запасной путь загрузки.

**Чего нет.** Ghidra-проекта клиентского `zbf3.exe` нет (есть `ghidra-proj-server` — сервер, `ghidra-proj` — botlayer,
`re\vu\pyghidra_mcp\vu.gpr` — VU). Установки VU на диске больше нет (`%LOCALAPPDATA%\VeniceUnleashed\client\vu\vubin` пуст),
но строки и gzf `vu-core.dll` сохранены в `re\`.

## 3. Что fun-bots вызывает у клиента (поиск по `fun-bots\ext\Client\*.lua`)

NetEvents: Subscribe 42, SendLocal 44, Send 10. InputManager: WentKeyDown 42, GetLevel 3, WentKeyUp 2. Console:Register 31.
Events:Subscribe 24 (Extension:Loaded/Unloading, Engine:Message, UpdateManager:Update, Engine:Update, Level:Loaded/Destroy,
Player:Respawn/Deleted, Client:UpdateInput, UI:DrawHud, Partition:Loaded). PlayerManager: GetLocalPlayer 9, GetPlayers,
GetPlayersByTeam, GetPlayerById. RaycastManager: Raycast 4, CollisionRaycast 1. ClientUtils: WorldToScreen 4,
GetCameraTransform 3, GetWindowSize 2. DebugRenderer: DrawText2D 4, DrawSphere/DrawOBB/DrawLine. Hooks:Install 2.
WebUI: Init, Show/Hide, EnableMouse/Keyboard, ResetMouse/Keyboard, ExecuteJS 5.
Для ботов важны: `ClientBotManager.lua` — лучи (`Raycast`, `CollisionRaycast`) → `Botmanager:RaycastResults`, ответ на
`CheckBotBotAttack`, `Client:RequestSettings` (на `Engine:Message`), `Client:RequestEnterVehicle`/`RequestChangeVehicleSeat`.

## 4. Вывод и предлагаемый путь (рекомендация, не проверено)

1. **Загрузка в клиент — наш же `activation-loader --client --dll EVBots.dll`**, как на сервере. Один `EVBots.dll` с двумя
   режимами, как у VU один `veniceext.dll`: в `bf3.exe` — клиентский режим (дождаться расшифровки кода, как `activation.dll`
   во втором этапе, затем клиентский `LuaContext`, без серверных хуков). Запасной путь — прокси `dinput8.dll` (но его место
   сейчас занимает ZLO).
2. **Канал клиент↔сервер — свой, как Harmony у VU, а не через игровое соединение движка.** Сервер EVBots слушает свой порт;
   клиент при входе подключается, получает `ext/Client` + `ext/Shared` мода (у друга мода на диске нет) и дальше гоняет
   NetEvents в обе стороны. Это то, что делает VU (факт: свой RakNet-канал и раздача модулей); выбор протокола (TCP/UDP,
   формат) — наш. Порт добавить в «Фаервол (UAC)» лаунчера.
3. **Адреса клиентских функций движка — из таблицы VU** (они уже в адресах `bf3.exe`) плюс Ghidra-проект `zbf3.exe` для сверки
   (правило 18: каждое место — дизассемблером до патча).

## 5. План клиентской части (порядок работ)

- **К1. Ghidra-проект клиентского `zbf3.exe`** (headless, долго; не во время игры). Разметить клиентские слоты таблицы VU:
  по декомпиляту veniceext найти, какие функции `Client::*` читают какие слоты (`RaycastManager`, `ClientUtils`,
  `PlayerManager` клиента, `InputManager`, `UpdateManager`, `Level`, `DebugRenderer`). Итог — таблица «биндинг → слот → адрес bf3.exe».
- **К2. Посмотреть транспорт VU** в `vu.gpr` (vu-core): что и в каком порядке шлёт Harmony (вход, модули, NetEvent),
  как сериализуются аргументы NetEvent. Только чтобы повторить поведение, не протокол байт в байт.
- **К3. Клиентский режим `EVBots.dll`** (минимум): определить `bf3.exe`, дождаться расшифровки, поднять Lua 5.4 клиента,
  загрузить `ext/Client/__init__.lua` из локальной копии мода (на ПК разработки), события `Extension:Loaded`, тик
  (`UpdateManager:Update`/`Engine:Update`), `Level:Loaded/Destroy`. В `join.ps1` — `--dll EVBots.dll`. Первый прогон —
  с пользователем: в логе клиента `LUA-MODE READY (client)`, клиент не падает.
- **К4. Канал и NetEvents**: сервер слушает порт, клиент подключается по `personaref`; `NetEvents:Send/SendLocal` клиента →
  серверные подписки мода; `SendTo*/Broadcast*` сервера → клиенту. Раздача `ext/Client` + `ext/Shared` по каналу.
- **К5. Лучи как у VU**: клиентские `PlayerManager:GetLocalPlayer`, `ClientUtils:GetCameraTransform`, `RaycastManager:Raycast/
  CollisionRaycast`. Подмену лучей в мосте (`VextPlayerManager.cpp:3278-3560`) оставить под флагом и выключать, когда пришли
  настоящие `Botmanager:RaycastResults` от клиента (сравнить поведение на прогоне).
- **К6. `InputManager`, `Console:Register`, `DebugRenderer`, `ClientUtils:WorldToScreen`** — консольные команды и редактор путей.
- **К7. WebUI (cohtml/ui.vuic)** — меню на левом Alt, последним.
- Гостю (другу) клиентский режим приезжает в том же `EVBots.dll`; лаунчер должен передавать `--dll` при «Войти в игру».

## 5б. ResourceManager и EntityManager в клиенте (23.10, расхождение с VU)

- **Факт (VU-Docs):** у VU оба — общие библиотеки (`shared\library`), есть и в клиенте. У нас в клиенте (сборка 215) их нет:
  наши реализации ходят по жёстким адресам `bf3_server_Final.exe` (`EngineResources.cpp`: 13 адресов, сверка прологов).
- **Факт (fun-bots, только чтение):** клиентская часть мода `EntityManager` не зовёт. `ResourceManager` зовут:
  (1) `Shared\__init__.lua` — «обход пропадающего оружия» (`MountSuperBundle`, `SearchForInstanceByGuid`, `AddRegistry`, хук
  `ResourceManager:LoadBundles`) — только при `Registry.COMMON.USE_LOAD_BUNDLE_BUGFIX`, а он `false`;
  (2) `WeaponList:UpdateWeaponList` (`SearchForDataContainer`) — из `ClientBotManager:OnWriteClientSettings`, то есть по
  NetEvent `WriteClientSettings` с сервера: до канала К4 не срабатывает; (3) `EbxEditUtils` — редактор/помощники.
  Итог: сейчас разрыв не виден, с К4 первым упадёт `UpdateWeaponList` («attempt to index a nil value»).
- **Как закрывать (как VU — одна общая реализация):**
  1. ✅ сделано в сборке 215 (md5 33225088…): в клиенте `ResourceManager` и `EntityManager` со всеми методами VU-Docs —
     заглушки документированного типа (`VextClient.cpp`), обработчики загрузки копятся в `ResourceManager._handlers`;
  2. К5: сделать `EngineResources` двухсторонним — таблица адресов на процесс (сервер — нынешняя, клиент — по `zbf3.exe`),
     та же логика C++. Клиентские адреса — из слотов VU, размеченных как ResourceManager (`18E74F40` 004E0490, `F48` 004E6430,
     `F50` 004E45C0, `FB4` 005760F0, `5028` 004C1310, `5220` 0049EB60; данные `505C` 0230D220, `5224` 022ED35C) и
     EntityManager (`F00` 00549A90, `F04` 00549920, `DA4` 00552770), плюс сопоставление по коду с серверными функциями
     (как в `re\vu\SEQUENCE-REPORT.md`); каждое место — дизассемблером по `zbf3.exe`, «всё или ничего» (правило 18).

## 5в. Что клиент fun-bots берёт из API VU (сверка 23.10, сборка 215)

Собрано по коду `ext\Client` + `ext\Shared` мода (только чтение) и VU-Docs. Скрипт —
scratchpad `vu_usage.py`; проверка в тест-хосте — раздел 10 «все глобалы VU … определены».

**Глобалы (31):** библиотеки ClientUtils, Console, DebugRenderer, Events, Hooks, InputManager, MathUtils, NetEvents,
PlayerManager, RaycastManager, ResourceManager, SharedUtils, WebUI; типы Guid, Vec2/3/4; перечисления CharacterPoseType,
InputConceptIdentifiers, InputDeviceKeys, MaterialFlags, MessageType, RayCastFlags, ResourceCompartment, TeamId, UpdatePass;
fb-классы AlternateSpawnEntityData, RegistryContainer, UIScreenAsset. Все есть в клиенте сборки 215: fb-классы — все 2068
из VU-Docs как глобальные типы, как у VU (`VextFbTypes.cpp`, генератор `tools\gen_fb_types.py`).

**События и хуки движка — источник нужен в К5** (у клиента их сейчас никто не шлёт, кроме Extension:*):

| Событие / хук | VU-Docs | Зачем моду (клиент) | Приоритет |
|---|---|---|---|
| UpdateManager:Update (pass `UpdatePass_PreSim`) | shared\event | ClientBotManager: лучи «видит бота» → `Botmanager:RaycastResults`; редактор путей | 1 |
| Engine:Message | shared\event | ClientBotManager: `Client:RequestSettings` при входе | 1 |
| Level:Loaded / Level:Destroy | client\event / shared\event | готовность UI и менеджеров | 1 |
| Player:Respawn / Player:Deleted | client\event | сборка мусора, редактор путей | 2 |
| Client:UpdateInput | client\event | клавиши мода (F-клавиши, меню), посадка `Client:RequestEnterVehicle` | 2 |
| Hook Input:PreUpdate | client\hook | смена места в технике (`Client:RequestChangeVehicleSeat`) | 2 |
| UI:DrawHud | client\event | отрисовка редактора путей / ники (выкл. в Registry) | 3 |
| Hook UI:PushScreen | client\hook | меню (CommoRose) редактора | 3 |
| Partition:Loaded | shared\event | ClientSpawnPointHelper (точки спавна) | 3 |
| Engine:Update | shared\event | в клиенте мода пустой | — |
| Level:LoadResources, Level:RegisterEntityResources, Hook ResourceManager:LoadBundles | shared | обход бандлов, `USE_LOAD_BUNDLE_BUGFIX = false` | — |

Тик клиента — хук кадра движка клиента (как у VU UpdateManager), не таймер; адреса — после Ghidra `zbf3.exe`.

## 6. Открытые вопросы

- ~~Грузит ли `activation-loader --client --dll` DLL~~ — по коду: грузит, но **вместо** `activation.dll` (одна DLL на процесс),
  до `ResumeThread`, то есть до расшифровки кода. Для К3 нужен способ загрузить обе DLL (решение пользователя: свой загрузчик
  EVBots с исходниками, `EVBots.dll` сам подгружает `activation.dll`, или иное).
- Грузится ли ZLO (`dinput8.dll`) в наш клиент и не мешает ли (модули живого клиента).
- Транспорт NetEvent у VU — Harmony или игровое соединение (К2).
- PunkBuster: клиент только LAN/оффлайн (правило 5) — не меняется.
