# Трек 2 — техника: посадка и выход ботов (EnterVehicle / ExitVehicle, события Vehicle:*)

Агент (haiku) 16.10.2026; файл записал оркестратор из сообщения агента (агенту запись .md запрещена). Только чтение, capstone.
Листинги и скрипты агента: `C:\Temp\claude\C--Games\c4657c58-f592-45b8-8608-819ca88fc837\scratchpad\2\` (enterVehicle/enterControllable/exitControllable/leaveControllable/fn_E304E0 `.asm.txt`; pe_util.py, analyze4.py, xref*.py, callers.py, encl*.py, msgs.py, flagw.py).

## Сводка
1. Все четыре функции thiscall, ECX = ServerPlayer*. enterVehicle(техника, entryId): RET 8, bool в AL. enterControllable(техника, entryId): RET 8, void. exitControllable(): RET 0. leaveControllable(a1, a2, a3): RET 0xC, все байтовые флаги; a1 — НЕ entryId.
2. Опровергнуто из STUB-RESEARCH: enterVehicle(this, entryId) неверна; enterVehicle не вызывает leaveControllable (вызывает exitControllable и enterControllable).
3. enterVehicle вызывают 7 мест (спавн в технике, загрузка сохранения, триггер, VeniceServerPlayerExtent::spawnOnVehicle и др.); leaveControllable — 2 (обёртка 0xE36540 и неопознанное). Только прямые call.
4. ГЛАВНЫЙ БЛОКЕР: мост не отдаёт технику моду (VextEntityManager.cpp:15–16, итератор пуст), событий Vehicle:* нет; без объекта техники (entryCount, GetPlayerInEntry, data.controllableType, PhysicsEntity) мод не работает.
5. Вызывать движок только из DetourSpmUpdate (каждый кадр) в HookScope + OnEngineThreadCtx, через очередь. LegacyBridgeRetryPending раз в 2 с — для техники мало.
6. Vehicle:Enter — хук enterVehicle после оригинала при AL=1 + проверка [player+0x3D8]==техника. Vehicle:Exit — хук leaveControllable, технику читать до вызова. enterControllable без фильтра не брать: его вызывает и спавн солдата.
7. Открыто: соответствие ExitVehicle(force, exitOnEntry) флагам a1/a2/a3 (гипотеза a2=exitOnEntry); что с солдатом при [player+0x3C8]==0.
8. Уверенность: сигнатуры и вызовы — высокая; флаги выхода — низкая; путь человека к enterVehicle не найден — низкая.

## 2.1 Мод
- Посадка: Bot\BotActions.lua:272–369 `Bot:_EnterVehicleEntity`: свободное место по `GetPlayerInEntry(seat)==nil` (327) → `self.m_Player:EnterVehicle(p_Entity, s_SeatIndex)` (328) → сразу `PhysicsEntity(p_Entity).internalHealth` (329), таймеры (336–350), `_SetVehicleObjectiveState` (344). Результат не проверяется.
- Поиск: BotActions.lua:375–400 — `EntityManager:GetIterator('ServerVehicleEntity')` (376), ближайшая в MIN_DISTANCE_VEHICLE_ENTER.
- Прочие EnterVehicle: Bot.lua:345, 360, 375; BotSpawner.lua:185, 207, 1416, 1433, 1517; BotManager.lua:494, 531, 536.
- Выход: VehicleActions.lua:8–30 `ExitVehicle(true, false)`; BotManager.lua:530 `ExitVehicle(false, false)`.
- Подписки: Server\__init__.lua:129–133 (Vehicle:SpawnDone, Unspawn, Damage, Enter, Exit), обработчики 511–546; PlayerData.lua:32–34 (`g_Vehicles:GetVehicleByEntity(...).Type`, nil — ошибка), AirTargets.lua:24, 31.
- Реестр: SpawnDone → GameDirector:OnVehicleSpawnDone (635), GetSpawnableVehicle (572); SpawnInJet — `controlledControllable` (BotSpawner.lua:1577).
- Нужно от объекта: `VehicleEntityData(e.data).controllableType`, `entryCount`, `GetPlayerInEntry(i)`, `transform.trans`, `PhysicsEntity(v).internalHealth`; у игрока `controlledControllable`, `controlledEntryId`.
- VU: Player:EnterVehicle(vehicle, entryId), Player:ExitVehicle(force, exitOnEntry) (player.md:462–482); Vehicle:Enter/Exit(vehicle, player).

## 2.2 Мост
- Заглушки VextPlayerManager.cpp:576–580; техника не отдаётся VextEntityManager.cpp:15–16; событий Vehicle:* нет.
- Образец событий — опрос как CapturePoint (VextEntityManager.cpp:419–428).
- Lua: controlledEntryId=0, controlledControllable=nil (VextPlayerManager.cpp:386, 626–628).
- DetourSpmUpdate (SpmProbe.cpp:461, VA 0x00E5EF00) каждый кадр; LegacyBridgeRetryPending раз в >2 с (SpmProbe.cpp:532–537). HookScope 2686–2689; OnEngineThreadCtx 4631–4641.

## 2.3 Дизассемблер
| Функция | VA | RET | Аргументы | Примечание |
|---|---|---|---|---|
| enterVehicle | 0x00E37950 | 8 | техника ([esp+0x1A0]), entryId ([esp+0x1AC]) | AL=1 (0xE37CE8) / 0 (0xE37CF7) |
| enterControllable | 0x00E35450 | 8 | техника, entryId | void |
| exitControllable | 0x00E35AC0 | 0 | — | void |
| leaveControllable | 0x00E36090 | 0xC | a1 dword-флаг, a2 byte, a3 byte | void |
- EDX нигде не вход ⇒ thiscall, объявлять __fastcall(self, fakeEdx, ...).
- enterVehicle: проверки техника≠0, [техника+0xA]&1 (заспавнена), entryId < ([+0x100]−[+0xFC])/4; солдат [player+0x3C4]==0 → 0; при [player+0x3E8]≠0 сначала exitControllable; затем enterControllable. AL=1 в общем эпилоге — после вызова проверять [player+0x3D8]==техника.
- enterControllable пишет место в [player+0x3D4] и [+0x3E4], вызывает 0xE1F7A0 (ECX=техника).
- leaveControllable: ветка A ([+0x3C8]==[+0x3E8]≠0) — место [+0x3D4], техника [+0x3D8]; ветка B — [+0x3E4]/[+0x3E8]; при [+0x3C8]==0 — только exitControllable; a2≠0 — копия 64 байт записи входа, a2==0 — 0xE304E0; a3≠0 — смещение vtbl[+0xE0]; в конце при [+0x3C8]≠0 enterControllable(player, [+0x3C8], 0).
- Xref enterVehicle: 0x00DD90BC (спавн в технике), 0x00DF75B2 (сохранение), 0x00E37E8F, 0x00E5B28B (триггер; метка Ghidra «createPlayer» неверна), 0x00E66815, 0x0118651E (spawnOnVehicle), 0x0132D39B. leaveControllable: 0x00E36567 (обёртка 0xE36540 ← 0xD4D355, 0xDEF322), 0x00E367FB. enterControllable — также при каждом спавне солдата (через 0xE37180 «Attaching soldier to player»). exitControllable — 12 мест.
- Хеши сообщений: ServerVehicleEnterMessage 0xBE8B9F3B, ExitMessage 0x07147573, ClientPlayerEnterExitVehicleMessage 0x9F775494 (обработчик 0xF396E0), SpawnDone 0x809EF7A8, Unspawn 0x7AAB1773, Damage 0x936D3098, Destroyed 0x0234D2D0. Место отправки Enter/Exit не найдено.
- Флаг ассертов [0x28C8168] = 0, записей нет ⇒ int3 по статике не срабатывает.

## 3. Гипотезы
- H1: ExitVehicle(force, exitOnEntry) → leaveControllable: a2 ≈ exitOnEntry; force — a1 или a3 (проверить 4 комбинации).
- H2: при [+0x3C8]==0 солдат может не получить точку выхода.
- H3: путь человека (enterVehicle или enterControllable) — проверить хуками на стенде.
- H4: поля +0x3C8, +0x3D4/+0x3E4, +0x3E0, +0x3B0 — не определены полностью.
- H5: 0xE37180 — кандидат Player:AttachSoldier. H6: ServerVehicleSpawnDoneMessage — источник Vehicle:SpawnDone.

## 4. Опровергнуто
- enterVehicle(this, entryId); «enterVehicle затем leaveControllable»; leaveControllable(this, entryId, force, teleport); метка FUN_00e5b170 «createPlayer»; «ассерт роняет через int3».

## 5. Реализация (флаги по умолчанию ВЫКЛ: LEGACYEXT_VEHICLES=1, LEGACYEXT_VEH_EVENTS=1)
- Сигнатуры (RVA = VA − 0x400000): EnterVehicle 0x00A37950 `(self, edx, vehicle, entryId)` → uchar; LeaveControllable 0x00A36090 `(self, edx, a1, a2, a3)`; ExitControllable 0x00A35AC0 `(self, edx)`; EnterControllable 0x00A35450 (только наблюдение).
- Lua: `Player:EnterVehicle` → `NativeBridge.VehEnter(ptr, vehicle.nativePtr, entryId)`; `Player:ExitVehicle(force, exitOnEntry)` → `NativeBridge.VehLeave(ptr, force, exitOnEntry, 0)` — только очередь.
- C++: кольцо на 32 под SRWLOCK, VehDrain() из DetourSpmUpdate в HookScope; проверки Readable, солдат, [vehicle+0xA]&1, entryId < count, [player+0x3E8]==0; __try и лог каждого вызова.
- События: inline-хук enterVehicle (после оригинала, AL=1 и [+0x3D8]==техника) и leaveControllable (технику до вызова) → очередь → Lua `Events._DispatchEngine('Vehicle:Enter'/'Vehicle:Exit', vehicleObj, playerObj)`; обновлять controlledControllable/controlledEntryId.
- Блокеры вне трека: (1) итератор ServerVehicleEntity пуст (нужны transform, entryCount, GetPlayerInEntry, controllableType, internalHealth); (2) Vehicle:SpawnDone/Unspawn/Damage; (3) controlledControllable.
- Риски: конвенция (ModdingInterface.cpp:42), OOB внутри движка, вложенный Lua, устаревший указатель техники (перепроверять через итератор по typeinfo, VextTicketManager.cpp:35–73).

## 6. Проверить на стенде
- Строки «VEH enter this=… vehicle=… entry=… ret=…», «VEH leave this=… a1/a2/a3 … pre3C8=…».
- После enterVehicle: [player+0x3D8]==техника, [+0x3D4]==entryId, [+0x3E8]≠0.
- leaveControllable — 4 комбинации флагов, позиция солдата до/после.
- Человек садится/выходит руками — какие хуки срабатывают.
