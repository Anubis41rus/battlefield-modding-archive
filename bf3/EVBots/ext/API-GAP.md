# API-GAP — что мост умеет, а что нет

> Честный реестр поверхности VU, которую мы эмулируем в
> `legacyext/src/VextPlayerManager.cpp`. Обновляется по мере работы.
> Связан с `MODLOG.md` (хронология) и `PLAN.md` (план).

## Как читать

- **✅ натив** — метод вызывает настоящую функцию движка.
- **⚠️ частично** — вызывает движок, но чего-то не хватает (видимость, сеть).
- **⛔ заглушка** — только пишет в Lua-таблицу, игру не трогает.

> ⚠️ 19.10.2026 (сборка 198): файл от 12.10, многие строки устарели. Актуальный список заглушек — `HANDOFF.md` §6.
> Исправлено здесь: `CreateSoldier` (ниже). Адреса в файле — смесь VA и RVA (RVA = VA − 0x400000), сверять по коду.

**На 12.10: 16 заглушек из ~30 методов поверхности игрока (устарело).**

---

## PlayerManager

| Метод | Статус | Комментарий |
|---|---|---|
| `GetPlayers` | ✅ | Lua-реестр + нативные игроки |
| `GetPlayerById` / `ByName` / `ByOnlineId` / `ByGuid` | ✅ | |
| `GetPlayersByName` / `ByTeam` / `BySquad` | ✅ | |
| `GetPlayerCount` | ⚠️ | считает Lua-объекты, не нативных |
| `GetSquadLeader` | ⚠️ | отряд не зарегистрирован |
| **`CreatePlayer`** | ✅ | зовёт `createPlayer` (0x00E644B0) + `addToArray` |
| **`DeletePlayer`** | ⚠️ | |
| `SetSpectatorEnabled` / `IsSpectatorEnabled` | ⛔ | заглушка |
| `FadeInAll` / `FadeOutAll` / `GetSpectatorCount` | ⛔ | заглушки |

## Player

| Метод | Статус | Комментарий |
|---|---|---|
| **`CreateSoldier`** | ⚠️ | Lua-объект: `NativeBridge.CreateEngineSoldier` всегда 0 (VextPlayerManager.cpp, «адрес создания солдата ещё не разобран»); настоящее тело создаёт `SpawnSoldierAt` (createSoldier+spawnSoldier движка, как VU, сборка 185). Прогон 196: 362 из 362 — фолбэк |
| **`SpawnSoldierAt`** | ⚠️ | мост зовёт `spawnSoldier` (0x00DDBBD0) на своей точке или точке игрока. Солдат появляется, но НЕ в сети и чужая команда гибнет в зоне поражения |
| `Spawn(spawnPoint, checkArea)` | ⛔ | всегда `false` |
| **`SelectWeapon`** | ⛔ | не найдено (vtable-реверс нужен) — `ext\STUB-RESEARCH.md` |
| **`SelectUnlockAssets`** | ⚠️ | прямой анкор не найден; реализуемый аналог `applyCustomization` 0x00E322F0 |
| `SelectUnlockAssets` (3 аргумента) | ⛔ | |
| **`EnableInput`** | ⛔ | адрес найден: `setLevel` 0x008BEF00 (массив float[10] в +0x10, битсет в +0x1E0/0x1E4) — не хватает смещения поля `input` в ServerPlayer |
| `AttachSoldier` | ⛔ | логevent-only; через `createSoldier` + `entryComponent` |
| `SetSquadLeader` | ⚠️ | только флаг в Lua |
| `EnterVehicle` / `ExitVehicle` | ✅ адреса найдены | 0x00E37950 / 0x00E36090 — в build не встроены, ждут ввода |
| `Fade` | ⛔ | |
| `Kick` / `Ban` / `BanTemporarily` / `BanByName` / `BanByNameTemporarily` / `BanByIp` | ⛔ | 6 заглушек, мод их почти не зовёт |

## Player — свойства

| Свойство | Статус | Комментарий |
|---|---|---|
| `name`, `id`, `teamId`, `squadId` | ✅ | |
| `nativePtr` | ✅ | указатель на `ServerPlayer` |
| `hasSoldier`, `soldier` | ⛔ | ссылаются на Lua-объект, не на нативного солдата |
| **`input`** | ⚠️ | объект `EntryInput` есть (был `nil` — мод не мог управлять), но `SetLevel` уходит в `NativeBridge.SetInputLevel`, который пока только логирует. Адрес массива уровней в движке не найден |
| `selectedKit` | ✅ | мод достаёт сам через `ResourceManager` |
| `corpse`, `attachedControllable`, `controlledControllable` | ⛔ | |
| `score`, `kills`, `deaths` | ⛔ | `PointSystem` не знает ботов: `No player info found for victim` |
| `isAllowedToSpawn` / `On` | ✅ | |

## SoldierEntity

| Метод | Статус |
|---|---|
| `SetPosition` | ⛔ пишут в таблицу; реализуется поверх `setSoldierTransform` 0x00E0C0B0 |
| `SetTransform` | ✅ адрес найден 0x00E0C0B0 — в build не встроен |
| `SetPose` | ⛔ нет строки-якоря |
| `Kill` / `ForceDead` | ⛔ ставят `hasDied` в таблице; путь — `ServerDamageManager`, «не найдено» |
| `ApplyCustomization` | ✅ адрес найден 0x00E322F0 — в build не встроен |
| `SingleStepEntry` | ⛔ реализуемо через `enterControllable` 0x00E35450 |
| остальное (`GetTransform`, `IsAlive`, ...) | ⛔ |

---

## Найденные адреса движка (проверены)

| Что | RVA | Статус |
|---|---|---|
| `createPlayer` | 0x00A644B0 | ✅ работает |
| `addToArray` | 0x00A6CA00 | ✅ работает |
| `spawnSoldier` | 0x009DBBD0 | ✅ солдат создаётся |
| `onCreatePlayerMessage` | 0x00A48490 | ⛔ роняет сервер `80000003` |
| `ServerPlayer::update` | 0x00A5EF00 | ✅ источник `g_managerPtr` |
| `ServerPlayerSquadManager::addToTeam` | 0x00D8EF80 | ⚠️ хук добавлен, ждёт прогона |
| `spawnMgrGet` | 0x00A3E1D0 | ⛔ запрещён (роняет) |
| конструкторы точек спавна | 0x00DDDE30 / 0x00DDE1D0 | ⛔ запрещены (роняют) |
| `ServerPlayerSquadManager::addToTeam` | 0x00D8EF80 | ⚠️ сигнатура разобрана, хук роняет сервер |
| `ServerCharacterEntity_spawn` | 0x00D48930 | обёртка профилирования |
| **`настоящая посадка персонажа`** | **0x00E18900** | ⚠️ разбирается |
| `ServerSoldierEntity_init` | 0x00D51180 | обёртка профилирования |
| инициализация солдата | 0x00E340A0 | ⚠️ разбирается |

## Что удалось узнать про `addToTeam` (0x00D8EF80)

```
addToTeam(игрок=F2DDD020 команда=1 отряд=1 менеджер=04E3F100)
```

Функция **статическая**, не метод: все 4 параметра в стеке.
Первое «похожее на `this`» значение меняется между прогонами — это мусор
в ECX, а не `this`. Хук отключён (`LEGACYEXT_SPM_SQUAD=0`): четыре попытки
дать четыре `ACCESS_VIOLATION`, причина в украденных байтах не найдена.

## Признак AI-игрока

`+0x318` — байтовый флаг `isAIPlayer()`. Движок проверяет его **при
посадке** в `spawnSoldier`:

```c
if ((*(char *)(param_1 + 0x318) == '\0') && (*(int *)(in_ECX[5] + 0x180) != 0))
```

Тип задаётся **данными**, а не кодом:

| Blueprint | PlayerSpawnType |
|---|---|
| `characters/soldiers/mpsoldier` | `PlayerSpawnType_HumanPlayer` |
| `characters/soldiers/aisoldier` | `PlayerSpawnType_AiPlayer` |

Наш бот — HumanPlayer без сетевого подключения, поэтому клиент его не
рисует. Это диагностика, а не решение: пользователь wants Lua-ботов.

## Смещения в `ServerPlayer`

| Смещение | Что | Как узнано |
|---|---|---|
| `+0x10` | имя | строки логов менеджера отрядов |
| `+0x20` | **onlineId / personaId** | дифференциальный дамп |
| `+0x31C` | команда | эталон в момент спавна |
| `+0x318` | 0 (НЕ «флаг AI») | эталон в момент спавна |
| `+0x3C4` | солдат | проверка живости |
| `+0x480` | указатель, заполняемый до посадки | эталон в момент спавна |

---

## Порядок работы (почему такой)

```
1. addToTeam        → таб, очки, респавна          [в работе]
2. CreateSoldier    → настоящий SoldierEntity       [нужен адрес функции]
3. SpawnSoldierAt   → посадка через штатный путь
4. SelectUnlockAssets / SelectWeapon → комплект и оружие
5. EnableInput      → боты поедут
6. SoldierEntity:*  → управление солдатом
7. Сетевой путь     → видимость тел (бэкенд, см. BACKEND-PATH.md)
```

Пункт 7 не решается в DLL: клиент не получает сущность солдата без
сетевого подключения игрока.
