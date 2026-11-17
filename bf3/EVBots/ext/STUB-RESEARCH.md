# STUB-RESEARCH: адреса функций движка BF3 для заглушек VU Player/SoldierEntity

Дата: 12.10.2026 (правки 13.10)

> ⚠️ 19.10.2026: таблица ниже — исследование 12–13.10, частично устарела. `Player:SelectUnlockAssets` реализован (выбор через
> обработчик движка 0x00E331B0, сборка 185; applyCustomization 0x00E322F0); ExitVehicle/EnterVehicle — с 178. Заглушки сейчас —
> `HANDOFF.md` §6.
Объект исследования: `bf3_server_Final.exe` (32-битный, ImageBase 0x00400000).
Цель: найти VA (Virtual Address) функций движка, соответствующих ~14 методам
VeniceEXT, которые в `legacyext/src/VextPlayerManager.cpp` объявлены как заглушки
(красят Lua-объект, движок не трогают).
Код DLL в этом отчёте НЕ меняется — только документируется.

---

## 1. Метод поиска (кратко, подробности — в истории)

1. **Таргет строки.** Имена в `.rdata` адресуются прямо в машинный код как
   `PUSH imm32` (лежат в `.text`). Точка вставки = следующая инструкция после
   PUSH. Это даёт **VA функции**, в которую строка попадает (прок-блок функции).
2. **Файл ↔ RVA.** `.text` raw = 0x400, `.text` RVA = 0x1000 → вычитать 0xC00 из
   файлового смещения, чтобы получить RVA/VA. Безусловно `.rdata` (все строки
   выше `0x02400000`) уже в VA-разрешении: в список войдут готовые VA.
3. **CALL НЕ искать.** x86 direct CALL — rel32, и по строке код не найти иначе
   как через перекрёстные ссылки Ghidra. Строка «живая», если на неё есть ref
   **в `.text` (код)**; ссылки в `logevent`-базу (`0x02C…`) — это журнал, а не код.
4. **Мёртвые строки** = нет ref в `.text` вовсе (или только logevent). Их нельзя
   превратить в адрес без анализа данных.
5. Результат подтверждали декомпиляцией в Ghidra (postScript): строки
   проф-блока с суффиксом `_` дают читаемый код; строки `fb::ServerSoldierEntity::*`
   почти всегда «мёртвые» — текущий вывод: такие имена не PUSH-аются из `.text`
   вообще (только в assert/logevent), поэтому функция начинается в другом месте.

---

## 2. Сводная таблица «метод VU → адрес движка»

| # | VU-метод (сигнатура по playermanager.md) | Адрес функции (VA) | Refer (PUSH-строка) | Уверенность | Как искал |
|---|-------------------------------------------|--------------------|---------------------|-------------|-----------|
| 1 | `SoldierEntity:ApplyCustomization(customizationData)` | **0x00E322F0** | 0x00E32B85 = "fb::ServerPlayer::applyCustomization" | высокая | PUSH-строка + декомпиляция |
| 2 | `Player:EnterVehicle(vehicle, entryId)` | **0x00E37950** | 0x00E37A7C = "fb::ServerPlayer::enterVehicle" | высокая | PUSH-строка + декомпиляция |
| 3 | `Player:ExitVehicle(force, exitOnEntry)` | **0x00E35AC0** | 0x00E35AF1 = "ServerPlayer_exitControllable" | высокая (кандидат) | PUSH-строка + декомпиляция |
| 4 | `Player:ExitVehicle` (альтернатива по семантике) | **0x00E36090** | 0x00E360CD = "ServerPlayer_leaveControllable" | высокая (кандидат) | PUSH-строка + декомпиляция |
| 5 | `SoldierEntity:SetTransform(transform)` | **0x00E0C0B0** | 0x00E0C0EF = "ServerSoldierEntity_setSoldierTransform" | высокая | PUSH-строка + декомпиляция |
| 6 | `Player:SelectWeapon(slot, weaponUnlockAsset, unlockAssets)` | **не найдено** | `fb::ServerPlayer::onPlayerSelectedWeaponMessage` — только logevent 0x02C57770 | низкая | строка не PUSH-ается из кода |
| 7 | `Player:SelectUnlockAssets(characterCustomizationAsset, …)` | **не найдено** | `fb::ServerPlayer::onPlayerSelectedUnlockAssetsMessage` — только logevent 0x02C57784/0x02C57798 | низкая | строка не PUSH-ается из кода |
| 8 | `Player:AttachSoldier(soldier)` | **не найдено** | `fb::ServerPlayer::attachCharacter` — только logevent 0x02C57978/0x02C5798C | низкая | строка не PUSH-ается из кода |
| 9 | `Player:EnableInput(action, enable)` | **не найдено** | — (нет прямой строки; компонент ввода разобран лишь частично) | низкая | нет строки-якоря |
| 10 | `Player:Fade(fadeTime, fadeToBlack)` | **не найдено** | — | низкая | нет строки-якоря |
| 11 | `SoldierEntity:SetPosition(position)` | **не найдено** | найден только несвязанный `SetPositionRigOp` | низкая | референсы не дают функции |
| 12 | `SoldierEntity:SetPose(pose, immediately, overridePending)` | **не найдено** | — (строка мёртвая) | низкая | нет PUSH в `.text` |
| 13 | `SoldierEntity:Kill()` / `ForceDead()` | **не найдено** | ближайшее: `ServerSoldierEntity_onDamage` refer 0x00E08027 (функцию создать не удалось); есть `VehicleEntityHealth::setState` — для техники | низкая | стратегия onDamage не собралась |
| 14 | `SoldierEntity:SingleStepEntry(entryId)` | **не найдено** | — | низкая | нет строки-якоря |

Примечание к таблице:
- «Адрес» — это **VA** (ImageBase 0x400000). В коде DLL при вызове через базу
  модуля (`GetModuleHandleA("bf3_server_Final.exe")`) нужно вычитать 0x400000.
- Refer — VA, куда строка PUSH-ится; на 1–5 адресов это подтверждено
  декомпиляцией (re/stub_decomp.txt).

---

## 3. Подробно: найденные функции (что за сигнатура)

Строки проф-блока (с суффиксом `_`) дают полноценные функции. Расшифровка по
`re/stub_decomp.txt`.

### 3.1 `fb::ServerPlayer::applyCustomization` → `0x00E322F0` (высокая)
- Пуш-строка `fb::ServerPlayer::applyCustomization` по `0x00E32B85` —
  внутри самой функции как имя assert-блока.
- **Декомпиляция:** `void FUN_00e322f0(uint soldier/player, int unkParam, uint customizationData)`.
  Читает у игрока `*(this+0x54C)` (активный солдат), дёргает
  `ServerSoldierEntity` и валит: «No WeaponSlot set on weapon», «Too many unlocks
  on weapon!  Max allowed is 5», «No active weapon can be set».
- Это метод **ServerPlayer**, но в VeniceEXT `Player:SelectUnlockAssets` и
  `SoldierEntity:ApplyCustomization` адресуют тот же пользовательский слой
  (кастомизация солдата комплектом). Пригоден как **реальный** аналог
  `SoldierEntity:ApplyCustomization`.
- Сигнатура для DLL-вызова (thiscall): `void applyCustomization(ServerPlayer* this /*ECX*/, uint customization, bool addToPool)`.

### 3.2 `fb::ServerPlayer::enterVehicle` → `0x00E37950` (высокая)
- Пуш-строка `fb::ServerPlayer::enterVehicle` по `0x00E37A7C` — точка возврата
  из assert-блога внутри функции.
- **Декомпиляция:** `undefined4 FUN_00e37950(int entry /*ECX*/, uint entryId)`.
  Проверки: «Trying to enter non-spawned vehicle», «ServerEntryComponent::getEntry
  invoked with invalid index (%i)». Внутри ищет других игроков в entry, вызывает
  `FUN_00e35450` = `enterControllable` (0x00E35450), затем `leaveControllable`,
  `d7c930`, сетку. Возвращает `1`/`0`.
- Сигнатура (thiscall): `int enterVehicle(ServerPlayer* this /*ECX*/, uint entryId)`.
- VU `Player:EnterVehicle(vehicle, entryId)` — транспорт: vehicle игрока мы
  передаём как входной `ServerEntryComponent`; метод идеально ложится.

### 3.3 `ServerPlayer_exitControllable` → `0x00E35AC0` (высокая, кандидат для ExitVehicle)
- Пуш-строка `ServerPlayer_exitControllable` по `0x00E35AF1`.
- **Декомпиляция:** `void FUN_00e35ac0(void)` (this в ECX). Освобождает
  controllable (`in_ECX+0x3D8`), повторно входит в entry, чистит поля.
- VU `Player:ExitVehicle(force, exitOnEntry)` ближе всего к этой паре —
  `exitControllable` (уйти из управляемого) + `leaveControllable` (покинуть entry
  целиком). Обе адресуются → кандидаты для ExitVehicle.

### 3.4 `ServerPlayer_leaveControllable` → `0x00E36090` (высокая, кандидат)
- Пуш-строка `ServerPlayer_leaveControllable` по `0x00E360CD`.
- **Декомпиляция:** `undefined4 FUN_00e36090(undefined4 param_1, char param_2, char param_3)`
  — это **theFastCall-like** сигнатура: `(this /*ECX*/, entryId, forced = exitOnEntry, teleport)`
  → в точности параметры VU `ExitVehicle(force, exitOnEntry)`.
  Внутри решает, как вернуть игрока на карту (через конец entry или выход из
  техники), вызывает `0x00E35AC0` (exitControllable) и `0x00E35450` (enterControllable).

Итого для `Player:ExitVehicle` рекомендуем использовать **0x00E36090**
(`leaveControllable`, сигнатура `(entryId, force, exitOnEntry)`), а
`0x00E35AC0` — как внутренний шаг выхода из технического controllable.

### 3.5 `ServerSoldierEntity_setSoldierTransform` → `0x00E0C0B0` (высокая)
- Пуш-строка `ServerSoldierEntity_setSoldierTransform` по `0x00E0C0EF`.
- **Декомпиляция:** `void FUN_00e0c0b0(void* this /*ECX*/, char updatePhysics=false)`.
  Валидирует transform через `FUN_004aa9d0` (9 float-полей xyz/rot), только
  потом `FUN_0083d1d0(this)` (применение transform на сущность) и при
  `updatePhysics` разницу физики. Assert-строка «Invalid transform in
  ServerSoldierEntity::setSoldierTransform: %s».
- VU `SoldierEntity:SetTransform(transform)` — **прямое соответствие**.
- Вложенные вызовы: `0x00E08250` (ServerSoldierEntity::update?), `0x00E07730`,
  `0x008BB0C0`, `0x00E0AAA0` — внутренняя физика солдата.

### 3.6 Бонусные функции, найденные в ходе поиска (для физики/боя)
| Строка (проф) | Refer (PUSH) | Функция (VA) | Декомпиляция | Уверенность |
|---------------|--------------|--------------|--------------|-------------|
| `ServerSoldierEntity_postPhysicsUpdateEntry` | 0x00E0D01C | **0x00E0CFE0** | `void FUN_00e0cfe0(void*)` — финальные обновления физики, триггеры revive/движения | средняя (не ключ к заглушкам) |
| `ServerSoldierEntity_spawn` | 0x00E0EB4D | **0x00E0EB10** | `void FUN_00e0eb10(char*)` — спавн солдата, ставит transform, вешает компоненты | средняя |
| `ServerSoldierWeaponsComponent_updateWeaponFiring` | 0x00E09790 | **0x00E09750** | `void FUN_00e09750(undefined4)` — обработчик fire-цикла оружия, sway, «No weaponSwayCallback!» | высокая |
| (мёртвые) `onDamage` refer 0x00E08027, `prePhysicsUpdateEntry` refer 0x00E0A7F8 | — | функцию создать не удалось | старт функции не найден: имя PUSH-ится только в logevent/assert-базу | низкая |

---

## 4. «Не найдено» — почему и что ещё пробовать

1. **`Player:SelectWeapon`** — якорь-строка `fb::ServerPlayer::onPlayerSelectedWeaponMessage`
   есть, но единственная ссылка — в logevent-базу `0x02C57770` (журнал для
   отладки отложенной обработки сети), а не в `.text`. Значит реальный метод —
   обработчик сетевого сообщения, и его адрес надо искать через vtable
   `ServerPlayer::onMessage`/диспетчер сообщений (не через строку).
2. **`Player:SelectUnlockAssets`** — то же самое для
   `onPlayerSelectedUnlockAssetsMessage` (0x02C57784/0x02C57798). Мост к коду
   отсутствует; применить можно через уже найденный `applyCustomization`
   (0x00E322F0), т.к. он настраивает комплект/слоты оружия.
3. **`Player:AttachSoldier`** — `attachCharacter`/`setCharacter` так же
   logevent-only (0x02C57978/0x02C578B0). В VeniceEXT это привязка солдата к
   игроку; вероятно реализуется через `createSoldier` + `ServerPlayerEntryComponent`
   (метод `FUN_00e2ab10` — «soldier attached»).
4. **`Player:EnableInput(action, enable)`** — задействует ServerPlayerInputTrigger/
   ServerInputBinding. Точечной строки нет; входной компонент разбирается
   (INPUT-DUMP в мосте), но адрес метода ввода не найден. Искать в vtable
   `ServerPlayer` (слоты ~0x90–0xC0).
5. **`Player:Fade`** — метод экранного фейда (FadeEntity). Отдельная строка
   `FadeEntity`/`ServerFade` в filter пустая — в этом билде серверных
   fade-строка нет. Скорее всего это client-side эффект; на сервере обрабатывается
   через `ServerPlayerView`/`ServerFadeManager` — не локализовано.
6. **`SoldierEntity:SetPosition(position)`** — найден только `SetPositionRigOp`
   (операция-риг тела), который **не** является позицией солдата. В движке
   позиция ставится через `setSoldierTransform` (0x00E0C0B0) — VU `SetPosition`
   можно реализовать поверх него (взять текущий rot из transform, поменять trans).
7. **`SoldierEntity:SetPose(pose, immediately, overridePending)`** — строки-якоря
   нет в `ServerSoldier` (pose живёт в `SoldierPoseComponent`). Искать в vtable
   pose-компонента — не сделано.
8. **`SoldierEntity:Kill()` / `ForceDead()`** — убийство солдата идёт через
   `ServerSoldierEntity::onDamage/updateDamage` (refer 0x00E08027) — функцию
   создать **не удалось** (строка в logevent, старт функции не определён), и
   `VehicleEntityHealth::setState` (есть адрес, но это техника, не солдат).
   Остаются открытыми: классический путь — послать урон через
   `ServerDamageManager`/`damageMessage`.
9. **`SoldierEntity:SingleStepEntry(entryId)`** — «шаг в точку входа»;
   похоже на `enterControllable(entryId)` (0x00E35450), можно реализовать через
   него. Отдельной строки `SingleStepEntry` нет.

---

## 5. Итог и рекомендации по реализации

- **Прямые, готовые к вызову VA (высокая уверенность):**
  - `applyCustomization`/SelectUnlockAssets → **0x00E322F0**
  - `EnterVehicle` → **0x00E37950**
  - `ExitVehicle` → **0x00E36090** (+ внутренний шаг 0x00E35AC0)
  - `SetTransform` → **0x00E0C0B0**
- **Реализуемы через найденные:** `SetPosition` (через 0x00E0C0B0 с изменением trans),
  `SingleStepEntry` (через `enterControllable` 0x00E35450).
- **Требуют отдельного реверса (vtable/диспетчер сообщений), сейчас «не найдено»:**
  `SelectWeapon`, `SelectUnlockAssets`, `AttachSoldier`, `EnableInput`, `Fade`,
  `SetPose`, `Kill`/`ForceDead`.

### Рецепт вызова из legacyext
```cpp
// VA из таблицы прибавить к базе модуля минус ImageBase:
//   fn = (F)((uintptr_t)base + (va - 0x00400000));
// __thiscall: в x86 это __fastcall с фиктивным первым параметром ECX.
// Аргументы/сигнатуры — из декомпиляций (раздел 3).
```

---

## 6. Источники/артефакты
- `re/stub_decomp.txt` — декомпиляции всех найденных функций (0x00E0C0B0, 0x00E32B85,
  0x00E37A7C, 0x00E3548D, 0x00E35AF1, 0x00E360CD, 0x00E09790, 0x00E0D01C, 0x00E0EB4D,
  0x00E08027, 0x00E0A7F8, «SoldierEntity exits vehicle at pos» 0x00E3633B → 0x00E36090).
- `re/wide_scan.txt` — «живые/мёртвые» строки по классам (ServerPlayer, ServerSoldierEntity…).
- `re/method_filter.txt` — фильтр по всем классам (что МЁРТВАЯ в `.text`).
- `re/soldier_refs.txt` — уникальные ссылки строк SoldierEntity/ServerSoldierEntity → PUSH.
- `re/player_mgr.txt` — декомпиляция `createPlayer`/`addToArray`/fun-00ddbbd0
  (подтверждает картину адресов, пути к спавну).
- `trash/2026-10-12/pe_find.py`, `find_soldier.py`, `packed_refs.py`, `stub_scan.py`,
  `wide_scan.py`, `method_index.py`, `class_scan.py` — скрипты поиска.
- `legacyext/src/VextPlayerManager.cpp` — список заглушек (раздел с Lua-объектами).
- `VU-Docs/content/vext/ref/server/type/{player,soldierentity}.md` — эталонные сигнатуры VU.

---

## 7. Проверка по бинарнику (13.10, читает только `bf3_server_Final.exe`)

Сделано: дизассемблирование Capstone от каждого адреса, сверка секций PE,
подсчёт `RET imm16` и стековых аргументов `[ebp+N]`. Скрипт вне репо:
`scratchpad\verify_stubs.py`.

**Адреса — настоящие функции.** `ImageBase = 0x400000`; пролог на месте
(`55 8B EC 83 E4 F0` у `leaveControllable`/`setSoldierTransform`,
`81 EC …` у `enterVehicle`/`exitControllable`); строки PUSH попадают внутрь
функции (смещение 0x31…0x12C).

**Сигнатуры из раздела 3 неверны — вызывать пока нельзя:**

| Функция | В отчёте | По бинарнику |
|---|---|---|
| `applyCustomization` 0x00E322F0 | thiscall `(this, customization, addToPool)` | стековые аргументы `[ebp+0xC]`, `[ebp+0x10]`, `[ebp+0x38..0x45]` — **не thiscall**; совпадает с `STATE.md` (cdecl). Точное число аргументов не снято: `RET` не найден в первых 0x1000 байтах |
| `enterVehicle` 0x00E37950 | `int (this, entryId)` | `RET 8` → 2 стековых + ECX; читает `[ebp+0xA]`, `[ebp+0xFC]`, `[ebp+0x100]` — один аргумент из отчёта не объясняет |
| `setSoldierTransform` 0x00E0C0B0 | `void (this, updatePhysics=false)` | `RET 8` → 2 стековых; читает только `[ebp+0xC]` — **первый аргумент не виден**, семантику transform надо доказать |
| `leaveControllable` 0x00E36090 | `(this, entryId, force, teleport)` | `RET 0xC` → 3 стековых + ECX; согласуется с отчётом по числу |
| `exitControllable` 0x00E35AC0 | `void (this)` | `RET 0` → 0 стековых + ECX; согласуется |
| `enterControllable` 0x00E35450 | (шаг для SingleStepEntry) | `RET 8` → 2 стековых |
| `spawnSoldier` 0x00DDBBD0 (**мост, рабочий**) | 4 стековых + ECX/EDX | `RET 0x10` → 4 стековых — **совпадает с кодом моста**, это подтверждение |

**Вывод.** Подключать из моста можно только функцию, у которой сигнатура
подтверждена по `RET` и по тому, какие `[ebp+N]` читаются. По этому критерию
сейчас готов только `spawnSoldier` (уже в работе). `applyCustomization`,
`setSoldierTransform`, `enterVehicle` требуют полного разбора аргументов перед
вызовом: неверное число аргументов портит стек и роняет сервер (уже было, см.
`MODLOG.md`, 0xC0000005). Встраивать до этого — не делаем.

**Следующий шаг (по порядку):**
1. `setSoldierTransform`: проверить, откуда читается transform (`[ebp+8]` без
   явного обращения — возможно через `lea`/копию), и что делает `[ebp+0xC]`.
2. `applyCustomization`: снять полный `RET` (функция длинная), сверить 3
   аргумента с `SelectUnlockAssets` VU.
3. `enterVehicle`: сверить `entryId` с `ServerEntryComponent::getEntry`.
   Только после этого — вызов из моста на тест-хосте, затем на стенде.
