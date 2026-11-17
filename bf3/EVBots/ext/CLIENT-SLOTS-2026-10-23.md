# Клиентские слоты таблицы движка VU → адреса нашего `bf3.exe` (К1, 23.10.2026, только чтение)

Источник слотов: `re\temp-archive\19a41ba6-bae4-453f-abd4-727985919b45\table.txt` (450 слотов `0x18E74C00..0x18E754A4`,
значение = адрес в `bf3.exe`). Разметка — по декомпиляту `re\vu\decomp\chunk*.decomp.txt` и байтам `re\vu\chunks\*.dll`.
Скрипты (`re\client-k1\`, повторяемые): `slots_index.py` (индекс функций и слотов → idx.pkl, ~5 мин), `reg_disasm.py`
(регистрации usertype: имена методов и обёртки по байтам → `regdis.txt`), `tu2.py` (слот → класс по соседству → `tu2.tsv`),
`check_zbf3.py` (сверка по `zbf3.exe`), `vu_usage.py` (что из API VU берёт fun-bots).

## Как размечено и насколько надёжно

- **Факт.** Из 450 слотов функции VU (кроме init) читают 148; 3 из них — общие служебные (`18E74CF0`, `18E74D20`, `18E74D28`,
  тысячи пользователей). Остальной API VU работает через рефлексию движка (`TypeInformation`) и глобальные указатели, а не через
  прямые функции из таблицы.
- **Факт.** Каждая функция регистрации библиотеки VU кладёт в стек массив имён методов (`mov [ebp-X], "Имя"`) и указатели на
  обёртки (`mov [ebp-Y], fn`), затем `push "__Имя"` и `lua_setglobal(..., "Имя")`. Найдено 32 таких регистрации (`reg_disasm.py`).
- **Факт.** Обёртки зовут реализацию метода через указатель (sol3), прямых вызовов нет. Реализации лежат в той же единице
  компиляции, рядом с регистрацией (пример: клиентский `RaycastManager::CollisionRaycast` — `0x10B49380`, регистрация —
  `0x10B4A8C0`; строки `RaycastManager::...` в теле). По этому соседству размечено 92 слота из 145 (`tu2.py`).
  **Это эвристика:** библиотека слота почти надёжна, конкретный метод — гипотеза, пока не сверено по `zbf3.exe` в Ghidra.
- **Факт (`check_zbf3.py`).** Все 32 кодовых адреса клиентских слотов из таблицы ниже — начала функций в `zbf3.exe` (перед входом
  `CC`/`ret`, нормальный пролог); адреса-данные лежат в `.data`. `zbf3.exe` timestamp `0x511C9356`, база `0x400000`.

## ★ К5 шаг 1 (23.10, вечер): уточнение по коду обёрток VU — читать вместо столбца «гипотеза» ниже

Метод: для каждого слота найдены все функции `veniceext.dll`, которые его читают, и строки в них (`re\vu\slotusers.py` →
`re\vu\slotusers-client.txt`); обёртки ClientUtils/InputManager разобраны дизассемблером (`re\vu\dis.py`), функции движка — в
Ghidra клиента (MCP `ghidra-client`, :8767). **Факт** = строка или вызов в коде VU / дизассемблер zbf3; иначе — гипотеза.

| Слот → bf3.exe | Было (гипотеза) | Стало | Чем подтверждено |
|---|---|---|---|
| `18E74DD4` → `00FF8D50`, `18E74F5C` → `00FF6120` | GetCameraTransform | **`ClientUtils:PopScreen(UIGraphAsset)`**: `00FF6120` = геттер UI-глобала `[0x2385CE0]`, `00FF8D50` (thiscall, `ecx=[this+0x28]`) — снять экран, вернуть bool. `00FF8D50` — idx 47 в vtable `020ACC8C` (и idx 38 в `02143A68`) | факт: обёртка VU `10ADB790` проверяет тип `UIGraphAsset`, затем `[18E74F5C]()` и `[18E74DD4](arg, asset)` |
| `18E74DD8` → `00884E10`, `18E74DDC` → `0236813C` | WorldToScreen/окно | **`ClientUtils:SetSquadAutoJoining(bool)`**: если `*[0x236813C]` — `00884E10("loadPlayer_joinSquad", bool)` (запись значения UI-данных) | факт: обёртка `10ADBE20`, строка `loadPlayer_joinSquad` (`0x1781D250`) |
| `18E7501C` → `006C3A90` | проекция/матрица | **пересчёт матриц вида** (fastcall, `this` = вид+0x50): из трансформа камеры `this+0` → view `+0x200`, проекция/view·proj `+0x280…+0x47F`. VU зовёт её перед `GetCameraTransform` (`10ADB4B0`), `ScreenToWorld` (`10ADBBE0`), `WorldToScreen` (`10ADC020`) | факт: декомпиляция `006C3A90` (Ghidra клиента); VU читает view·proj по `вид+0x410…0x44C` (= `this+0x3C0`), трансформ камеры — `вид+0xDE0…0xE1F` |
| (нет слота) | — | **Откуда VU берёт «вид» и окно:** не из таблицы движка. vu-core кладёт указатели в объект VU (`[0x18C992C4]`, 0x160 б) через vtable `0x177C0410`: idx `0x88` → поле `+0xCC` (вид), idx `0x8C` → `+0xD0` (окно: HWND `+0x10`, ширина/высота `+0x20/+0x24`). Нам искать вид в движке самим (К5 шаг 4) | факт: сеттеры `10731A40`/`10731AD0`, их позиции в vtable; кто в vu-core их зовёт — не смотрел |
| `18E752CC` → `0083B9B0` | Raycast | **`RaycastManager:SpatialRaycast`** (клиент) | факт: единственный пользователь — `10B48770` со строкой `RaycastManager::SpatialRaycast` |
| `18E74F20` → `01199D10` | общий луч | **`CollisionRaycast` и `DetailedRaycast`** (клиент) | факт: `10B49380`/`10B496B0` со строками `RaycastManager::CollisionRaycast/DetailedRaycast` |
| `18E752C0/C4/C8`, `18E75304/08` | обвязка лучей | **общая обвязка приведения типов/TypeInfo** — читают ~25 функций VU (Player, SoldierEntity, EntityManager, лучи) со строками `GetClassTypeInfo`, `unsupported cast` | факт по списку пользователей; что именно делает каждая — гипотеза |
| `18E74F38` → `00FBFFC0` | менеджер ввода | **менеджер «понятий ввода»** (`InputConceptIdentifiers`, не клавиши!) `[0x2384EB8]`, vtable `02144658`, ctor `0103D0B0` (0x30 б). Тот же объект — `[[Main 0x022E3790]+0xB4]+0x30`. Методы: **GetLevel** = vfunc `+0x14(concept,0)`, **IsDown** = `+0x20(concept,0)` (`10B01DE0`), **WentDown** = `+0x28(concept)` (`10B021E0`), **WentUp** = `+0x30(concept)` (`10B02280`, через Main) | факт (вечер 23.10, 2-й заход): `ConceptUndefined = 124 = 0x7C` по VU-Docs — граница в коде; имена ↔ обёртки из массива регистрации `10B01E90`; все 4 vfunc по zbf3 — одна семья (таблица `[this+0x28]` по 16 б, отказ при 0x7C) |
| Клавиши и мышь (`IsKeyDown`, `WentKeyDown/Up`, мышь, курсор) | — | не из таблицы движка: объекты от vu-core — клавиатура объект VU `+0xB8` (vfunc `+0x10` IsKeyDown, `+0x14` WentKeyDown, `+0x18` WentKeyUp), мышь `+0xB4`. **Свой источник клавиатуры в движке ещё искать** | факт: `10B01E10`, `10B02200`, `10B02220` |
| `18E75268` → `02384D74` | состояние клавиш | **настройки профиля (GameSettings/ProfileOptions)**: его читают ~120 функций VU с именами настроек (`AimAssist`, `Brightness`, `FieldOfView`, `MouseSensitivity`…) | факт по строкам |
| `18E75278` → `00FB8B40` | опрос клавиши | **чтение float-настройки по имени** (`(&opts+0x90, "MouseSensitivity", 0.5)`) | факт: обёртка `10B01D30` |
| `18E75270` / `18E75288` / `18E75284` → `00FBB4D0` / `00FAFE50` / `00FBEED0` | — | **`ProfileOptions`**: запись float по имени / разослать `ProfileOptionsApplyMessage(раздел)` (switch 0–5, 8) / сохранить профиль (сериализация в буфер 0xFA000, `"AllocatingBuffer"`). Так VU делает `SetMouseSensitivity`: get → сравнить → set → apply(2) → save | факт: обёртка `10B02160`; хэши сообщения `0x0F72B7A1`=`ProfileOptionsApplyMessage`, `0x43FBA85E`=`ProfileOptions` (djb2-xor по строкам zbf3) |
| Мышь (`InputManager` мышь/курсор) | — | не из таблицы: через объект VU `+0xB4`/`+0xB8` (от vu-core) | факт: `10B01D00`, `10B02200` |
| `18E74F54` / `18E74F58` → `004EC2C0` / `004EF080` | Client.Player | **DatabasePartition: добавить / убрать экземпляр** | факт: строки `Cannot add/remove … instance … partition` |
| `18E74E30` / `18E74E34` | LifeCounterEntity | геттеры полей сущности-счётчика (как было) | пользователь `107A7DA0` |
| `18E74E4C` → `0046DE50` | EffectParams | используется в логгере ошибок VU (`10787920`, `GetConsoleLogger`) — назначение не ясно | — |
| `18E74D50/D54/DA0/DF0/E18` | камера/наблюдение | внутренний код VU камеры наблюдения (`107248D0`: `ClientPlayerCameraEntity`, `PlayerSpectateCamera`), не Lua-API | факт по строкам |

**Переименовано в Ghidra клиента** (сохранено): `VuSlot_Client_SpatialRaycast` (`0083B9B0`), `VuSlot_PhysicsRaycast_CollisionDetailed`
(`01199D10`), `VuSlot_PopScreen_GetUiManager` (`00FF6120`), `VuSlot_SetSquadAutoJoining_SetUiValue` (`00884E10`),
`VuSlot_RenderView_UpdateMatrices` (`006C3A90`), `VuSlot_GetKeyboardDevice` (`00FBFFC0`), `VuSlot_ProfileOptions_GetFloat`
(`00FB8B40`), `VuSlot_DebugRenderer_Get` (`004B2EA0`). `00FF8D50` в проекте не функция (создавалась временно) — не переименована.

**2-й заход (23.10, поздний вечер) — переименовано ещё:** `VuSlot_GetKeyboardDevice` → `VuSlot_GetInputConceptManager` (`00FBFFC0`,
прежнее имя неверное), `InputConcepts_ctor` (`0103D0B0`), `VuSlot_ProfileOptions_SetFloat` (`00FBB4D0`),
`VuSlot_ProfileOptions_PostApplyMessage` (`00FAFE50`), `VuSlot_ProfileOptions_Save` (`00FBEED0`), `GameView_ctor` (`00FFDDA0`).
vfunc `01033A30/A80/AD0/B10` (GetLevel/IsDown/WentDown/WentUp) в Ghidra не функции — имена только здесь.

**Открыто — объект «вида» камеры (К5 шаг 4).** Что известно (факт): `Main` = `[0x022E3790]` (пишет `Main::Main` `00415E10`);
`Main+0xB4` = объект клиента (ctor `00F71F60`, 0x20C б); в нём `+0x24` = **`GameView`** (vtable `021433D0`, ctor `00FFDDA0`, 0x90 б;
также `[клиент+0xC]+0x38`); слот VU `00FF6370` = vfunc 15 `GameView`. «Вид» VU (≥0xE20 б, камера `+0xDE0`, структура матриц `+0x50`
с флагом «грязно» `+0x80` бит 0, пересчёт `006C3A90`) — **не найден**: `GameView` мал (0x90), `010181B0`/`01018E70` — UI
(`ClientUIGraphEntity`), не камера (прежний кандидат снят). Дальше: что `GameView` хранит в полях (указатель на вид/рендер),
и кто в vu-core зовёт сеттер idx 0x88 (`10731A40`).

**Рефлексия Frostbite — работает (факт):** `ClassInfo` заполняется статическим конструктором `push infoData; mov ecx, ClassInfo;
call`, `infoData+0` → имя; `getType` = `mov eax, ClassInfo; ret`. Найдено 4129 имён классов, 498 vtable с getType
(`re\client-k1\fbtypes.py` → `fbtypes-client.tsv`; функция → vtable/индекс/класс — `fbvt.py`). Почти все функции слотов —
невиртуальные, поэтому имена им даёт не рефлексия, а код VU (таблица выше). Рефлексия пригодится для К5 шагов 2–4
(`ClientGameEntity`, `ClientPlayerCameraEntity` `02149618`, `ClientPlayerEntryComponent` `02142C28` и т. п.).

## ★★ К5 шаг 2 (23.10, ≈19:30): откуда VU берёт тик и события клиента — разбор vu-core.dll

Метод: Ghidra-проект `re\vu\pyghidra_mcp\vu.gpr` (`vu-core.dll`), скрипты pyghidra (не MCP) — вывод в `re\client-k1\`.
**Факт** = декомпиляция/дизассемблер; гипотеза помечена.

- **Хэши событий VU** — FNV-1a 32 от имени (`Player:Created` = `0xD6F2F1CA`, `Engine:Update` = `0x9D195C92`, `Level:Loaded` =
  `0x98292C20`, `Player:Respawn` = `0xC8BF3EE6`, `Player:Deleted` = `0xCC189FC7`, `Engine:Message` = `0x0196F99A`). Хэши
  **сообщений движка** — djb2-xor (как в `SEQUENCE-REPORT`). Хэша `UpdateManager:Update` (`0x951031C2`) в коде veniceext нет.
- **Хуки движка у VU ставит vu-core**, не veniceext: установщик `10186EF0`, ~110 мест (`FUN_10246AD0(цель, обход, &оригинал)`).
  Полный список целей/обходов — `re\client-k1\vucore-hooks.txt`. Таблица **клиентская** (факт: на сервере `0x0040A470` —
  середина функции). Многие хуки — правки игры VU (макс. игроков 0x7F, настройки), не Lua-API.
- **Тик клиента — факт:** хук `0x0040A470` = один кадр игрового цикла (`do {} while (frame(loop))` в `004171F0`/`00417270`,
  «GameLoop»). Обход VU `10196DF0`: оригинал → если 0, выход; иначе `VU-объект->vfunc+0x10(dt, simDt)` = **`Engine:Update`**
  (`10732220` в veniceext, рассылает `0x9D195C92`). `dt = (float)[[loop+4]+0xBC]+0x58`, `simDt = +0x50`: `loop+4` = `Main`
  (`00415E10`), `Main+0xBC` = таймер кадра (ctor `01195870(.., 30, 1)`). Пролог `56 8BF1 8B4E04 8B01` (8 б). **Сделано в 215.**
- **`UpdateManager:Update`** — источник фаз у VU **не найден**: не vu-core (единственный вызов методов VU-объекта с float — `+0x10`
  = Engine:Update) и не по хэшу в veniceext. У нас пока из того же хука кадра: PreSim, PostSim, PostFrame (реестр В1а ⚠).
- **События клиента — факт:** veniceext `MessageManager` (`10C14D60`) подписывается слотом **`18E74F80` = `004A5D90`**
  (подписка на категорию сообщений движка; `Main::Main` зовёт её же с `0x7C813E9E` = `Core`) на **55 категорий**: `Core`,
  `Client`, `ClientLevel`, `ClientPlayer`, `ClientSoldier`, `ClientState`, `ClientWeapon`, `ClientVehicle`, `Network`,
  `Server*`, `UI`, `Session`… — список `re\client-k1\vu-message-categories.txt`. Отсюда `Level:Loaded`, `Player:*`,
  `Engine:Message`. **Прежняя разметка «`18E74F80` — Client.RagdollComponent» (таблица ниже) неверна.** Это и путь для А3
  сервера (Level:Loaded по сообщению движка, а не по RCON). Дальше (шаг 3): интерфейс слушателя (vtable
  `VeniceEXT::Managers::Shared::MessageManager`, метод приёма сообщения) и тип сообщения → имя (`typeinfo`).

## ★★★ К5 шаг 4 (23.10 ≈23:55): PlayerManager и Player клиента у VU — факт по дизассемблеру veniceext

- **Менеджер игроков клиента** (`0x170D5180`): `[[[0x022E3790]]+0xB4]+0x1C` — `Main` → объект клиента (`+0xB4`) → `+0x1C`.
  Поля: `+0x08` — число слотов (граница id), `+0xC0` — массив игроков по id, `+0x9C…+0xA0` — вектор игроков,
  `+0xAC…+0xB0` — второй вектор (наблюдатели; GetPlayerByOnlineId ищет в обоих), **`+0xBC` — локальный игрок**.
- **GetLocalPlayer** (`10B1B810`): `p = [pm+0xBC]`; годен, если `[p+0x3EC] < [pm+8]` и `[pm+0xC0][id] == p`. Lua-объект VU —
  ручка `{тип 0x177C3E74, id}`, игрок ищется заново при каждом доступе (`107A0270`: `[pm+0xC0][id]`, иначе поиск по вектору).
- **Client::Player** (регистрация в chunk00 ≈`107C…`, геттеры): `name` — `char*` `[+0x10]`; `id` — `+0x3EC`; `onlineId` — u64
  `+0x20`; `teamId` — `+0x31C`; `squadId` — `+0x46C`; `isSquadLeader` — байт `+0x470`; `inVehicle` — vfunc `+0x0C` игрока;
  `soldier` — `[[+0x3C0]] − 4` (слабая ссылка); `corpse` — `[[+0x3C4]] − 4`; `alive` — `o = [[+0x3C8]] − 4`, затем vfunc `+0x0C`
  у `o+0xB4`. `attachedControllable`/`controlledControllable` — `107CCA70`/`107CCAC0` (ветку «найден» ещё разобрать).
- **Лучи клиента (факт, декомпилят `10B49380`):** `CollisionRaycast` у VU = `[18E74F20] = 01199D10("RaycastManager::CollisionRaycast",
  1, &from, &to, буфер, maxHits, materialFlags, rayFlags, 0)` → число попаданий; буфер — `maxHits` записей по 0x40 байт
  (выделение `maxHits*0x40`, выравнивание 0x10), каждая копируется в Lua `RayCastHit` (`10B452F0`). Перед вызовом VU проверяет
  `[[X+0x10]+0xC0] != 0`, `X = 170D5160() = [[[0x022E3790]+0xB4]+0xC]`; это и есть `this` луча (`10B494BA`) — тот же объект,
  что в глобале `[0x023813A0]` (факт 25.10: единственный конструктор `00F93C10` пишет себя туда, владелец — в `+0xC0`, MODLOG 226). Соглашение вызова `01199D10`, раскладка 0x40 и
  `rigidBody:Is(...)` (тип сущности по рефлексии) — разобрать в Ghidra клиента (после игры пользователя).
- Дальше: лучи (`01199D10`, `RayCastHit.rigidBody:Is(...)`) —
  чтобы клиент fun-bots считал `Botmanager:RaycastResults` сам и сервер выключил эмуляцию (Б1).

## Клиентские слоты (для К3–К6) — первичная разметка 23.10 днём (столбец «гипотеза» частично неверен, см. ★ выше)

| Слот | bf3.exe | Библиотека/класс VU (по соседству) | Что внутри (capstone по zbf3) | Метод — гипотеза |
|---|---|---|---|---|
| `18E74DD4` | `00FF8D50` | ClientUtils (обёртка `10ADB790`) | `mov ecx,[ecx+0x28]` … | GetCameraTransform: трансформ камеры |
| `18E74F5C` | `00FF6120` | ClientUtils (`10ADB790`) | `mov eax,[0x2385CE0]; ret` — геттер глобала | GetCameraTransform: менеджер камер |
| `18E74DD8` | `00884E10` | ClientUtils (`10ADBBE0`, `10ADBE20`) | вход, `call 0x883FD0` | WorldToScreen/ScreenToWorld или GetWindowSize |
| `18E74DDC` | `0236813C` (данные) | ClientUtils (`10ADBBE0`, `10ADBE20`) | `.data` | глобал рендера/окна |
| `18E7501C` | `006C3A90` | ClientUtils (`10ADC020`, `10ADBBE0`, `10ADB4B0`) | `push ebp; mov ebp,esp; and esp,-16` (SSE) | проекция/матрица |
| `18E752CC` | `0083B9B0` | Client.RaycastManager | SSE-пролог | Raycast / клиентский луч |
| `18E74F20` | `01199D10` | RaycastManager клиента и сервера | SSE-пролог; строки `RaycastManager::CollisionRaycast/DetailedRaycast` | общий физический луч |
| `18E752C0/C4/C8`, `18E75304/08` | `0058C740`, `00591A40`, `023A7A14` (данные), `0118D040`, `0118D020` | RaycastManager клиента (и ProfileOptions, TypeInformation) | — | общая обвязка (не только лучи) |
| `18E74F38` | `00FBFFC0` | InputManager (обёртки `10B01DE0`, `10B021E0`, `10B02140`, `10B02160`) | `mov eax,[0x2384EB8]; ret` — геттер глобала | менеджер ввода |
| `18E75268` | `02384D74` (данные) | InputManager | `.data` | состояние клавиш/мыши |
| `18E75270` | `00FBB4D0` | InputManager | `sub esp,0x20` … | — |
| `18E75278` | `00FB8B40` | InputManager (WentKeyDown/IsKeyDown — обёртки `10B01D00`, `10B01D30`, `10B01CC0`) | `sub esp,8` … | опрос клавиши |
| `18E75284` | `00FBEED0` | InputManager | `sub esp,0x40c; mov esi,ecx` | — |
| `18E75288` | `00FAFE50` | InputManager | `sub esp,0x30` … | — |
| `18E74F7C` | `022E3790` (данные) | почти все библиотеки (d1) | `.data` | главный глобал (контекст игры) |
| `18E74E90` | `004B2EA0` | DebugRenderer (все 6 обёрток) | `mov ecx,[0x22FB188]; test ecx,ecx` — геттер | экземпляр отладочного рендера |
| `18E74E94` | `004B7610` | DebugRenderer (`10DB0BC0`) | — | одна из Draw* |
| `18E74E98` | `004B77E0` | DebugRenderer (`10DB0B70`) | `push 0x40; push 4; call 0x4B7540` | одна из Draw* |
| `18E74EA0` | `004B9980` | DebugRenderer (`10DB03F0`) | `push 2; push 3; call 0x4B9850` | одна из Draw* |
| `18E74EA4` | `004B9850` | DebugRenderer (`10DB0C20`, `10DB0480`, `10DB0540`) | — | DrawText/2D |
| `18E74D80` | `011E8EF0` | Client.Player | SSE-пролог | — |
| `18E74F54` / `18E74F58` | `004EC2C0` / `004EF080` | Client.Player (+ DatabasePartition) | — | — |
| `18E74DA4` | `00552770` | Client.EntityManager | `sub esp,8; mov esi,ecx` | — |
| `18E74E30` / `18E74E34` | `007C2AC0` / `007C2AD0` | Client.LifeCounterEntity | `mov eax,[ecx+0xB4]` / `[ecx+0xB0]` — геттеры полей | — |
| `18E74F80` | `004A5D90` | Client.RagdollComponent | `mov ecx,[ecx]; jmp 0x4A58F0` | — |
| `18E75108` / `18E7510C` | `00FCE780` / `00FD2B60` | ClientPlayerEvent / ClientDoublePlayerEvent | — | отправка клиентских событий сущностям |
| `18E74E4C` | `0046DE50` | Client.EffectParams | — | — |
| `18E74D50/D54/DA0/DF0/E18`, `18E74EFC/F00` | `00FF6350`, `00FF6370`, `010AB520`, `011A4560`, `0109B060`, `00549A40`, `00549A90` | не размечено; функция-пользователь `107248D0` со строками `ClientPlayerCameraEntity`, `PlayerSpectateCamera` | все — входы функций | камера игрока / наблюдение |

**Что не нашлось в слотах (факт):** клиентские `PlayerManager` (`GetLocalPlayer`…), `NetEvents`, `Events`, `WebUI`, `Console`,
`Hooks` в своих обёртках не читают ни одного функционального слота (только глобал `18E74F7C`). Значит, у VU они работают через
структуры движка, хуки vu-core или свой код, а не через функцию движка из таблицы. Для них адреса придётся искать в Ghidra.

## Порядок имён и обёрток (для разметки методов)

Имена методов из байтов регистраций (полный список — `regdis.txt` в scratchpad; сюда — клиентские):
- `__ClientUtils` `10ADB940`: SetSquadAutoJoining, SpectateServer, ConnectToServer, PopScreen, GetWindowSize, ScreenToWorld,
  WorldToScreen, GetCameraTransform; обёртки `10ADBF70, 10ADBE50, 10ADC020, 10ADBBE0, 10ADB660, 10ADB790, 10ADBE20, 10ADB400, 10ADB2E0`.
- `__RaycastManager` (клиент) `10B4A8C0`: CollisionRaycast, DetailedRaycast, SpatialRaycast, Raycast, GetEntityInRaycast.
- `__RaycastManager` (сервер) `10E85E50`: CollisionLatencyRaycast, DetailedLatencyRaycast, CollisionRaycast, DetailedRaycast.
- `__InputManager` `10B01E90`: … WentKeyUp, WentKeyDown, IsKeyDown …, мышь, курсор, чувствительность (16 обёрток).
- `__PlayerManager` (клиент) `10B1CCA0`: GetSquadLeader, GetSpectatorCount, GetLocalPlayer, GetPlayerCount, GetPlayersByName,
  GetPlayerByName, GetPlayerByOnlineId, GetPlayerById, GetPlayersBySquad, GetPlayersByTeam, GetPlayers.
- `__DebugRenderer` `10DB1440`: DrawOBB, DrawVertices, DrawText2D, DrawLine2D, DrawSphere, DrawTriangle, DrawLine (7 обёрток).
- `__WebUI` `10B6CFC0`, `__NetEvents` (клиент) `10B0C470`, `__Console` `10AE4870`, `__Events` `10BBEC00`, `__SharedUtils` `10C0ACA0`.
**Не проверено:** соответствие «имя ↔ обёртка» по позиции (порядок в кортеже sol у MSVC может быть обратным). Метод конкретной
обёртки решать по её коду (аргументы, возвращаемый тип), а не по порядку.

## Ghidra-проект клиента (готов 23.10 ≈16:00) и что он дал

- **Проект:** `re\ghidra-proj-client\bf3client.gpr` ← `zbf3.exe`, анализ 5465 с, «Analysis/Save succeeded», 98 746 функций, 344 МБ.
- **Факт:** все 41 адрес таблицы — функции/данные в ожидаемых блоках (.text/.data). Почти все функции Ghidra уже создала сама
  (временно созданы только `00FF8D50`, `004B77E0`, `00FF6350`), но **ссылок на них нет**: их вызывают через указатели (vtable,
  таблицы VU), поэтому callers пусты. Указатели найдены поиском байтов: напр. `00FF8D50` — в `020ACD48` и `02143B00`
  (.rdata, похоже на vtable), `00FF6370` — в `0214340C`.
- **Факт:** меток `vftable` по RTTI в программе всего **29** — у Frostbite не MSVC RTTI, а своя рефлексия (секции `typeinfo`,
  `fieldinf`). Поэтому имён классов Ghidra почти не дала; имена функций — `FUN_xxxxxxxx`.
- Размеры и вызываемые (для сверки с сервером): ClientUtils `00FF8D50` 23 б → `0101BAE0`; `00FF6120` 6 б (геттер глобала);
  `00884E10` 19 б → `00883FD0`; `006C3A90` 1514 б. Raycast: `0083B9B0` 65 б → `01779630`, `0083B5D0`; `01199D10` 917 б (общий
  клиент/сервер). InputManager: `00FBFFC0` 6 б (геттер), `00FBB4D0` 146 б, `00FB8B40` 117 б, `00FBEED0` 123 б, `00FAFE50` 87 б.
  DebugRenderer: `004B2EA0` 18 б (геттер экземпляра), `004B7610`/`004B77E0`/`004B9980`/`004B9850` → общий `004B7540`.
  Полный вывод — `re\client-k1\slot_names.txt`, начало декомпиляции — `re\client-k1\slot_decomp.txt` (вне репо).
- **Как получить имена (первый шаг К5):** (1) сопоставить с серверным проектом `re\ghidra-proj-server` по коду — общий код
  движка (лучи, ресурсы, сущности) там уже разобран и назван (`re\vu\SEQUENCE-REPORT.md` — так находили адреса сервера);
  (2) рефлексия Frostbite: имя класса из `typeinfo` → его vtable → методы; (3) слоты VU — что VU передаёт в функцию (аргументы
  из декомпилята veniceext). Скрипт: `re\client-k1\slot_names2.py` (функции создаёт во временной транзакции с откатом;
  запуск `tools\pyghidra-mcp-venv\Scripts\python.exe`, проект не должен быть открыт MCP).

## Дальше по К1

- ✅ Ghidra-проект клиента готов (см. выше). Имён по RTTI нет — имена через сверку с сервером и рефлексию (К5, шаг 1).
- Для `GetLocalPlayer`, NetEvents клиента, событий тика и уровня — искать в `zbf3.exe` по RTTI (`ClientPlayerManager`,
  `ClientGameContext`, `UpdateManager`) и по коду vu-core (`re\vu\pyghidra_mcp\vu.gpr`), т. к. в таблице их нет.
