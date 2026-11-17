# WEAPONS-RESEARCH — оружие и снаряжение для Lua-ботов

Дата: 2026-10-11. Задача: разобраться, почему мод `fun-bots` при спавне не находит
оружие, и что нужно от движка, чтобы это заработало.

Область: только анализ. Ничего не компилировалось, сервер/клиент не запускались.

---

## 1. Что сейчас происходит

Все три нужные мод функции в проекте — заглушки:

| Функция | Файл проекта | Состояние |
| --- | --- | --- |
| `ResourceManager:SearchForDataContainer` | `legacyext\src\VextResourceManager.cpp:140` | всегда `return nil` |
| `ResourceManager:SearchForInstanceByGuid` | `legacyext\src\VextResourceManager.cpp:146` | всегда `return nil` |
| `Player:SpawnSoldierAt` | `legacyext\src\VextPlayerManager.cpp:140` | заглушка |
| `EntityManager:CreateEntitiesFromBlueprint` | — | **отсутствует полностью** (в `VextEntityManager.cpp` такого метода нет; grep по `legacyext\src` не находит ни `CreateEntitiesFromBlueprint`, ни `CreateFromBlueprint`) |
| `SoldierEntity:ApplyCustomization` | `legacyext\src\VextPlayerManager.cpp:76` | рабочая заглушка |
| `Player:SelectUnlockAssets` | `legacyext\src\VextPlayerManager.cpp:128` | заглушка (пустая) |

Цепочка отказа: `BotSpawner:GetResourceDataContainer()` (`BotSpawner.lua:43`) кэширует
результат в `self._ResourceDataContainer`; кэш сбрасывается на `OnLevelLoaded`
(`BotSpawner.lua:84`). Так как поиск всегда даёт `nil`, в `BotSpawner:_GetCustomization`
(`BotSpawner.lua:1999`) срабатывают шесть проверок `== nil` и пишутся шесть
`WARNING: Path not found: ...` — по одному на слот
(primary / gadget1 / gadget2 / grenade / pistol / knife).

Проверено по коду мода: `WeaponList.lua:625-641` (закомментированный отладочный
блок с тем же текстом «Path not found») и `BotSpawner.lua:1991` («Attachment invalid»).

Отдельно: в проекте **нет подсистемы DataContainer**. Поиск по `legacyext\src` не
находит ни `MakeWritable`, ни `typeInfo`, ни какого-либо регистра EBX-типов.
То есть даже если `SearchForDataContainer` начнёт что-то возвращать, мод тут же
упадёт на обращении к полям — см. раздел 2.

---

## 2. Что ожидает мод

### 2.1 Результат — типизированный DataContainer (Lua-userdata), не таблица

Прямое доказательство — `EbxEditUtils.lua:17-49`:

```lua
function EbxEditUtils:GetWritableInstance(p_ResourcePathOrGUIDOrContainer)
	if type(p_ResourcePathOrGUIDOrContainer) == 'userdata'
	   and p_ResourcePathOrGUIDOrContainer.typeInfo ~= nil then
		local s_ReturnInstance = _G[p_ResourcePathOrGUIDOrContainer.typeInfo.name](p_ResourcePathOrGUIDOrContainer)
		if s_ReturnInstance.MakeWritable ~= nil then
			s_ReturnInstance:MakeWritable()
		end
		return s_ReturnInstance, true
	end

	local s_Instance = ResourceManager:SearchForDataContainer(p_ResourcePathOrGUIDOrContainer)
	if s_Instance == nil then
		s_Instance = ResourceManager:SearchForInstanceByGuid(Guid(p_ResourcePathOrGUIDOrContainer))
	end
	if s_Instance == nil then
		s_Instance = ResourceManager:FindDatabasePartition(Guid(p_ResourcePathOrGUIDOrContainer))
	end
	if s_Instance == nil then
		return nil, 'Could not find Data Container, Instance, or Partition'
	end

	local s_WorkingInstance = _G[s_Instance.typeInfo.name](s_Instance)
	if s_WorkingInstance.MakeWritable ~= nil then
		s_WorkingInstance:MakeWritable()
	end
	return s_WorkingInstance, true
end
```

Из этого следуют конкретные требования к возвращаемому объекту:

1. Lua-тип `userdata` с полем `typeInfo`.
2. `typeInfo.name` — строка с именем класса (например `"SoldierWeaponUnlockAsset"`).
3. В `_G[<typeInfo.name>]` лежит конструктор-каст, принимающий этот userdata.
4. Метод `MakeWritable()` (в моде вызывается опционально, через проверку `~= nil`).
5. Индексация по полям: `s_WorkingInstance[s_PropertyName]` (`EbxEditUtils.lua:66`,
   `:112`), в том числе по вложенным контейнерам — там же делается «safety cast»
   `_G[s_WorkingInstance.typeInfo.name](s_WorkingInstance)` (`EbxEditUtils.lua:70`),
   то есть `typeInfo` должен быть доступен и у разыменованных вложенных объектов.
6. `type(p_Instance)` для скаляров — `string`/`number`/`boolean`/`nil`
   (`EbxEditUtils.lua:101-104`), так что Lua-значения внутри контейнера должны быть
   настоящими Lua-скалярами, а не обёртками.

### 2.2 Результат кастуется в конкретные типы

| Место в моде | Код | Требуемый тип |
| --- | --- | --- |
| `BotSpawner.lua:2027` | `s_PrimaryWeapon.weapon = SoldierWeaponUnlockAsset(s_PrimaryWeaponResource)` | `SoldierWeaponUnlockAsset` |
| `BotSpawner.lua:2047, 2060, 2075, 2089, 2103` | то же для gadget1 / gadget2 / grenade / pistol / knife | `SoldierWeaponUnlockAsset` |
| `BotSpawner.lua:1994` | `p_UnlockWeapon.unlockAssets:add(UnlockAsset(s_Asset))` | `UnlockAsset` (вложения) |
| `WeaponList.lua:630, 637` | `SoldierWeaponUnlockAsset(...)`, `UnlockAsset(...)` | то же (в отключённом debug-блоке) |

Все шесть «боевых» кастов — это слоты солдата. Ни один из них не проходит проверку
типа: если в `_G` лежит заглушка, подставленная «наугад», мод не заметит подмены —
это отдельный риск, см. «Открытые вопросы».

### 2.3 Куда результат передаётся дальше

```lua
-- BotSpawner.lua:1545 — комплект солдата
p_Bot.m_Player.selectedKit = self:GetResourceDataContainer('Characters/Soldiers/MpSoldier')

-- BotSpawner.lua:1973 / 1979 / 1981 — три DataContainer'а разных типов
p_Bot.m_Player:SelectUnlockAssets(s_SoldierKit, { s_Appearance }, s_Unlocks)

-- BotSpawner.lua:1557 / 1567 — итоговая кастомизация
p_Bot.m_Player.soldier:ApplyCustomization(self:_GetCustomization(p_Bot, s_BotKit))
```

`_FindAppearance` (`BotSpawner.lua:2214`) имеет аннотацию `---@return DataContainer|nil`,
`_FindKit` (`BotSpawner.lua:2186`) — тоже `DataContainer|nil`. То есть мод считает,
что по пути `Gameplay/Kits/USAssault` приходит «таблица комплекта», по пути
`Persistence/Unlocks/Soldiers/Visual/MP/Us/MP_US_Assault_Appearance01` — «внешность»,
а перки (`_GetUnlocks`, `BotSpawner.lua:1901/1905`) — «перки».

### 2.4 Результат уходит в создание сущностей

`Server\__init__.lua:666-686` — единственное место, где ресурс используется не как
описание, а как чертёж:

```lua
local resource = ResourceManager:SearchForDataContainer('Weapons/M67/M67_Projectile')
if not resource then return end

local creationParams = EntityCreationParams()
creationParams.transform = LinearTransform()
creationParams.networked = true
creationParams.transform.trans = p_Position

local createdBus = EntityManager:CreateEntitiesFromBlueprint(resource, creationParams)
if createdBus == nil then return end
for _, entity in pairs(createdBus.entities) do
	entity:Init(Realm.Realm_ClientAndServer, true)
end
```

Здесь важно: функция называется `EntityManager:CreateEntitiesFromBlueprint`, а не
`EntityFactory:CreateFromBlueprint`. В самом моде `EntityFactory` встречается только
как **хук** `Hooks:Install('EntityFactory:Create', ...)` (`Server\__init__.lua:144`,
обработчик `:560`), который фильтрует `p_EntityData.typeInfo.name`. То есть
`EntityFactory:Create` — это не то, что создаёт сущности из найденного ресурса;
для этого нужна именно `EntityManager:CreateEntitiesFromBlueprint`
(она же документирована в `LegacyBF3\VEXT-API-MAP.md:161`).

### 2.5 Нужна автоматическая разыменовка ссылок (это главная сложность)

`WeaponClass:learnStatsValues()` (`WeaponClass.lua:33-70`) читает с оружия реальные
числовые характеристики для ИИ, проходя по вложенным контейнерам:

```lua
s_Blueprint, s_Success = g_EbxEditUtils:GetWritableInstance(self:getResourcePath())
s_FireData,  s_Success = g_EbxEditUtils:GetWritableContainer(s_Blueprint, 'Weapon.Object.WeaponFiring.PrimaryFire')
s_AiData,    s_Success = g_EbxEditUtils:GetWritableContainer(s_Blueprint, 'Weapon.Object.aiData')
s_BulletData,s_Success = g_EbxEditUtils:GetWritableContainer(s_FireData,  'shot.ProjectileData')
```

Путь начинается с `Weapon` — поля типа «ссылка на инстанс». Формат ссылки в EBX:
`Weapons/M67/M67/C569F719-AB04-7BB0-2C08-B906DBC9FD3A` (проверено в
`Venice-EBX\Weapons\M67\U_M67.txt:15`) — это `путь/раздела` + `/` + `GUID инстанса`.
Значит при обращении к `s_Blueprint.Weapon` подсистема должна **сама** найти раздел
`Weapons/M67/M67` и внутри него инстанс с этим GUID и вернуть его как DataContainer.
Без этого `learnStatsValues` вернёт «No blueprint for: <имя>» для каждого ствола —
и типы оружия (`WeaponTypes.LMG/PDW/Sniper/...`) останутся незаполненными
(`WeaponTypes.None`), а ИИ-бот будет стрелять наугад.

### 2.6 Пути, которые мод спрашивает (всего)

Проверено скриптом по `WeaponList.lua`: **125 уникальных литералов вида
`Weapons/...` — все 125 присутствуют в `Venice-EBX`.** Отсутствующих нет.

Дополнительно мод строит пути в runtime:

| Шаблон | Примеры (проверено) |
| --- | --- |
| `Characters/Soldiers/MpSoldier` | существует |
| `Gameplay/Kits/<TEAM><Kit><suffix>` | 20 файлов существуют, см. п. 4.3 |
| `Persistence/Unlocks/Soldiers/Visual/MP/<Us\|Ru>/MP_<T>_<Kit>_Appearance01` | все 8 существуют |
| `persistence/unlocks/soldiers/specializations/<X>` (7 перков) | существуют |
| `persistence/unlocks/vehicles/<X>` (≈23 перка) | существуют |

Плюс два обращения по GUID:
`ResourceManager:SearchForInstanceByGuid(Guid('261E43BF-259B-41D2-BF3B-9AE4DDA96AD2'))`
(`BotSpawner.lua:935`, комментарий в моде: `-- SoldierBlueprint`) и два GUID реестров
уровней в `Shared\__init__.lua:87/90` (через `RegistryContainer(...)` + `AddRegistry`).

---

## 3. Что есть в данных EBX (`Venice-EBX`)

### 3.1 Формат выгрузки — установлено по 2916 файлам каталога `Weapons\`

Формат файла:

```
Partition <GUID раздела>
<Тип> <GUID инстанса>            ← инстанс №1, не обязательно нужный
	...
<Тип> <GUID> #primary instance   ← «именованный» инстанс раздела
	$::DataContainer
	Name Weapons/Knife/U_Knife
	...
```

Факты, проверенные скриптом по всему `Weapons\`:

* в каждом из **2916** файлов есть ровно один маркер `#primary instance`;
* у именованного инстанса поле `Name` совпадает с путём файла без `.txt`.

Примеры:

| Файл | `#primary instance` | `Name` |
| --- | --- | --- |
| `Weapons\Knife\U_Knife.txt` | `SoldierWeaponUnlockAsset 8963F500-…` | `Weapons/Knife/U_Knife` |
| `Weapons\M67\U_M67.txt` | `SoldierWeaponUnlockAsset 9F789F05-…` | `Weapons/M67/U_M67` |
| `Weapons\M67\M67_Projectile.txt` | `ProjectileBlueprint F39ED0C9-…` | `Weapons/M67/M67_Projectile` |
| `Weapons\M416\U_M416.txt` | `SoldierWeaponUnlockAsset 3A6B6A16-…` | `Weapons/M416/U_M416` |
| `Weapons\M416\U_M416_Kobra.txt` | `UnlockAsset 126D58B3-…` | `Weapons/M416/U_M416_Kobra` |
| `Characters\Soldiers\MpSoldier.txt` | `SoldierBlueprint 261E43BF-…` | (GUID из п. 2.6) |
| `Gameplay\Kits\USAssault.txt` | `VeniceSoldierCustomizationAsset A15EE431-…` | `Gameplay/Kits/USAssault` |
| `Persistence\Unlocks\Soldiers\Visual\MP\Us\MP_US_Assault_Appearance01.txt` | `UnlockAsset F2ECBAB2-…` | полный путь |
| `Persistence\Unlocks\Soldiers\Specializations\SprintBoostL2.txt` | `ValueUnlockAsset B8CEA0C3-…` | полный путь |

**Следствие:** поиск «по имени DataContainer» и поиск «по имени раздела» — это
разные вещи. Например, `Characters/Soldiers/MpSoldier` вернёт
`SoldierBlueprint` (а не `AIEntryData`, который стоит первым в файле), и это
ровно то, что помечает мод как `-- SoldierBlueprint`. Поиск по GUID
`261E43BF-…` должен вернуть **тот же самый** объект.

### 3.2 По одним и тем же путям бывают разные типы инстансов

Гистограмма по 2916 файлам `Weapons\` (тип `#primary instance`):

| Count | Type |
| ---: | --- |
| 1238 | `UnlockAsset` |
| 296 | `GunSwayModifierData` |
| 292 | `TextureAsset` |
| 270 | `SkinnedMeshAsset` |
| 190 | `SoldierWeaponUnlockAsset` |
| 166 | `RigidMeshAsset` |
| 105 | `SoldierWeaponBlueprint` |
| 88 | `GameDataContainerAsset` |
| 64 | `VeniceSoldierWeaponCustomizationAsset` |
| 61 | `DataContainerAsset` |
| 40 | `ShaderGraph` |
| 38 | `ObjectVariation` |
| 22 | `ProjectileBlueprint` |
| 9 | `WeaponFiringDataAsset` |
| 8 | `SoldierWeaponMultiUnlock` |

Все они лежат в одном пространстве имён `Weapons/...`. Конкретный пример
разнобояя внутри одного каталога `Venice-EBX\Weapons\M416\`:

| Путь | Тип инстанса |
| --- | --- |
| `Weapons/M416/U_M416` | `SoldierWeaponUnlockAsset` (ствол — то, что бот вешает в слот 0) |
| `Weapons/M416/U_M416_Kobra` | `UnlockAsset` (прицел — вложение, идёт в `unlockAssets:add`) |
| `Weapons/M416/U_M416_CAMO_1` | `BlueprintAndVariationPair` |
| `Weapons/M416/U_M416_M26_Frag` | `SoldierWeaponUnlockAsset` (ручной гранатётом — отдельный ствол) |
| `Weapons/M416/M416_MultiUnlock` | `SoldierWeaponMultiUnlock` |
| `Weapons/M416/M416_Customization` | `CustomizationTable` |
| `Weapons/M416/M416_Bipod` | `GunSwayModifierData` |
| `Weapons/M416/M416_D` | `TextureAsset` |
| `Weapons/M416/m416_1p_Mesh` | `MeshMaterial` |

Дополнительно вне `Weapons\`: `Weapons/M67/M67_Projectile` → `ProjectileBlueprint`
(при этом первым инстансом в том же файле идёт `GrenadeEntityData` — корень чертежа).

Вывод: реализация `SearchForDataContainer` обязана быть **типизированной** — она
обязана вернуть «тот самый» инстанс и корректное имя класса, потому что мод
в `_G[typeInfo.name](...)` полагается на имя класса, выведенное из данных, а не
угадывает его.

### 3.3 Ссылки между инстансами разрешаются по «путь/раздела» + GUID

Проверено:

```
Weapons\M67\U_M67.txt:15         Weapon Weapons/M67/M67/C569F719-AB04-7BB0-2C08-B906DBC9FD3A
Weapons\M67\M67.txt:153          SoldierWeaponBlueprint C569F719-AB04-7BB0-2C08-B906DBC9FD3A #primary instance
```

То есть поле `Weapon` у `SoldierWeaponUnlockAsset` — это ссылка на **именованный
инстанс другого раздела**, и именно он имеет тип `SoldierWeaponBlueprint`, который
мод и объявляет в аннотации `---@type SoldierWeaponBlueprint`
(`WeaponClass.lua:35`). Это подтверждает требование п. 2.5: разыменовка ссылок
обязательна, иначе ИИ не получит ТТХ.

### 3.4 Что типы значат для мода

* `SoldierWeaponUnlockAsset` — ровно то, что мод кастует в слоты
  (`BotSpawner.lua:2027…2103`). Тип данных в EBX совпадает с ожиданием мода.
* `UnlockAsset` — ровно то, что мод кастует в `unlockAssets:add` для прицелов
  и глушителей (`BotSpawner.lua:1994`). Тип данных совпадает.
* `ValueUnlockAsset` (`Persistence/Unlocks/.../SprintBoostL2`) — то, что мод
  кладёт в третий аргумент `SelectUnlockAssets` как «перк». Тип совпадает.
* `VeniceSoldierCustomizationAsset` (`Gameplay/Kits/USAssault`) — то, что мод
  кладёт первым аргументом в `SelectUnlockAssets`. Тип совпадает.
* `ProjectileBlueprint` (`Weapons/M67/M67_Projectile`) — то, что мод передаёт в
  `CreateEntitiesFromBlueprint`. Тип совпадает.

Вывод: **мод написан правильно, типы в данных на месте, расходимости нет.**
Чинить нужно движковую сторону.

---

## 4. Какие функции движка нужны

Ни одна из адресов ниже не выдумана: в проекте их реализаций нет, в
`LegacyBF3\HOOK-TABLE.md` упоминаний `ResourceManager`/`EntityFactory` нет,
в `backend\BLASE-FIELDS.md` (это протокол Blaze, не DataContainer) — тоже нет.
`LegacyBF3\PHASE1-MAP.md:170` упоминает «ResourceManager-обёртки (8 методов)» только
как пункт 6 в списке работ, без деталей.

### 4.1 Индекс ресурсов «путь → DataContainer» — ФУНДАМЕНТ

**Что:** построить карту из ~73 тыс. разделов EBX: путь раздела, GUID раздела,
GUID именованного инстанса, тип и GUID инстанса, класс, поля.
Дополнительно: карта GUID → инстанс (для `SearchForInstanceByGuid` и для разыменовки
ссылок вида `путь/GUID`).

**Сложность: высокая, но механическая.** Данные уже в `Venice-EBX` (73k файлов,
1.4 ГБ текста). Нужен разбор формата `.txt`-дампа Venice и построение индекса.
Открытый вопрос — откуда брать эти данные в рантайме (см. «Открытые вопросы»).

### 4.2 `ResourceManager:SearchForDataContainer(name)` — средняя

Нужен только после индекса: путь (строка) → userdata инстанса.
Возвращать `nil` если путь не найден. Сложность низкая, вся работа — в 4.1.

**Деталь про регистр:** мод для перков передаёт пути в нижнем регистре
(`persistence/unlocks/soldiers/specializations/sprintboostl2`), а в данных они
записаны с заглавных (`Persistence/Unlocks/Soldiers/Specializations/SprintBoostL2`).
На NTFS это один и тот же файл, но для индекса в памяти регистр значим. Проверить,
приводит ли реальный движок регистр к одному виду, не удалось. Безопасное решение —
строить индекс в нижнем регистре; риск коллизий нужно проверить.

### 4.3 `ResourceManager:SearchForInstanceByGuid(guid)` — средняя

Нужен для двух мест:
* `BotSpawner.lua:935` — `261E43BF-…` (именованный инстанс `Characters/Soldiers/MpSoldier`);
* `Shared\__init__.lua:87/90` — реестры уровней (`RegistryContainer(...)` → `AddRegistry`).

Второй случай сложнее: результат кастуется в `RegistryContainer`, а потом у него
читается содержимое (`.instances` / `.partition`). Тип `RegistryContainer` в проекте
также отсутствует.

Побочно обнаружено (не связано с заглушками): мод строит `Gameplay/Kits/USEngi`
для инженера (`BotSpawner.lua:1938` передаёт `'Engi'` в `_FindKit`, который
конкатенирует как есть, `BotSpawner.lua:2199`), а в данных файл называется
`Gameplay/Kits/USEngineer.txt`. Проверено: `USEngi`, `USEngi_GM`, `USEngi_XP4`,
`USEngi_GM_XP4`, `USEngi_XP4_SCV` — **отсутствуют**; `USEngineer*` — присутствуют.
То есть даже при идеальном `SearchForDataContainer` инженер не получит комплект.
Похоже на ошибку в моде, но не проверено, как это работает в оригинальном VU.

### 4.4 Lua-биндинг DataContainer — высокая, обязательная

Без неё мод не пройдёт дальше первого каста. Нужно:

| Требование | Где проверяется модом |
| --- | --- |
| userdata с полем `typeInfo` | `EbxEditUtils.lua:18` |
| `typeInfo.name` (строка) | `EbxEditUtils.lua:19, 42, 70` |
| конструктор-каст в `_G[<имя>]` для каждого имени класса | `EbxEditUtils.lua:19, 42, 70` |
| метод `MakeWritable()` | `EbxEditUtils.lua:22, 45, 73, 106` |
| индексация `obj['Поле']` | `EbxEditUtils.lua:66, 112` |
| Lua-скаляры для `string`/`number`/`boolean`/`nil` | `EbxEditUtils.lua:101-104` |
| **авторазыменовка ссылок `путь/GUID`** | `WeaponClass.lua:51, 58, 65` |
| метод `Is(<ИмяТипа>')` у entity-данных | `BotSpawner.lua:977, 996` (для `EntityData`, не для DC) |

Сейчас в `legacyext\src` этого нет вообще (пусто по `MakeWritable`/`typeInfo`).

### 4.5 `EntityManager:CreateEntitiesFromBlueprint(data, params)` — высокая

Нужна для `Server\__init__.lua:677` (спавн гранаты по консольной команде).
Должна создавать `EntityBus` из `ProjectileBlueprint` и заполнять `bus.entities`.
Нужно также `Entity:Init(Realm.Realm_ClientAndServer, true)` — по коду мода
`VEXT-API-MAP.md:148` считается доступным.

Проверено: в `VextEntityManager.cpp` есть только `Entity:__call`, `Entity:GetData`,
`EntityIterator:Next`, `EntityManager:GetIterator`, `GetCount`,
`FindEntityByName`, `GetEntityName`. `EntityBus` как тип в проекте отсутствует —
мод обращается к `bus.entities` (`BotSpawner.lua:999, 1082, 1127, 1175, 1255`).

### 4.6 `Player:SelectUnlockAssets(kit, {appearance}, unlocks)` — средняя

Заглушка (`VextPlayerManager.cpp:128`). Это запись состояния выбора в игрока
(«какие разблокировки применены»), а не создание сущностей. Без неё бот получит
стандартный комплект, но apply-кастомизация из `_GetCustomization` всё равно
выполнится (см. 4.7).

### 4.7 `SoldierEntity:ApplyCustomization(customizationData)` — средняя/высокая

Заглушка (`VextPlayerManager.cpp:76`). Здесь `CustomizeSoldierData` уже есть в
`VextCustomization.cpp` (слоты, `weapons:add`, `activeSlot`,
`removeAllExistingWeapons`) — то есть Lua-структура собрана. Не хватает передачи
в движок: создание оружия по `SoldierWeaponUnlockAsset` в слотах 0–7 и применение
`unlockAssets` (прицелы/глушители/камуфляж). Это отдельная и довольно большая работа.

### 4.8 Что НЕ нужно для оружия

`Player:SpawnSoldierAt` (`VextPlayerManager.cpp:140`) — это про появление бота
в игре, не про снаряжение. По приоритету проекта (бот появляется раньше оружия)
это отдельная задача.

---

## 5. Рекомендуемый порядок работ

**Шаг 0. Не трогать.** Спавн солдата (`createPlayer` / `SpawnSoldierAt`) решается
отдельно и другими людьми. Оружие — следующий этап. Изменения в
`VextResourceManager.cpp` / `VextCustomization.cpp` безопасны только если они
не ломают уже работающий путь «бот появился без оружия».

**Шаг 1. Индекс EBX (разбор дампа → структура в памяти).** Фундамент. Без него
пункты 2–3 писать не на чем. Результат проверяется офлайн: для всех 125 путей
из `WeaponList.lua` + 20 китов + 8 внешностей индекс должен давать тип из п. 3.1.

**Шаг 2. `SearchForDataContainer` + `SearchForInstanceByGuid`.** Первая
наблюдаемая польза: вместо `WARNING: Path not found` в лог пойдут реальные
типы. Мод при этом начнёт падать в `EbxEditUtils` — это ожидаемо и является
критерием перехода к шагу 3.

**Шаг 3. Lua-биндинг DataContainer** (`typeInfo`, `_G`-касты, поля, скаляры,
`MakeWritable`). После этого `learnStatsValues` и `EbxEditUtils` оживают,
типы оружия (`WeaponTypes.*`) заполнятся. Это уже даёт ИИ-ботам рабочие ТТХ.

**Шаг 4. `ApplyCustomization`.** Первый видимый результат: у бота появляется
ствол в слоте 0. Прицелы/глушители — следующая итерация.

**Шаг 5. `SelectUnlockAssets`.** Киты, внешности, перки (специализации, техника).
Нужен ещё тип `RegistryContainer` (для `Shared\__init__.lua:87`) и
`ResourceManager:AddRegistry` — иначе часть ассетов уровня не подрегистрируется.

**Шаг 6. `CreateEntitiesFromBlueprint` + `EntityBus`.** Только ради
`SpawnGrenade` (`Server\__init__.lua:666`). К ботам отношения не имеет,
сделать можно в любой момент — в том числе в самом конце.

**Шаг 7 (по желанию). `Hooks:Install('EntityFactory:Create', ...)`**
(`Server\__init__.lua:144`) — это хук на создание сущностей движком, а не
функция `EntityFactory`. Нужен, если заработает `CheckForFlareOrSmoke`
(`Server\__init__.lua:560`), т.е. для ИИ-ботов с реактивными защитами и ракетами.
Можно отложить.

---

## 6. Открытые вопросы

1. **Откуда брать EBX в рантайме.** `Venice-EBX` — это текстовая выгрузка
   (`.txt`), сделанная Venice Unleashed из игровых `.ebx` внутри zipped superbundles.
   Работающий `bf3_server` загружает `.ebx` из собственного реестра ресурсов.
   Значит есть два пути: (а) переиспользовать движковый `ResourceManager` сервера —
   тогда нужен реверс внутреннего поиска по имени; (б) читать выгрузку.
   Путь (б) в проде бесперспективен (1.4 ГБ текста, 73k файлов на каждый запуск).
   **Не проверено**, какой путь выбран в проекте и есть ли в репозитории
   скомпилированный/запакованный вид этих данных.
2. **Нужен ли настоящий движковый `DataContainer` или достаточно Lua-объекта.**
   `MakeWritable` в моде вызывается опционально и, судя по коду, только чтение.
   Если хватает чтения, биндинг можно сильно упростить. **Не проверено**,
   вызывает ли что-то ещё `MakeWritable` с ожиданием реальной записи в память движка.
3. **Регистр путей.** Мод мешает регистр (`persistence/...` против
   `Persistence/...`). Поведение реального движка **не проверено**.
4. **GUID реестров уровней** `0B40AC87-7AFF-EDDE-949A-FCBF42CF7126` и
   `D62726FF-B0E2-3619-E95F-57CC5F00D58B` (`Shared\__init__.lua:87/90`) в
   `Venice-EBX` **не найдены** (проверялся полный текст). Возможно, это отдельный
   класс инстансов, который в выгрузку не попал.
5. **`Gameplay/Kits/USEngi`** — баг мода или другое имя в реальном VU (п. 4.3).
   **Не проверено.**
6. **Подмены типов заглушками.** В `VextCustomization.cpp:100-108` конструкторы
   `SoldierWeaponUnlockAsset`/`UnlockAsset` возвращают `{ weapon = data }` /
   `{ linkedTo = { data } }` без проверки типа. Если индекс отдаст по пути
   `Weapons/M416/U_M416_Kobra` объект, который мод уже отдаст как «ствол»,
   подмена не обнаружится. Нужна проверка типа хотя бы по имени класса.
7. **Адреса функций движка** для `SearchForDataContainer` /
   `CreateEntitiesFromBlueprint` **не искались** и в этом документе **не указаны**.
   В `HOOK-TABLE.md`, `BOTLAYER-REV.md`, `PHASE1-MAP.md` и `re\ghidra\*` по этому
   вопросу записей нет.

---

## Приложение: карта «путь → тип» для путей, которые спрашивает мод

| Путь (пример) | Тип `#primary instance` | Зачем мод |
| --- | --- | --- |
| `Characters/Soldiers/MpSoldier` | `SoldierBlueprint` | `player.selectedKit` (`BotSpawner.lua:1545`) |
| `Weapons/M67/U_M67` | `SoldierWeaponUnlockAsset` | слот 6, граната (`BotSpawner.lua:2070`) |
| `Weapons/M67/M67_Projectile` | `ProjectileBlueprint` | `CreateEntitiesFromBlueprint` (`__init__.lua:667`) |
| `Weapons/M416/U_M416` | `SoldierWeaponUnlockAsset` | слот 0, ствол (`BotSpawner.lua:2022`) |
| `Weapons/M416/U_M416_Kobra` | `UnlockAsset` | вложение, `unlockAssets:add` (`BotSpawner.lua:1994`) |
| `Weapons/M416/U_M416_CAMO_1` | `BlueprintAndVariationPair` | вложение-камуфляж |
| `Weapons/M416/U_M416_M26_Frag` | `SoldierWeaponUnlockAsset` | ствол (гранатётом) |
| `Weapons/Knife/U_Knife` | `SoldierWeaponUnlockAsset` | слот 7, нож (`BotSpawner.lua:2098`) |
| `Gameplay/Kits/USAssault` | `VeniceSoldierCustomizationAsset` | `SelectUnlockAssets` №1 (`BotSpawner.lua:1979`) |
| `Gameplay/Kits/USEngineer` | `VeniceSoldierCustomizationAsset` | то же (но мод ищет `USEngi`) |
| `Persistence/.../Visual/MP/Us/MP_US_Assault_Appearance01` | `UnlockAsset` | `SelectUnlockAssets` №2 (`BotSpawner.lua:1973`) |
| `Persistence/.../Specializations/SprintBoostL2` | `ValueUnlockAsset` | `SelectUnlockAssets` №3, перк (`BotSpawner.lua:1901`) |
| `Persistence/.../Vehicles/MBTReactiveArmor` | `ValueUnlockAsset` | перк техники (`BotSpawner.lua:1837`) |

---

# Что сделано дальше (2026-10-11, вторая сессия)

## 7. Сборка и тесты

```
cmd /c "C:\Games\Legacy\legacyext\build.cmd"
...
Готово:
```

Результат: `legacyext\build\legacyext.dll`, 1 628 160 б, 14:58:05.

Тесты — нативный харнесс `legacyext\test\LegacyEXT.Test.exe` (собирается
`build.cmd`, dotnet/msbuild не нужны):

```
=== ИТОГ: успешно 44, провалено 0 ===
код возврата: 0
```

`X86Length.Dump.exe` — тоже нативный инструмент, собирается; проверка декодера
(`build.cmd` зовёт `python C:\Games\re\verify_decoder.py`) отработала без ошибок.

**Побочное наблюдение про сборку.** Первый прогон упал на
`sqlite3.obj: Permission denied`. Это не ошибка кода: параллельно собирал второй
агент, и `cl.exe`/`link.exe` держали те же `build\obj`. После завершения его сборки
повторный `build.cmd` прошёл с «Готово:». Правило на будущее: **`build.cmd` нельзя
запускать двум агентам одновременно** — они пишут в общий `build\obj`.

## 8. Сверка по логу: 46 реально запрошенных путей

`LegacyBF3\logs\legacyext.log` (3094 строки). Выбрано всё, что содержит
`Path not found:` — это ровно те обращения, где заглушка ответила `nil`.

* всего вхождений: **174**
* уникальных путей: **46**
* из них **найдено в `Venice-EBX`: 46, отсутствует: 0**
* тип `#primary instance` у всех 46: **`SoldierWeaponUnlockAsset` — 46 штук**

Это сильный результат по трём причинам:

1. Ни одного несуществующего пути → **индекс строить только на существующих данных**,
   «мусорных» запросов от мода нет.
2. Все 46 — ровно тот тип, в который мод кастует
   (`SoldierWeaponUnlockAsset(...)`, `BotSpawner.lua:2027…2103`) → **приоритет №1
   для проверки индекса — эти 46 путей**.
3. Пути покрывают все шесть слотов: стволы (`U_M416`, `U_MK11`, `U_M249`…),
   гаджеты (`Weapons/Gadgets/C4/U_C4`, `T-UGS/U_UGS`, `Ammobag`, `Medicbag`,
   `Defibrillator/U_Defib`, `RadioBeacon/U_RadioBeacon`, `Repairtool`),
   гранатомёт (`Weapons/RPG7/U_RPG7`), нож (`U_Knife`, `XP2_Knife_RazorBlade/U_Knife_Razor`),
   пистолеты (`Glock17`, `Glock18`, `M9`, `M1911`, `MP443`).

### Критерий приёмки индекса (предложение для второго агента)

Прогнать 46 путей и убедиться, что для каждого возвращается объект, у которого
`typeInfo.name == "SoldierWeaponUnlockAsset"`. Тогда шесть `WARNING: Path not found`
в логе исчезнут. Следующий ожидаемый сдвиг — падение в `EbxEditUtils.lua:42`
(нет `_G.SoldierWeaponUnlockAsset` как конструктора с проверкой типа), это признак,
что индекс заработал и пора браться за Lua-биндинг (шаг 3 в разделе 5).

## 9. Расхождение `Gameplay/Kits/USEngi` — разбор и решение

**Факт (проверено).** Мод строит путь конкатенацией, без словаря:

```lua
-- BotSpawner.lua:2186-2205
function BotSpawner:_FindKit(p_TeamName, p_KitName)
	local s_GameModeKits = { '', '_GM', '_GM_XP4', '_XP4', '_XP4_SCV' }
	for l_KitType = 1, #s_GameModeKits do
		local s_ProperKitName = string.lower(p_KitName)
		s_ProperKitName = s_ProperKitName:gsub("%a", string.upper, 1)
		local s_FullKitName = string.upper(p_TeamName) .. s_ProperKitName .. s_GameModeKits[l_KitType]
		local s_Kit = self:GetResourceDataContainer('Gameplay/Kits/' .. s_FullKitName)
		if s_Kit ~= nil then return s_Kit end
	end
	return nil
end
```

Вызовы (`BotSpawner.lua:1938/1940/1942` и 1949/1951/1953) передают
`'Engi'` для инженера. Итог — мод просит `Gameplay/Kits/USEngi` (и 4 варианта
суффикса). Проверено по диску:

| Просит мод | В данных | Статус |
| --- | --- | --- |
| `Gameplay/Kits/USEngi` | `USEngineer.txt` | **нет** |
| `Gameplay/Kits/USEngi_GM` | `USEngineer_GM.txt` | **нет** |
| `Gameplay/Kits/USEngi_XP4` | `USEngineer_XP4.txt` | **нет** |
| `Gameplay/Kits/USEngi_GM_XP4` | `USEngineer_GM_XP4.txt` | **нет** |
| `Gameplay/Kits/USEngi_XP4_SCV` | `USEngineer_XP4_SCV.txt` | **нет** |
| `Gameplay/Kits/USAssault`, `USSupport`, `USRecon` (+все суффиксы) | одноимённые файлы | есть (20 файлов) |
| `Gameplay/Kits/RU*` | одноимённые файлы | есть (20 файлов) |

Все 10 файлов `*Engineer*` — тип `VeniceSoldierCustomizationAsset`
(проверено: `USEngineer.txt` → `0A99EBDB-602C-4080-BC3F-B388AA18ADDD`).

То есть `s_Kit == nil` для инженера при **любом** качестве индекса, а
`_SetKitAndAppearance` на `BotSpawner.lua:1966-1968` сделает `return` — бот
инженера вообще не получит ни комплекта, ни внешности.

**Предлагаемое решение — таблица соответствий (алиасов) в `SearchForDataContainer`.**

Не править мод и не плодить файлы в `Venice-EBX`. Алиас — это подстановка при
поиске: путь не найден → пробуем замену. Минимальный набор (проверен по диску,
все 10 целей существуют):

```
Gameplay/Kits/USEngi         -> Gameplay/Kits/USEngineer
Gameplay/Kits/USEngi_GM      -> Gameplay/Kits/USEngineer_GM
Gameplay/Kits/USEngi_XP4     -> Gameplay/Kits/USEngineer_XP4
Gameplay/Kits/USEngi_GM_XP4  -> Gameplay/Kits/USEngineer_GM_XP4
Gameplay/Kits/USEngi_XP4_SCV -> Gameplay/Kits/USEngineer_XP4_SCV
Gameplay/Kits/RUEngi         -> Gameplay/Kits/RUEngineer
Gameplay/Kits/RUEngi_GM      -> Gameplay/Kits/RUEngineer_GM
Gameplay/Kits/RUEngi_XP4     -> Gameplay/Kits/RUEngineer_XP4
Gameplay/Kits/RUEngi_GM_XP4  -> Gameplay/Kits/RUEngineer_GM_XP4
Gameplay/Kits/RUEngi_XP4_SCV -> Gameplay/Kits/RUEngineer_XP4_SCV
```

Правило: алиас применяется **только если прямой поиск дал nil**, и в лог пишется
строка вида `WARN алиас: <запрошенный путь> -> <подставленный>` (один раз на путь).
Тогда и случай «в оригинальном VU такого расхождения нет» не сломается молча.

**Что НЕ делать:** не переименовывать файлы в `Venice-EBX` (это выгрузка-эталон,
её читают как источник истины), не добавлять туда новые `.txt`, не патчить
`BotSpawner.lua` в `BF-Portable` (рабочий комплект, и правка мод-источника
переживёт любую пересборку индекса хуже, чем алиас).

**Статус: не проверено**, как это ведёт себя настоящий VU — возможно, у него в
базе ресурсов есть алиас `USEngi`, и это опечатка мода. Алиас безопасен в обоих
случаях.

## 10. `CreateEntitiesFromBlueprint` — спецификация и чего не хватает

### 10.1 Факт: документация VU есть

Комментарий в `VextEntityManager.cpp:9` («Документация VU по entity manager:
НЕ найдена в VU-Docs») **неверен** — документация на месте:
`VU-Docs\content\vext\ref\shared\library\entitymanager.md`.
Это стоит учесть при планировании (снимает риск «нечем проверять»).

### 10.2 Подпись (из `entitymanager.md:14-15`, два перегруженных варианта)

```
CreateEntitiesFromBlueprint(blueprint: DataContainer, transform: LinearTransform) -> EntityBus | nil
CreateEntitiesFromBlueprint(blueprint: DataContainer, params:    EntityCreationParams) -> EntityBus | nil
```

Мод использует второй вариант (`__init__.lua:677`). Вместе с `CreateEntity`
(тот же файл, `:12-13`) это весь «создающий» API `EntityManager`.

### 10.3 Нужный тип `EntityCreationParams`

`VU-Docs\content\vext\ref\shared\type\entitycreationparams.md`:

| Поле | Тип | Нужно ли моду |
| --- | --- | --- |
| `transform` | `LinearTransform` | да (`__init__.lua:673,675`) |
| `networked` | `bool` | да (`__init__.lua:674`) |
| `variationNameHash` | `int` | нет, но объявить |
| `parentRepresentative` | `DataContainer \| nil` | нет, но объявить |

Конструктор без аргументов: `EntityCreationParams()`.

**Проверено: в проекте этого типа нет вообще** — поиск по `legacyext\src` не
находит ни `EntityCreationParams`, ни `EntityBus`. То есть `SpawnGrenade`
(`Server\__init__.lua:666`) упадёт раньше, чем дойдёт до поиска ресурса: на строке
672 вызовется `nil`-глобал. Это отдельная находка, её надо закрывать вместе с
`CreateEntitiesFromBlueprint`, иначе фикс будет бессмысленным.

### 10.4 Нужный тип `EntityBus`

`VU-Docs\content\vext\ref\shared\type\entitybus.md`. Мод читает только
`createdBus.entities` (`__init__.lua:683`) и `bus.entities`
(`BotSpawner.lua:999, 1082, 1127, 1175, 1255`). Минимум для текущих задач:

| Элемент | Тип | Зачем |
| --- | --- | --- |
| `entities` | `Entity{}` | перебор создаваемых сущностей |
| `data` | `DataContainer \| nil` | чертёж |
| `instanceId`, `networkId` | `int` | идентификация |
| `FireEvent(...)` | void | вызов события по имени/id |
| `RegisterEventCallback` / `UnregisterEventCallback` | int / void | подписка |
| `==` (оператор) | bool | сравнение шин |

### 10.5 Куда это класть (без конфликта с чужим файлом)

`VextEntityManager.cpp` **входит в мой запрет на правку**, и он же числится
запрещённым у человека. Значит:

* основное место — **новый** модуль (например `src\VextEntityBus.cpp`), который
  регистрирует `EntityCreationParams`, `EntityBus` и
  `EntityManager:CreateEntitiesFromBlueprint`;
* подключение — одна правка `build.cmd` и одна строка регистрации в
  `LuaRuntime.cpp` (файл **не** в запрете);
* **но**: `build.cmd` тоже в запрете у обоих. Значит реально это делает тот,
  кто сейчас владеет монтажом — либо человек по моей записке.

Приоритет низкий (см. раздел 5, шаг 6): к ботам отношения не имеет, только
консольная команда `SpawnGrenade`. Но тип `EntityCreationParams` дешёвый и
убирает падение по `nil`.

## 11. Передача второму агенту: контракт каста

То, что нужно соблюсти в реализации `SearchForDataContainer` / DataContainer,
свёрнуто в чек-лист (детали в разделах 2 и 3):

1. Возвращать **userdata**, у которого есть `.typeInfo.name` (строка).
2. `_G[<typeInfo.name>]` должен быть **конструктором-кастом**, принимающим этот
   userdata. Имена, которые понадобятся сразу (проверено по логу и коду):
   `SoldierWeaponUnlockAsset` (46 путей из лога + 6 слотов),
   `UnlockAsset` (прицелы/глушители, `BotSpawner.lua:1994`),
   `ValueUnlockAsset` (перки), `VeniceSoldierCustomizationAsset` (киты),
   `SoldierBlueprint` (`MpSoldier`).
3. `typeInfo` должен быть доступен и у **вложенных** контейнеров — мод делает
   «safety cast» на каждом шаге (`EbxEditUtils.lua:70`).
4. Индексация `obj['Поле']` + настоящие Lua-скаляры для примитивов
   (`EbxEditUtils.lua:66, 101-104, 112`).
5. **Авторазыменовка ссылок** вида `путь/GUID` при обращении к полю —
   иначе `learnStatsValues` не дойдёт до ТТХ
   (пример: `U_M67.Weapon` → `Weapons/M67/M67/C569F719-…` = `SoldierWeaponBlueprint`).
6. `MakeWritable()` — мод зовёт опционально, если метода нет — не ошибка.
7. Алиас `…/Kits/<T>Engi*` → `<T>Engineer*` (раздел 9).
8. Индекс строить **без учёта регистра** либо с нормализацией: мод для перков
   присылает `persistence/unlocks/soldiers/specializations/sprintboostl2`,
   а в данных `Persistence/Unlocks/Soldiers/Specializations/SprintBoostL2`.
   Без нормализации ~30 перков не найдутся.

## 12. Итог по сессии

| Пункт | Статус |
| --- | --- |
| Сборка `build.cmd` | **Готово**, DLL 1 628 160 б |
| Тесты `LegacyEXT.Test.exe` | **44 успешно, 0 провалено**, код 0 |
| 46 путей из лога vs EBX | **46/46 найдено, тип везде `SoldierWeaponUnlockAsset`** |
| Расхождение `USEngi` | Разобрано, готово решение (10 алиасов) |
| `CreateEntitiesFromBlueprint` | Спецификация VU получена, найдено 2 недостающих типа |
| Индекс EBX / `SearchForDataContainer` | **делает другой агент** (не трогал) |
| Lua-биндинг DataContainer | не начат — ждёт индекса |
---

## 13. Реализовано 2026-10-11 (позже): авторазыменовка ссылок, алиас, регистр

Это дополняет разделы 3.3 и 2.5. Найденное там «главное осложнение» сделано.

### 13.1 Почему индекс пришлось переделать (v1 -> v2)

Индекс v1 хранил **по одной записи на файл** — именованный инстанс с
`#primary instance`. Для `SearchForDataContainer` этого хватало, но ссылка
`Weapon Weapons/M416/M416/8775DC85-...` указывает на `SoldierWeaponBlueprint`,
а `Object 793FC225-...` — на `SoldierWeaponData`: это **обычные**, не
именованные инстансы, которых в v1 не было вовсе.

Поэтому v2 (`LEBXIX02`) кладёт **все** инстансы каждого раздела:

| | v1 | v2 |
| --- | ---: | ---: |
| записей | 21 685 | **183 266** |
| размер | 4,90 МБ | **18,28 МБ** |

Формат записи не изменился (те же 40 байт) — просто у обычных инстансов
заполняется `strGuid`, а `strPath`/`strPathLo` остаются пустыми. Ключевая
идея: **один и тот же парсер файла** (утилита) выдаёт и именованный, и
обычные инстансы; второй разборщик не писался. Отдельная таблица порядка
`guidOrderOff` даёт отсортированный по GUID список для разыменовки.

Уникальных GUID — 181 872 (меньше 183 266: часть GUID встречается дважды в
разных файлах).

### 13.2 Три формы ссылки и что с каждой стало

| Форма | Пример | Стало |
| --- | --- | --- |
| `путь/GUID` | `Weapons/M416/M416/8775DC85-...` | DataContainer `SoldierWeaponBlueprint` |
| голый GUID (тот же раздел) | `Object 793FC225-...` | DataContainer `SoldierWeaponData` |
| `путь/GUID` в другой каталог | `AIData AI/Weapons/.../D0941902-...` | DataContainer `AIWeaponData` |
| `*nullGuid*` | `NextLevelUnlockAsset *nullGuid*` | `nil` (как и было) |

Распознавание: значение считается ссылкой, если его хвост из 36 символов —
валидный GUID 8-4-4-4-12 (сOptional `/` перед ним). Всё остальное остаётся
строкой.

### 13.3 Имена полей приходят в мод в нижнем регистре — это ломало обход

Отдельная находка, без которой разыменовка всё равно не работала бы.
`EbxEditUtils:FormatMemberName` (`EbxEditUtils.lua:217-253`) приводит имя поля
к виду с маленькой первой буквой. Мод спрашивает:

| мод спрашивает | в данных |
| --- | --- |
| `weapon` | `Weapon` |
| `object` | `Object` |
| `weaponFiring` | `WeaponFiring` |
| `primaryFire` | `PrimaryFire` |
| **`shot`** | **`Shot`** |
| `projectileData` | `ProjectileData` |
| `aiData` | `AIData` |

Поэтому поиск поля идёт в два прохода: сначала точно, затем без учёта регистра.

### 13.4 Проверенная цепочка (WeaponClass.lua:44-65)

Взят реальный M416, все GUID — из данных, не выдуманы:

```
SearchForDataContainer('Weapons/M416/U_M416')   SoldierWeaponUnlockAsset
  .weapon            -> SoldierWeaponBlueprint   (ссылка путь/GUID)
  .object            -> SoldierWeaponData         (голый GUID, тот же файл)
  .weaponFiring      -> WeaponFiringData          (ссылка)
  .primaryFire       -> FiringFunctionData        (ссылка)
  .shot              -> ShotConfigData            (вложенная структура Shot::ShotConfigData)
  .shot.projectileData -> BulletEntityData        (ссылка в Weapons/Common/Bullets/...)
  .aiData            -> AIWeaponData              (ссылка в AI/...)
```

Тот же обход проходит для M67, M1911, Knife, SMAW, FIM92A — проверено скриптом
по данным, не только в тесте.

### 13.5 Алиас и регистр

* **Алиас `USEngi -> USEngineer`.** Срабатывает ТОЛЬКО после промаха по точному
  и безрегистровому поиску, и всегда пишет в лог, какой путь подменён на
  какой. Покрывает все варианты, которые строит мод (`USEngi`, `USEngi_GM`,
  `USEngi_XP4`, `USEngi_GM_XP4`, `USEngi_XP4_SCV`). На посторонний путь вроде
  `USEngiXXX` алиас не срабатывает (проверено тестом).
* **Регистр.** Сначала точное совпадение, затем запасной поиск по пути в нижнем
  регистре. Закрывает ~30 перков вида
  `persistence/.../sprintboostl2` против `Persistence/.../SprintBoostL2`.

### 13.6 Что даёт боту

`WeaponClass:learnStatsValues` больше не получает `WARNING: No fireData for: ...`
по этому пути — вместо этого читает реальные ТТХ (классы `FiringFunctionData`,
`AIWeaponData`, `BulletEntityData` приходят как полноценные DataContainer и
кастуются через `_G[typeInfo.name]`, как требует `EbxEditUtils.lua:70`).

### 13.7 Проверка

| Пункт | Результат |
| --- | --- |
| `build.cmd` | **Готово**, без предупреждений (`/W4 /WX`) |
| `LegacyEXT.Test.exe` | **44/44**, регрессий нет |
| `EbxDcCheck.exe` | **20/20**, включая полную цепочку разыменовки |
| `verify_ebx_index.py` | формат v2, 7/7 ссылок и 9/9 путей, **OK** |
| `verify_mod_paths.py` | **353/353** путей мода |

### 13.8 Осталось нерешённым

* `ApplyCustomization` и `SelectUnlockAssets` — заглушки. Данные теперь есть,
  но мод ещё не применяет их к солдату.
* `partitionGuid` / `partition` — nil (нет типа `DatabasePartition`).
* `CreateEntitiesFromBlueprint` + `EntityBus` — по-прежнему отсутствуют.
* Массивы (`UnlockParts::array`) отдаются как userdata с целочисленным доступом,
  но `add`/`size` у них нет — `unlockAssets:add(...)` в моде идёт через
  `VextCustomization`-список, а не по EBX.
* Полный корпус индекса (73 475 файлов, 1,4 ГБ) не строится: `Levels` (1 ГБ)
  и `Textures` мод не спрашивает. Если понадобится, утилита это умеет, но
  прогон упирается именно в эти каталоги.
