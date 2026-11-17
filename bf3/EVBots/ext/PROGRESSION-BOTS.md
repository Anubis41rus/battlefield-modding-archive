# PROGRESSION-BOTS.md — прокачка и спавн ботов, разбор по данным

Дата: 11.10.2026. Все утверждения — со ссылкой на файл:строку. Где не проверено —
так и написано.

---

## 1. Гипотеза

> «Отсутствие спавна бота может быть связано с прокачкой; можно ли боту по
> умолчанию дать доступ ко всему; в VU функции прокачки нет».

---

## 2. Что в данных EBX — привязка к прокачке ЕСТЬ

Данные: `C:\Games\Legacy\Venice-EBX` (текстовый дамп, 73 476 файлов).

### 2.1 Реальные примеры (3 из них)

**Пример 1 — `Persistence\Unlocks\Soldiers\Specializations\SprintBoostL1.txt:8-19`**
(`ValueUnlockAsset`, класс `UnlockAssetBase`):

```
ValueUnlockAsset 350432B9-5B42-49F4-8343-CD20DE1B82BD #primary instance
    $::UnlockAssetBase
        Name Persistence/Unlocks/Soldiers/Specializations/SprintBoostL1
        UnlockUserData *nullGuid*
        DebugUnlockId SprintBoostL1
        Identifier 3981044622 (Persistence/Unlocks/Soldiers/Specializations/SprintBoostL1)
        UnlockScore 0
        AutoAvailable False
        NextLevelUnlockAsset Persistence/Unlocks/Soldiers/Specializations/SprintBoostL2/B8CEA0C3-...
        HiddenInProgression False
        AvailableForPlayer UnlockAvailability_All
    UnlockValues::array
```

Точные имена полей в данных EBX: `UnlockScore`, `AutoAvailable`,
`NextLevelUnlockAsset`, `HiddenInProgression`, `AvailableForPlayer`,
`UnlockUserData`, `DebugUnlockId`, `Identifier`.

**Пример 2 — `Persistence\GateList\MainWeaponGateList12.txt:2-18`**
(`CriteriaGateList`, класс `Asset`):

```
CriteriaGateList D51D1424-7F82-49DB-8852-DDE2E4B2BC78 #primary instance
    Name Persistence/GateList/MainWeaponGateList12
    CriteriaGates::array
        member(0) 10.0
        member(1) 20.0
        ...
        member(11) 150.0
```

Это и есть «лестница рангов» в данных: список порогов очков, при которых
открываются разблокировки. В каталоге 20 файлов `Persistence\GateList\`
(`MainWeaponGateList12..19`, `PistolGateList01`, `ServiceStarGateList2/6`,
`DogTagGateList1`, `AdvancedDogtagGateList{,3}`, `CoopGateList5`).

**Пример 3 — `Persistence\PlayerTypeProfiles\MPProfile.txt:2-68`**
(`PlayerTypeProfile`) — корень всей системы:

```
PlayerTypeProfile DD8FFE39-E225-AB81-C69C-DDB788EE7BB1 #primary instance
    Name Persistence/PlayerTypeProfiles/MPProfile
    Values Persistence/ValueTemplates/MultiplayerTemplate/BCCD37D5-...
    Awards Persistence/Awards/Awards/4D628453-...
    RankParams Persistence/Ranks/RankParams/721343AA-490E-4040-9E08-2ADDF3A62CB5
    Scoring Persistence/Scoring/Scoring/54858295-5A19-4540-BF11-BBAEC6FC2372
    DogTags Persistence/DogTags/DogTags/F5485B0F-...
    EloParams::EloParameters (KWinner/KLoser/KNewbie/KCompetitor/Expected)
    StaticUnlocks Persistence/Unlocks/MPStaticUnlocks/72AC8FD8-F777-CB1D-BA50-D84BFC90C476
    OrderedUnlockNames::array
        member(0) U_SteyrAug_Rifle_Scope
        ... (1432 записи, до member(1395) в этом файле)
```

`OrderedUnlockNames` — **упорядоченный** список разблокировок; порядок в массиве
= порядок открытия. Это и есть «прокачка» в данных.

**Пример 4 (бонус) — `Persistence\Unlocks\MPStaticUnlocks.txt:2-30`**
(`StaticUnlockList`) — то, что доступно всем без очков:

```
StaticUnlockList 72AC8FD8-F777-CB1D-BA50-D84BFC90C476 #primary instance
    Name Persistence/Unlocks/MPStaticUnlocks
    UnlockInfos::array
        member(0)::BasicUnlockInfo
            UnlockGuid D0E124FB-7116-4FBB-AF00-D8994AEB548D
            Identifier 2182442014 (Weapons/Type88/U_Type88)
            UnlockScore 0
            Licenses::array
                member(0) pwp
            AdditionalLicenses *nullArray*
            StringId U_Type88
```

Всего в `Persistence\Unlocks\` — **1433 файла** (подтверждено подсчётом).

### 2.2 Ключевая находка: прогрессия в данных УЖЕ выключена

Полный подсчёт по всем 1433 файлам `Persistence\Unlocks\`:

| Поле | Значение | Сколько |
|---|---|---|
| `AvailableForPlayer` | `UnlockAvailability_All` | **1431 из 1431** |
| `AutoAvailable` | `False` | 1386 |
| `AutoAvailable` | `True` | 45 |
| `UnlockScore` | `0` | 1422 |
| `UnlockScore` | `200` | 178 |
| `UnlockScore` | `1` | 1 |

**Ни одного** asset с `AvailableForPlayer`, отличным от `UnlockAvailability_All`.
То есть данные BF3 **уже сконфигурированы так, что разблокировано всё для
всех** — ровно то, что нужно боту, и ровно то, что описывает VU в документации
(раздел 5 ниже). Гипотеза «надо дать боту доступ ко всему» в данных уже
выполнена; **сам прокачечный механизм в данных выключен** (`UnlockAvailability_All`).

`AutoAvailable False` у большинства означает «нужен явный вызов
`SelectUnlockAssets`», а не «закрыто навсегда».

---

## 3. Тип инстанса солдата — `SoldierBlueprint`

`C:\Games\Legacy\Venice-EBX\Characters\Soldiers\MpSoldier.txt:2875`:

```
SoldierBlueprint 261E43BF-259B-41D2-BF3B-9AE4DDA96AD2 #primary instance
    $::CharacterBlueprint
        $::ObjectBlueprint
            $::Blueprint
                $::EntityBusData
                    $::DataBusData
                        $::Asset
                            $::DataContainer
                            Name Characters/Soldiers/MpSoldier
```

**GUID подтверждён: `261E43BF-259B-41D2-BF3B-9AE4DDA96AD2`.** Коллега называл
это «вторичным инстансом» — по факту это **#primary instance** (пометка
`#primary instance` стоит в той же строке), т.е. именованный. Partition этого
файла: `MpSoldier.txt:1` → `Partition F256E142-C9D8-4BFE-985B-3960B9E9D189`.

### 3.1 Есть ли у SoldierBlueprint поля прогрессии

**Нет.** В `MpSoldier.txt` (2000+ строк) полей `Unlock*`, `Rank`, `Level`,
`Required*` не встречается. Прогрессия в этом файле косвенная, ровно две ссылки:

- `MpSoldier.txt:805-815` — `CustomizeSoldierEntityData EA16D13E-...`,
  `Realm Realm_Server`, поле `CustomizeSoldierData characters/Soldiers/Customizations/MP_SelectPistolCustomization/B407182A-1C98-13DE-49A3-EE7F7EADFB4D`.
  То есть **шаблон солдата сам запрашивает кастомизацию при спавне** — серверный
  realm. Это прямая связь «шаблон → комплект».
- `MpSoldier.txt:492-520` — `CharacterCustomizationComponentData 2F95B057-...`,
  поля `SocketList Characters/Soldiers/SoldierSocketList_MP/8F1A9F10-...` и
  `SharedLodGroup LodGroups/Character/4732859E-...`.

Целевой файл кастомизации `Characters\Soldiers\Customizations\MP_SelectPistolCustomization.txt`:

```
CustomizeSoldierData B407182A-1C98-13DE-49A3-EE7F7EADFB4D #primary instance
    $::CustomizeCharacterData
        Name characters/Soldiers/Customizations/MP_SelectPistolCustomization
        RestoreToOriginalVisualState False
        ClearVisualState False
        VisualGroups *nullArray*
        Unlocks *nullArray*
        OverrideMaxHealth -1.0
        OverrideCriticalHealthThreshold -1.0
    Weapons *nullArray*
    ActiveSlot WeaponSlot_1
    RemoveAllExistingWeapons False
```

**Обратить внимание:** `Unlocks *nullArray*` и `Weapons *nullArray*` — в шаблоне
по умолчанию **пусто**. Комплект формируется в рантайме через `SelectUnlockAssets`
и `ApplyCustomization`, а не хранится в шаблоне.

### 3.2 Индексируется ли он нашим `SearchForDataContainer`

**Да, проверено на бинарном индексе** `C:\Games\Legacy\LegacyBF3\activation\ebx_index.bin`
(19 163 336 байт) поиском ASCII-строк:

| Строка в индексе | Наличие |
|---|---|
| `Characters/Soldiers/MpSoldier` | **True** |
| `Gameplay/Kits/USAssault` | **True** |
| `Persistence/Unlocks/MPStaticUnlocks` | **True** |
| `Persistence/PlayerTypeProfiles/MPProfile` | **True** |
| `Persistence/GateList/MainWeaponGateList12` | **True** |

Механика индекса подтверждена в `legacyext\tools\build_ebx_index.py:86-89`:

```python
rx_instance = re.compile(
    rb"(?m)^([A-Za-z_]\w+) ([0-9A-Fa-f]{8}-...)")
rx_name = re.compile(rb"(?m)^\t+Name[ \t]*(\S*)")
```

`rx_instance` матчит `SoldierBlueprint 261E43BF-259B-...` (тип + GUID),
`rx_name` матчит `\t\t\tName Characters/Soldiers/MpSoldier`. Т.е. именованный
инстанс есть в индексе и по пути, и по GUID.

### 3.3 Комплект (кит) — откуда берётся

`Gameplay\Kits\RUAssault.txt` — `VeniceSoldierCustomizationAsset 28EC16E7-0BBF-4CB0-9321-473C6EC54125 #primary instance`
(строка 156), класс `SoldierCustomizationAsset` → `CharacterCustomizationAsset`
→ `Asset` → `DataContainer`. Поля:

```
VisualTable D5830061-D49E-4B63-BF2D-CAFD263FE20A
SpecializationTable FDB66F32-B036-4104-AB0F-0E90A3C56950
WeaponTable 37E834B4-D47C-4675-B89F-DFAEFE793533
```

А `RUAssault.txt:4-155` — 10 вложенных `CustomizationTable` (`UnlockParts::array`)
→ `CustomizationUnlockParts` с полями `UICategorySid` и `SelectableUnlocks::array`.
Категории: `ID_M_SOLDIER_PRIMARY` (28 оружий), `ID_M_SOLDIER_SECONDARY` (18),
`ID_M_SOLDIER_SPECIALIZATION` (15 — включая `Persistence/Unlocks/Soldiers/Specializations/SprintBoostL1/350432B9-...`),
`ID_M_SOLDIER_GADGET1`, `ID_M_SOLDIER_GADGET2`, `GADGET1`.

**Это и есть «прокачка» с точки зрения данных:** каталог того, что вообще можно
выбрать. Поле `SelectableUnlocks` = «выбираемые разблокировки».

---

## 4. Различия игроков — таблица

### 4.1 Честная оговорка

Запрошенный дамп (`SpmProbe: [p+0x..] = 0x...`) **в текущем логе отсутствует**.
Проверено:

- `C:\Games\Legacy\legacyext\logs\legacyext.log` (443 648 байт, единственный
  лог в `legacyext\logs\`) — 0 вхождений `SpmProbe`, 0 вхождений `SAMPLE`.
  Это старый тестовый прогон от 10.10 16:08–19:16, до моста.
- `C:\Games\Legacy\LegacyBF3\logs\legacyext.log` — 293 строки, 1 вхождение
  `SpmProbe` (строка 24, только `hook installed`), дампов нет.

Единственный полный дамп поля в проекте — цитата в `C:\Games\ext\SPAWN-RESEARCH.md:59-66`
(снят с живого игрока `KOTuk1`, `p=F2DDD020`). Дампа бота в файлах проекта
**нет** — сравнивать нечего. Это надо снять отдельным прогоном (раздел 7).

### 4.2 Что известно из живого игрока и из наших вызовов

Источники: `SPAWN-RESEARCH.md:59-66` (живой игрок), `SPAWN-RESEARCH.md:247-249`
(аргументы при входе), `SPAWN-RESEARCH.md:548-550` (состояние бота на прогоне
15:38), `MODLOG.md:3167+` (мост, 19:33).

| Смещение | Живой игрок (KOTuk1) | Бот (наш) | Комментарий |
|---|---|---|---|
| `+0x00` | `0x025C778C` (vtable ServerPlayer) | создаётся движком | совпадает по построению |
| `+0x10` | `0xF2CBAA04` → указатель на имя | есть (`addToArray` с name) | — |
| `+0x28` | `0x75544F4B` = `"KOTu"` (имя инлайном) | н/д | — |
| `+0x31C` | команда | `1` (уже совпадала, `SPAWN-RESEARCH.md:545`) | **не блокер** |
| `+0x318` | флаг AI | `1` (`createPlayer` ставит сам, `:552`) | **не блокер** |
| `+0x3BC` | `1` | н/д | — |
| `+0x3C0` | `0xF2DD3570` — **указатель на солдата** | `0` | **это и есть «нет тела»** |
| `+0x3C4` | ненулевое («есть солдат») | `0` | **симптом, не причина** |
| `+0x400` | `0xF2DDD420` | н/д | — |
| `+0x404` | `0xF2DDD420` (= `+0x400`) | н/д | — |
| `+0x418` | `0xF2D16C60` | н/д | — |
| `+0x41C` | `0xF2C40500` | н/д | — |
| `+0x480` | н/д (не снят) | н/д | **кандидат №1, см. 4.3** |

### 4.3 ★ Главный кандидат: `player+0x480` (прочитан в `re\spawn_template.txt`)

`FUN_00e59340` (RVA `0x00A59340`, 5829 байт) — функция шаблона спавна.
Единственное место, где она читает поле игрока, которое **не** команда и **не**
флаг AI, — `player+0x480`:

`C:\Games\re\spawn_template.txt`, тело функции, два вхождения:

```c
// ~строка 1466 (индекс 1453+13)
if ((param_1 == 0.0) || (*(uint *)((int)param_1 + 0x480) == 0)) {
    // ветка «у игрока НЕТ своего комплекта»:
    puVar17 = (uint *)in_ECX[5];
    piVar28  = (int *)in_ECX[4];
    ...
} else {
    // ветка «у игрока ЕСТЬ свой комплект»:
    puVar17   = (uint *)0x0;
    uStack_1364 = *(uint *)((int)param_1 + 0x480);
    iVar8 = FUN_00dd4470(*(undefined4 *)((int)param_1 + 0x31c));
}
```

Прочитанные по игроку поля функции, полностью:

| Поле | Где | Назначение |
|---|---|---|
| `player+0x318` | условие выше | флаг «это AI» (уже 1) |
| `player+0x31C` | `FUN_00e57a50/FUN_00e41cd0/FUN_00dd4470` | команда (уже совпадает) |
| **`player+0x480`** | **условие выше** | **указатель на комплект/кастомизацию — НЕ ПРОВЕРЕНО, что он у бота NULL** |

Прочие чтения `in_ECX + 0x10/0x20/0x58/0x78/0x94/0x98/0xA4/0x8C/0xD19/0x329` —
это поля **точки спавна** (`in_ECX` = `this`), не игрока.

`FUN_00e59340` имеет **два ранних `return 0;`** (в `spawn_template.txt` индексы
~301 и ~1359) и множество `return uVar7`. Условие раннего возврата `0`:

```c
// индекс ~1352
if ((!bVar4) && (cVar5 == '\0')) {
    ...
    return 0;
}
```

где `bVar4`/`cVar5` — флаги, вычисленные выше по результатам
`FUN_004afe30(&DAT_02b47ac0/2/4/…)` и `FUN_00e57a50(...)`. **Какая именно из
проверок даёт `bVar4==0 && cVar5==0` — не проверено** (нужен разбор верхней
половины функции).

### 4.4 Вывод по таблице

Среди различий **нет ни одного поля, похожего на прогресс/статистику/
разблокировки**. Отличия — только указатели на сущности (солдат, имена) и
счётчики. Единственный содержательный кандидат — `+0x480`.

---

## 5. Что в VU — коллега ПРАВ

### 5.1 Прямое подтверждение цитатой

`C:\Games\Legacy\VU-Docs\content\troubleshooting\_index.md:65-67`, раздел
«Are my Battlelog stats transferred over?»:

> Your VU profile is independent from your Battlelog profile.
> **There's no progression and everything is unlocked for everyone by default.**

То есть: **прокачки в VU нет, и всё разблокировано у всех по умолчанию.**
Коллега прав, подтверждено первоисточником.

### 5.2 Проверка на нашей документации

`C:\Games\Legacy\LegacyBF3\VU-HOWITWORKS.md:158-182`, раздел 4:

- `:160-168` — описан тот же механизм Blaze: `getStatGroup("player_mpdefault2")`
  (определения) + `getStatsByGroupAsync(EID=…)` (значения).
- `:170-173` — «У нативных ботов botlayer идентификатора нет… клиент не знает
  ранг».
- `:181-182` — «В VU это сделано тем же способом — солдат есть и у бота,
  созданного через `PlayerManager:CreatePlayer(name, team, squad)`.»

Т.е. ранг в VU — это только **отображение** в таблице счёта (Blaze), а не
механизм, влияющий на спавн.

### 5.3 Как VU обходится без прокачки при спавне — механизм

Ключ: разблокировки передаются боту **явным вызовом**, а не «начислением очков».
Из документации VU `C:\Games\Legacy\VU-Docs\content\vext\ref\server\type\player.md`:

Методы (`player.md:62-63`, подробности `:367-385`):

```
SelectUnlockAssets(characterCustomizationAsset, visualUnlockAssets)
SelectUnlockAssets(characterCustomizationAsset, visualUnlockAssets, miscUnlockAssets)
CreateSoldier(soldierBlueprint, transform) -> SoldierEntity | nil
SpawnSoldierAt(soldier, transform, pose)
```

Свойства (`player.md:28-41`):

```
selectedKit      DataContainer | nil
customization    DataContainer | nil
selectedUnlocks  DataContainer{}
visualUnlocks    DataContainer{}
weapons          DataContainer[]
weaponUnlocks    DataContainer{}[]
```

**Механизм VU: (1) `SelectUnlockAssets` кладёт набор разблокировок в игрока,
(2) `CreateSoldier` создаёт солдата по шаблону с этим комплектом,
(3) `SpawnSoldierAt` сажает его на точку.**

Это в точности совпадает с тем, что делает fun-bots
(`BotSpawner.lua:1973/1975/1979/1981` → `BotManager.lua:905` → `:913`).
Ср. также `VU-Docs\content\vext\changelog.md:94` — в VU **специально добавили**
перегрузку `SelectUnlockAssets` с третьим аргументом `miscUnlockAssets`,
чтобы можно было передать больше разблокировок разом.

**Ответа VU на вопрос «как дать боту доступ ко всему»: никак не нужно — всё и так
разблокировано (`AvailableForPlayer UnlockAvailability_All` в данных + «everything
is unlocked for everyone by default» в документации). Нужно лишь передать
нужные ассеты через `SelectUnlockAssets`.**

---

## 6. Наш бэкенд — прогресс ЕСТЬ, но в спавне НЕ участвует

### 6.1 Хранит ли уровень/ранг/разблокировано

**Да, частично.** `C:\Games\Legacy\backend\src\Legacy.Backend\Data\ProfileStore.cs`:

- `:22-27` — `ProgressFile`: `schema`, `updatedUtc`, `stats: Dictionary<string,double>`.
- `:30-36` — `ProfileFile`: `schema`, `id` (personaref), `name`, `createdUtc`.
- `:267-293` — `GetProgress(personaref)` читает
  `profiles\<id>\progress.json`.
- `:295-309` — `SaveProgress`.
- `:315-326` — `CloneProgress(from, to)`: копирует набор стат.
- `:329-343` — `CreateBot(name, cloneFrom)`: создаёт солдата + копирует прогресс.

Формат на диске — `C:\Games\Legacy\LegacyBF3\profiles\234471775\progress.json`
(2863 байта): `{schema, updatedUtc, stats{...}}`, ключи вида `ACH36_01`,
`c___sa_g`, `c_wA__kwa_g`. Каталог — 1432 стата, группа `player_mpdefault2`
(`VU-HOWITWORKS.md:141-144`; подтверждено логом бэкенда: `stats catalog :
1432 stats, group 'player_mpdefault2'`).

То есть у бэкенда **есть счёт (score)**, из которого клиент рисует ранг.

### 6.2 Разблокировок бэкенд не хранит

В `ProfileStore.cs` полей вида unlock/licence/entitlement нет — только числовые
статы. Права/DLC — отдельная команда `listUserEntitlements2`
(`BlazeServices.cs:451-471`), отдаёт `Payloads.ProgressEntitlements` /
`ProgressEntitlementsLength`, прогресс — `QueryGeneral`.

### 6.3 Участвует ли бэкенд в спавне — НЕТ

`FUN_00e59340` (шаблон спавна) **не обращается ни к одному сетевому вызову**.
В `re\spawn_template.txt` все 50+ вызываемых функций — локальные
(`FUN_004ab22*`, `FUN_004d040*`, `FUN_0049090*`, `FUN_00532a6*` — это контейнеры,
строки, исключения). Ни `getStatsByGroupAsync`, ни `listUserEntitlements2`, ни
что-либо сетевое не встречается.

**Путь спавна: мод → Lua → нативный мост → движок. Бэкенд в нём не участвует
вообще.** Бэкенд нужен только чтобы у бота был **personaref** (для таблицы
счёта на экране) — это отдельная задача, не блокер тела.

---

## 7. Вердикт

### Гипотеза подтверждена ЧАСТИЧНО

| Часть гипотезы | Вердикт |
|---|---|
| «В VU функции прокачки нет» | **ПОДТВЕРЖДЕНО** — `troubleshooting\_index.md:67`, дословно |
| «Можно ли боту дать доступ ко всему» | **ДА, и это уже сделано в данных** — `AvailableForPlayer UnlockAvailability_All` у 1431 из 1431 ассетов |
| «Отсутствие спавна связано с прокачкой» | **ОПРОВЕРГНУТО как причина** — Blaze-прогресс в `FUN_00e59340` не читается вообще; бэкенд в пути спавна не участвует |
| «Отсутствие спавна связано с комплектом» | **подтверждено косвенно, но это ДРУГАЯ гипотеза** — кандидат `player+0x480` |

**Итог:** прокачка **не** является причиной отсутствия тела. Причина —
у игрока-бота нет **комплекта (кастомизации)**, и это поле `player+0x480`
в `FUN_00e59340` — единственный непрочитанный кандидат.

### Что конкретно надо сделать боту, чтобы получить «доступ ко всему»

Не абстракция, а конкретные вызовы — в порядке, который требует мод
(`fun-bots\ext\Server\BotManager.lua:901-914`, `BotSpawner.lua:1963-1983`):

**Шаг 1. Передать кит игроку.** Нужна функция, эквивалентная VU
`Player:SelectUnlockAssets(characterCustomizationAsset, visualUnlockAssets, miscUnlockAssets)`
(`VU-Docs\...\server\type\player.md:63, 378-385`).
Аргументы — ассеты **из нашего EBX-индекса, все проверены как присутствующие**:

| Аргумент | Источник | Проверено в индексе |
|---|---|---|
| `characterCustomizationAsset` | `Gameplay/Kits/<US\|RU><Assault\|Engineer\|Support\|Recon>` (`BotSpawner.lua:2186-2208` `_FindKit`) | `Gameplay/Kits/USAssault` → **True** |
| `visualUnlockAssets[1]` | `Persistence/Unlocks/Soldiers/Visual/MP/<Team>/MP_<Team>_<Kit>_Appearance01` (`BotSpawner.lua:2214-2231` `_FindAppearance`) | раздел `Persistence\Unlocks\Soldiers\Visual` — **не проверено**, требует проверки |
| `miscUnlockAssets` | `Persistence/Unlocks/MPStaticUnlocks/72AC8FD8-F777-CB1D-BA50-D84BFC90C476` | → **True** |

**Шаг 2. Собрать `CustomizeSoldierData` и применить его.**
Эквивалент VU `Soldier:ApplyCustomization(customization)` (упоминается в
`BotSpawner.lua:1557/1567`, поля типа — `VextPlayerManager.cpp:74`).
Структура — по образцу `Characters\Soldiers\Customizations\MP_SelectPistolCustomization.txt`:
поля `Unlocks::array`, `Weapons::array`, `ActiveSlot`, `RemoveAllExistingWeapons`.
Заполнять из `SelectableUnlocks::array` кит-таблицы
(`Gameplay\Kits\RUAssault.txt:24-52` и далее).

**Шаг 3. Создать солдата шаблоном.**
Эквивалент VU `Player:CreateSoldier(soldierBlueprint, transform)`
(`BotManager.lua:905`). Шаблон — **именованный** инстанс, доступен поиском по пути:

```
Characters/Soldiers/MpSoldier  →  SoldierBlueprint 261E43BF-259B-41D2-BF3B-9AE4DDA96AD2
(Venice-EBX\Characters\Soldiers\MpSoldier.txt:2875, раздел F256E142-C9D8-4BFE-985B-3960B9E9D189)
```

Это НЕ `FUN_00e59340` и НЕ `FUN_00ddbbd0`. Это отдельная пара функций,
которую в проекте ещё не нашли.

**Шаг 4. Посадить солдата.** `Player:SpawnSoldierAt(soldier, transform, pose)`
(`BotManager.lua:913`) — у нас это `NativeBridge.SpawnEnginePlayer`,
сейчас дёргающий `FUN_00ddbbd0` (RVA `0x009DBBD0`). Этот вызов верен по
сигнатуре, но он — последний в цепочке и без шагов 1–3 бесполезен.

**Упрощение:** раз прокачки нет и всё разблокировано, **шаг 1 можно свести к
одному вызову** `SelectUnlockAssets(кит, {внешность}, MPStaticUnlocks)`.
Мод это уже умеет (`BotSpawner.lua:1973`), когда `s_Unlocks` непуст.

### Порядок работ

1. Прочитать верхнюю половину `FUN_00e59340` и найти, что даёт
   `bVar4==0 && cVar5==0` → `return 0` (единственный ранний отказ).
2. Снять `player+0x480` у живого игрока и у бота. **Если у бота `0`, а у живого
   ненулевое — причина найдена точно.**
3. Найти `CreateSoldier` (парный к `SelectUnlockAssets`) в Ghidra — сейчас он
   не идентифицирован.
4. Сделать шаги 1–3 в порядке мода: `SelectUnlockAssets` → `CreateSoldier` →
   `SpawnSoldierAt`.

---

## 8. Как проверить — минимальный эксперимент

**Эксперимент (1 прогон, ~2 минуты работы).** Цель — проверить
`player+0x480`, единственный непрочитанный кандидат.

1. В `DumpPlayerSample` снять ещё один диапазон: смещения `0x470..0x4A0`
   (сейчас снимаются только `0x00..0x40` и `0x3B0..0x430`; `0x480` в
   `0x3B0..0x430` **не попадает** — это и есть причина, почему поле не видели).
   Формат тот же: `SpmProbe:   [p+0x480] = 0x........`.
2. Один и тот же дамп снять дважды:
   - для живого игрока (заходит клиент, ловится в `ServerPlayerManager::update`);
   - для бота (`NativeBridge.CreateEnginePlayer`, лог `19:33`).
3. Сравнить `+0x480`.

**Интерпретация:**

| Результат | Вывод |
|---|---|
| у живого ненулевое, у бота `0` | **Причина найдена.** Надо писать код, кладющий комплект в `player+0x480` (через `SelectUnlockAssets`/`ApplyCustomization`) |
| у обоих одинаково | `+0x480` не причина → копать `bVar4`/`cVar5` в верхней половине `FUN_00e59340` |
| у обоих `0` | отказ раньше, читать верхнюю половину функции |

**Предупреждение (из опыта, `SPAWN-RESEARCH.md:653-659`, ИНЦИДЕНТ №6):**
движковые функции на **незнакомых** объектах роняют сервер. Поэтому в этом
эксперименте — **только чтение полей** через `Read()`, никаких вызовов
движка по адресам, найденным перебором. Спавн оборачивать в `__try/__except`.

**Второй (дешёвый, без прогона) эксперимент.** Проверить наличие раздела визуалок,
который я не проверил: существует ли
`Legacy\Venice-EBX\Persistence\Unlocks\Soldiers\Visual\MP\Us\...`? Если да —
все аргументы `SelectUnlockAssets` доступны из индекса, и шаг 1 реализуем
целиком на Lua+нативе без нового реверса.

---

## 9. Что НЕ проверено (честный список)

1. Дамп полей бота — **отсутствует в проекте**. Сравнительная таблица
   (раздел 4.2) собрана из разных источников, полного парного дампа нет.
2. Значение `player+0x480` у живого игрока и у бота — **не измерено**.
3. Что именно даёт `bVar4==0 && cVar5==0` → `return 0` в `FUN_00e59340` —
   **не прочитано** (верхняя половина функции).
4. Наличие `Persistence\Unlocks\Soldiers\Visual\MP\...` в индексе —
   **не проверено**.
5. Существует ли нативная функция, реализующая `SelectUnlockAssets` —
   **не искалась в Ghidra**.
6. `CreateSoldier` как отдельная движковая функция — **не идентифицирована**.
7. Утверждение «у всех 1431 ассетов `AvailableForPlayer UnlockAvailability_All`»
   проверено по `Persistence\Unlocks\` (1433 файла, 1431 вхождение поля).
   Раздел `Persistence\Awards\`, `Persistence\Ranks\`, `Persistence\GateList\`
   отдельно на этот счёт **не проверялся**.
8. Утверждение «бэкенд не участвует в спавне» — проверено **отсутствием
   сетевых вызовов в `FUN_00e59340`** и по коду бэкенда. Формальный
   перехват сетевого трафика на спавне **не делался**.