# Старт новой сессии после 23.10.2026 — клиентская часть EVBots

> Читать первым в новой сессии. Дальше по обычному порядку: `HANDOFF.md` → `STATE.md` → верх `PLAN.md`.
> Цель проекта — свой локальный Venice Unleashed: мод fun-bots (и вообще Lua-моды VU) работает у нас так же, как на VU,
> без правок мода и без интернета. Всё, где мы пока не как VU, — в `ext\VU-DIVERGENCES.md`.

## 0. ★ СЕЙЧАС (23.10 ≈20:50) — читать первым: начат К4 (канал клиент↔сервер)

**На стенде:** `EVBots.dll` **218** (md5 00e823fe…) + **`ev-core.dll` 2** (md5 4940fd1c…, `EVBots-dev\ev-core\`, аналог vu-core).
Прогон 218 (20:38) чистый: окно сервера как у VU (консоль `[info] [EVBots] …`, панель `Master: EV` / `GameState: Online`);
клиент (`join.ps1` даёт `--dll EVBots.dll`) — тик из хука кадра `0x40A470`, события из сообщений движка (Core → менеджер
`[0x022ED3B8]`, остальные → `[0x02380B58]`), `Level:Loaded/Finalized`, `Engine:Message` доходят до fun-bots; **клиент fun-bots сам
шлёт `NetEvents:SendLocal("Client:RequestSettings")` — упирается в отсутствие канала. Поэтому К4.**

**Решения пользователя 23.10 вечером:** две DLL как у VU (`ev-core.dll` = vu-core, `EVBots.dll` = veniceext); протокол
бэкенда **EV** = Blaze + функции Zeus (`backend\EV-PROTOCOL.md`, E1–E2 сделаны в бэкенде, не на стенде); обновление для друга —
**после К4**, установщиком-обновлением: найти его `EVBots`, бэкап, заменить изменённые файлы, добавить недостающие
(`ev-core.dll`, новый лаунчер), **старый активатор (`activation-loader.exe`) заменить нашим `evbots-loader.exe`**, профили не
трогать. Лаунчер (EV Launcher) должен грузить серверу `ev-core.dll` перед `EVBots.dll`, клиенту — `EVBots.dll` (+`ev-core.dll`).

**План К4 (как Harmony у VU — RakNet, порт 7948 «Monitored Harmony server listening on 0.0.0.0:7948» на скринах VU):**
1. Транспорт — в `ev-core.dll` (у VU Harmony живёт в vu-core: RTTI `MonitoredHarmony`): сервер слушает TCP `0.0.0.0:7948`
   (`EVCORE_HARMONY_PORT`), клиент подключается к IP сервера из `-requestStateParams serveraddr=…`; кадр `u32 длина | данные`.
   Экспорт: `EvNetSetHandler(cb)`, `EvNetSend(conn, data, len)`, `EvNetBroadcast(data, len)`, `EvNetClientConnect(host)`.
2. `EVBots.dll`: сериализация аргументов NetEvents на Lua (числа, строки, bool, таблицы, Vec2/3/4, Guid, LinearTransform),
   приём из сети — в очередь, доставка подписчикам на тике движка под `TickGate` (сервер — тик, клиент — хук кадра).
   Клиент шлёт «hello» с именем игрока → сервер сопоставляет соединение с игроком (`Player`) для `NetEvents:SendTo(player…)`
   и для первого аргумента обработчика на сервере.
3. Проверка: тест транспорта (loopback в одном процессе), тест сериализации в тест-хосте; прогон — `Client:RequestSettings`
   доходит до сервера, сервер отвечает `WriteClientSettings`.
4. Позже: раздача `ext\client`+`ext\shared` модов по каналу/EV (сейчас `EVBOTS_CLIENT_MODS`, реестр В10), лучи клиента (Б1).

**Статус К4 (≈21:40):** шаги 1–3 сделаны — `ev-core.dll` 3 (канал, тест 18/18) и `EVBots.dll` 219 (кодек, мост, NetEvents
сервера и клиента, тест-хост 77/77), `join.ps1` готов. **Не развёрнуто и не прогнано** — следующий шаг: прогон стенда по
слову пользователя (`start.cmd` сам поставит обе DLL с бэкапом). Что смотреть — MODLOG «К4».

Подробности сегодняшних сборок — `MODLOG.md` (верх: «Прогон 218», «Прогон 217», «Сборка 217 + ev-core 1», «Сборка 216»),
окно сервера — `ext\SERVER-WINDOW-2026-10-23.md`, разбор vu-core — `CLIENT-SLOTS` ★★.

## 1. Где мы на конец 23.10 (утро/день — история)

- **Сервер** — опорная сборка **214** (md5 cc1f15bb…), на стенде. Прогон 23.10 14:03 на новом загрузчике: TEST 84 PASS / 0 FAIL,
  VEH 0, ERR 0. Сервер будем улучшать позже (решение пользователя 23.10): открытое — `PLAN.md`, блок «04:47», и реестр, раздел Б.
- **Клиент** — сборка **215** (md5 12179fa7…), **собрана, не развёрнута, в игре не запускалась**. В `bf3.exe` тот же `EVBots.dll`:
  ждёт расшифровки кода, поднимает клиентский Lua, грузит `ext\shared`+`ext\client` модов из `EVBOTS_CLIENT_MODS`, шлёт
  `Engine:Init`/`Extension:Loaded`. Клиентские библиотеки VU — заглушки по VU-Docs (`VextClient.cpp`), все 2068 fb-классов —
  глобальные типы (`VextFbTypes.cpp`). Тест-хост 61/61, luachk 43/43. Тика и событий движка клиента нет, канала к серверу нет.
- **Свой загрузчик** `evbots-loader.exe` (`EVBots-dev\loader`, исходники): несколько DLL по порядку, клиенту `activation.dll`
  первой, серверу `EVBots.dll`. Стенд и оба лаунчера на нём. `botlayer.dll` не используется.
- **Комплект** `dist\EVBots-2026-10-23` (DLL 214 + новый лаунчер + загрузчик) и `EVBots-Setup.exe` собраны и проверены
  (`/quiet`, `um publish check`), установка `C:\Games\EVBots` обновлена. **Другу не отдано.**
- **Реверс клиента:** Ghidra-проект `re\ghidra-proj-client` по `zbf3.exe` готов (98 746 функций). Таблица клиентских слотов VU —
  `ext\CLIENT-SLOTS-2026-10-23.md`; скрипты и вывод — `re\client-k1\`. Имён функций пока нет (у Frostbite своя рефлексия, RTTI
  почти пуст) — это шаг 1 ниже.

- **Ghidra MCP (23.10, поздний вечер):** `.mcp.json` переведён на stdio-посредник `tools\ghidra-mcp\proxy.py` (цели server :8765
  и client :8767; botlayer убран 23.10 — не используется). ✅ Проверено в новой сессии 23.10: `mcp__ghidra-client__*` есть,
  `list_project_binaries` → `/zbf3.exe` (анализ готов, индекс строк/кода не построен). Если сервер не запущен, посредник поднимет
  его сам при первом вызове (если bf3 не запущен).
- **К5 шаг 1:** ввод и ProfileOptions — факт; объект «вида» камеры — открыт (`CLIENT-SLOTS` ★, раздел «Открыто»).
- **К5 шаги 2–3 (23.10 ≈19:45, 215 md5 36e86874…, не прогнано):** тик клиента — хук кадра `0x0040A470` как у VU; события —
  слушатель сообщений движка `004A5D90` (Engine:Message, Level:Loaded/Finalized). Разбор vu-core — `CLIENT-SLOTS` ★★.
- **Протокол EV** (Zeus + Blaze → EV, решение пользователя): `backend\EV-PROTOCOL.md`. Имя выбрано — **EV**; совместимость с
  `vu.exe` по проводу не делаем (§5 ниже — закрыто).

## 2. Главные документы клиента

| Файл | Что |
|---|---|
| `ext\CLIENT-RECON-2026-10-23.md` | как VU устроен на клиенте (один veniceext, канал Harmony), план К1–К7, §5б ResourceManager/EntityManager, §5в что клиент fun-bots берёт из API VU + таблица событий/хуков с приоритетами |
| `ext\CLIENT-SLOTS-2026-10-23.md` | слоты VU → адреса клиента (ClientUtils, RaycastManager, InputManager, DebugRenderer, классы), результат Ghidra |
| `ext\VU-DIVERGENCES.md` | реестр расхождений с VU (рантайм, сервер, клиент, сеть) — вести обязательно |
| `loader\README.md` | загрузчик: отличия от старого, сборка, тест |
| `MODLOG.md` (верх) | подробности всех правок 23.10 |

## 3. Правила, появившиеся 23.10 (кроме обычных из CLAUDE.md)

- **Статика клиента — только `Battlefield 3\zbf3.exe`.** Это расшифрованный `bf3.exe` (тот же timestamp, те же адреса секций);
  `bf3.exe` на диске зашифрован DRM. Адреса из `zbf3.exe` = адреса в запущенной игре. Декомпилировать `bf3.exe` не нужно.
- **Долгую фоновую задачу (Ghidra) не останавливать без вопроса** — сказать, что потеряется (анализ сохраняется только в конце).
- **Каждое «не как VU» — строкой в `VU-DIVERGENCES.md`** в той же правке.
- Проверка Lua-чанков: `python -I EVBots-dev\evbots-dll\tools\luachk.py src\*.cpp` (lua54 — `C:\Games\tools\lua54\`).
- Уборка: функцию удаления в PowerShell не называть `Del` (это псевдоним `Remove-Item`); сначала честный `-DryRun`.
  `BF-Portable\Xampp` и `htdocs` не трогать (решение пользователя).
- Сессия Claude Code сейчас без прав администратора: загрузчик и тесты запускаются напрямую; стенд — как раньше,
  `explorer.exe ...\run\start.cmd`, только по слову пользователя.

## 4. Что делать дальше — К5 по шагам (решить порядок с пользователем)

1. **Имена клиентских функций.** Сопоставить функции из `CLIENT-SLOTS` с серверным проектом `re\ghidra-proj-server`
   (общий код движка: лучи `01199D10`, ресурсы, сущности) — как находили адреса сервера (`re\vu\SEQUENCE-REPORT.md`);
   рефлексия Frostbite (`typeinfo`: имя класса → vtable → методы). Проверять дизассемблером по `zbf3.exe` (правило 18).
2. **Тик клиента** — источник `UpdateManager:Update` с `UpdatePass_PreSim` и `Engine:Update`: как VU получает кадр клиента
   (декомпилят veniceext: кто рассылает UpdateManager:Update) → хук кадра движка клиента. **Не таймер** (реестр А1, В1).
3. **События клиента** первого приоритета: `Engine:Message` (ClientBotManager шлёт `Client:RequestSettings`),
   `Level:Loaded` / `Level:Destroy`. Затем `Player:Respawn/Deleted`, `Client:UpdateInput`, хук `Input:PreUpdate`.
4. **`PlayerManager` клиента** (`GetLocalPlayer`, `GetPlayers`) и **лучи** (`SpatialRaycast` `0083B9B0`, Collision/Detailed
   `01199D10`; `ClientUtils:GetCameraTransform` = `006C3A90` на «виде» + трансформ `вид+0xDE0`; сам «вид» VU получает от vu-core,
   его объект в движке ещё найти — `010181B0` оказался UI и снят; найден `GameView` = `[[0x022E3790]+0xB4]+0x24`, но он мал
   (0x90) — смотреть его поля; `CLIENT-SLOTS` ★ «Открыто») — чтобы клиент сам считал
   `Botmanager:RaycastResults` (реестр Б1). **Шаг 1 сделан 23.10 вечером** — `CLIENT-SLOTS` раздел ★ (`00FF8D50`/`00FF6120` — PopScreen, не камера).
5. **`EngineResources` двухсторонний** — клиентские `ResourceManager`/`EntityManager` настоящие (`CLIENT-RECON` §5б).
6. **Первый прогон клиента с пользователем:** в `run\join.ps1` клиенту `--dll EVBots.dll` и `EVBOTS_CLIENT_MODS=<Admin\Mods>`
   (после очистки `EVBOTS_*`), смотреть `logs\EVBots-client.log`: `LUA-MODE READY (client)`, игра не падает.
7. **К4 — канал клиент↔сервер** (как Harmony у VU, UDP; раздача `ext\client`+`ext\shared`, NetEvents в обе стороны) — до или
   после К5, решить с пользователем: без К4 клиентские лучи не дойдут до сервера.

## 4б. Свой аналог Zeus (решение пользователя 23.10)

- **Что это.** Zeus — облачный бэкенд VU: аккаунты/солдаты, список серверов (браузер), ключ сервера (`server.key`), допуск игрока
  на сервер VU, обновления, связь сервера с облаком. **Blaze** — другое: протокол EA, которым говорит сама BF3; его у нас делает
  EV Backend (`EVBots-dev\backend`). Делаем **свой локальный аналог Zeus под своим именем** (не «Zeus» и не «Blaze»; имя выбирает
  пользователь) — чтобы всё работало как у VU, но в LAN без интернета.
- **Что уже есть.** EV Backend частично закрывает роль Zeus: аккаунты, `/servers`, вход по personaref, прогресс
  (`stand\VU-HOWITWORKS.md` §3.1: «роль Zeus ~75%»). Не хватает: идентичность сервера, допуск/отказ игрока как у VU
  («Player disallowed … by an extension»), раздача модов клиентам и канал клиент↔сервер (VU — «Harmony», это К4).
- **Материалы (только чтение, проект EV пользователя, не править):** `%LOCALAPPDATA%\VeniceUnleashed\client\EV\docs\`
  `ZEUS-PROTOCOL.md` (реверс `vu-core.dll`: транспорт, кадр, LZ4, крипто DH2+SHA3+AES-128-GCM, Login/TokenLogin/CreateAccount),
  `ZEUS-MESSAGES.md` (модули Account, Update, Server, Authentication, Player, Utility, Manager — ID сообщений),
  `VU-INTERNALS.md`, `LOCAL-SETUP.md`; эмулятор `EV\src\ev_backend\` (Python). Пользователь: EV — «другое пытался, косячно» —
  **брать только нужное и сверять**, а не переносить целиком.
- **Рекомендация (не решено):** повторять **функции** Zeus (что делает каждый модуль), а не байтовый протокол: наш клиент — BF3
  с `EVBots.dll`, а не `vu.exe`, ему совместимость по проводу не нужна. Если пользователь захочет пускать настоящий `vu.exe` —
  тогда протокол из `ZEUS-PROTOCOL.md`.
- **Первые шаги:** сводная таблица «модуль Zeus → что делает → есть ли у EV Backend → нужно ли нам» по `ZEUS-MESSAGES.md`;
  выбрать с пользователем имя сервиса; связать с К4 (канал и раздача модов могут жить в этом же сервисе).

## 5. Ждут решения пользователя

- Имя своего аналога Zeus; нужна ли совместимость по протоколу с настоящим `vu.exe` (см. §4б).

- Порядок К4 и К5 (см. шаг 7).
- Подключить клиентский Ghidra-проект к Ghidra MCP: цель `client` в `tools\ghidra-mcp\start.ps1` + запись в `C:\Games\.mcp.json`
  (правка конфигурации — спросить).
- PR заметки universal-modder `knowledge\games\battlefield-3\bf3-client-decrypted-dump-vu-slot-table-and-loader.md` — только с согласия.
- Отдать другу комплект `dist\EVBots-2026-10-23` — только по слову.
- Выключить задачу VS `BackgroundDownload` (мусор в `C:\Temp`) — делает пользователь сам (память `reference-vs-background-download`).
