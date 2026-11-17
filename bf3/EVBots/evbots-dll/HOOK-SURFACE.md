# HOOK-SURFACE — какие перехваты нужны LegacyEXT

**Почему этот документ существует.** Правило ноль скила
`reverse-engineering`: **читай настоящее, не гадай**. `botlayer.dll` — чужая
попытка сделать нативных ботов, **не эталон** и **не источник истины** для
нашей поверхности событий.

Из botlayer берётся только **механизм перехвата** (inline 8 байт + трамплин).
Это базовая техника x86: без неё не сделаешь ни один мод BF3, включая
аналог `veniceext.dll`. Сама по себе она не имеет отношения к ботам.

Поверхность событий выводится из **того, что реально нужно Lua-модам**.

---

## 1. Что должно работать (из fun-bots, 114 .lua)

### 1.1. Хуки VEXT — 13 серверных и shared

Это точки, где мод может **изменить** поведение. Для ботов важнейшие:

| Хук | Что делает бот |
|---|---|
| `Player:findbestteam` | выбирает команду для спавна |
| `Player:findbestsquad` | выбирает отряд |
| `Player:requestjoin` | игрок просит вступить в отряд |
| `Player:selectteam` | игрок выбирает команду |
| `Soldier:damage` | реакция на урон (медики, реакции) |
| `Entityfactory:create` | спавн сущностей |
| `Entityfactory:createfromblueprint` | спавн по чертежу (оружие, техника) |
| `Bulletentity:collision` | попадание пули — цель для стрельбы |
| `Server:suppressenemies` | отключение врагов |
| `Entity:shouldcollidewith` | столкновения (shared) |
| `Resourcemanager:loadbundles` | загрузка ресурсов (shared) |
| `Terrain:load` | загрузка ландшафта (shared) |
| `Visualterrain:load` | визуальный ландшафт (shared) |

Источник: `vext\ref\server\hook\`, `vext\ref\shared\hook\` → `vext-inventory.md`.

### 1.2. События — 26, на которые подписан fun-bots

**Серверные (критичны, без них боты не появятся):**

```
Engine:Init          Level:Loaded        Level:Destroy
Player:Created       Player:Killed       Player:Died
Player:EnteredVehicle  Player:LeftVehicle
Soldiertype:Deployed  Soldiertype:Undeployed
```

**Клиентские (нужны для интерфейса и локального UI бота):**

```
UI:LayoutMobile     UI:LayoutDesktop    UI:LayoutXBox
UI:MapLayer         UI:IngameMenuBackground
NetMessages:Received
```

**Прочие, на которые подписан мод:**

```
Play:Persist        Play:ServerSettingChanged
Play:MatchSettingsChanged
Soldier:WeaponFired Soldier:Damaged
Vehicle:Created     Vehicle:Entered    Vehicle:Left
UI:CharacterScoreScreen
```

**Плюс один кастомный, который VU-шный API не покрывает:**
`OnEntityCreated` — fun-bots подписывается на него отдельно.

### 1.3. Менеджеры — 51 вызов

| Библиотека | Методов | Ключевые для бота |
|---|---|---|
| `PlayerManager` | 10 | `CreatePlayer`, `DeletePlayer`, `GetPlayers`, `GetPlayerByName` — **спавн и учёт ботов** |
| `ResourceManager` | 8 | `SearchForDataContainer`, `SearchForInstanceByGuid` — **доступ к EBX-данным** |
| `NetEvents` | 7 | `SendToLocal`, `SendLocal`, `BroadcastLocal`, `Subscribe` |
| `SharedUtils` | 6 | `GetTime`, `GetTimeNS`, `GetTickrate`, `GetLevelName`, `GetCurrentGameMode` |
| `RaycastManager` | 3 | `CollisionRaycast`, `Raycast`, `DetailedRaycast` — **попадания и видимость** |
| `MathUtils` | 3 | `GetRandomInt`, `GetRandom`, `FNVHash` |
| `TicketManager` | 3 | `GetCurrentRound`, `GetRoundCount`, `GetTicketCount` |
| `ChatManager` | 2 | `SendMessage`, `Yell` — реплики бота |
| `EntityManager` | 2 | `GetIterator`, `CreateEntitiesFromBlueprint` |
| `Events` / `Hooks` | 2 | `Subscribe`, `Dispatch`, `Install` |
| `TeamSquadManager` | 1 | `GetSquadPlayerCount` |
| `Vec3` / `Quat` / `LinearTransform` | — | геометрия |

Полный список с числом файлов: `LegacyBF3\funbots-api-usage.md`.

---

## 2. Что уже есть у нас

| Что | Статус |
|---|---|
| **Механизм перехвата** (8 байт, трамплин, атомарно под LOCK, `FlushInstructionCache`) | ✅ готов и проверен самопроверкой без BF3 |
| Адреса 4 функций движка | ✅ есть из botlayer, но это **проверочные**, не наша поверхность |
| 12 из 26 событий | ⚠️ покрыты хуками botlayer — попутно полезно, но список неполный |
| Остальные ~14 событий и 13 хуков VEXT | ❌ адреса функций движка **не найдены** |

### Почему хуки botlayer частично полезны

12 из 26 событий проходят через те функции, которые он уже хукает —
значит, часть его адресов совпадает с нужными нам. Это экономит работу, но
**не повод считать его список нашим**: он подобран под спавн и ИИ нативных
ботов, а нам нужен полный набор для скриптов.

---

## 3. Порядок работы по поверхности

1. **Слой Lua без перехватов.** `lua.dll 5.4.2`, загрузчик модов, свой
   `require`, окружение VEXT, SOL3-биндинги. Работает и проверяется
   **без BF3** — это основной объём.
2. **Найти функции движка под 13 хуков VEXT.** Приём: смотреть, что
   `veniceext` пишет в свои таблицы нативов `0x18E74Cxx..0x18E753xx`, и
   сопоставлять с именами методов из RTTI (уже есть инвентарь типов).
3. **Найти функции под 26 событий.** Те же точки плюс места, откуда VU
   рассылает события.
4. **Подтвердить на живом клиенте.**

Пока пункты 2 и 3 не сделаны, `legacyext.dll` грузит моды и даёт Lua-слой,
но игровые события не приходят. Это нормальный порядок: так же устроен и
настоящий `veniceext.dll` — сначала VM, потом привязка к движку.

---

## 4. Правило проекта

**Не брать поверхность из botlayer.** Брать:

- **механизм** — да, переиспользуем (это не боти);
- **адреса** — только если попутно совпадают с нашим списком;
- **список событий и API** — из `funbots-api-usage.md` и `vext-inventory.md`,
  то есть из того, что реально требуют Lua-моды.

Источник истины по именам полей Blaze — `BLASE-FIELDS.md`.
Источник истины по API VEXT — `LegacyBF3\vext-inventory.md`.
Источник истины по потребностям ботов — `LegacyBF3\funbots-api-usage.md`.