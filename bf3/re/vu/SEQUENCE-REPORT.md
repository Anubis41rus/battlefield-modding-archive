# Как VU спавнит бота: порядок вызовов и что делает каждый биндинг

> 15.10.2026, только чтение. Источники: fun-bots (`BF-Portable\...\Mods\fun-bots\ext\`), `veniceext.dll` (VU),
> `Battlefield 3\zbf3.exe` (расшифрованный клиентский bf3.exe, база 0x400000, тот же timestamp 0x511C9356, что у bf3.exe),
> наш `bf3_server_Final.exe`. Таблица движковых функций VU (`[0x18E74xxx..0x18E75xxx]`) заполняется в `0x107027xx..0x107035xx`
> строками `mov dword ptr [слот], адрес_в_bf3.exe`, у каждого слота одна запись. Значит, **сервер VU — это клиентский bf3.exe
> в серверном режиме**, и адреса VU — это адреса bf3.exe. Для нашего сервера аналоги найдены по совпадению кода (сравнивался
> побайтовый ход функций и смещения полей), а не по адресам.
> Хэши типов сообщений: djb2-xor (`h = h*33 ^ c`, старт 5381) от полного имени. Все хэши сошлись с именами из строк VU.

## 1. Что fun-bots делает для одного бота (всё в ОДНОМ синхронном вызове Lua)

Конфигурация по умолчанию: `Config.SpawnMethod = SpawnOnTdm` (`Shared/Config.lua:170`). Для TDM/SDM/GM и AirSuperiority
берётся `Spawn` (событие на точке спавна), для остальных режимов (Conquest, Rush) — `SpawnSoldierAt` (`Server/__init__.lua:326-337`).

### Ветка SpawnSoldierAt (Conquest/Rush), `BotSpawner:_SpawnSingleWayBot`
1. `BotManager:CreateBot` → `PlayerManager:CreatePlayer(name, team, squad)` — `BotManager.lua:868`; сразу `player.input = EntryInput()` — `:873-876`.
   (Если бот уже есть, `CreateBot` только пишет `teamId`/`squadId` — `:849-850`.)
2. `SetSquadLeader(true,false)`, если он первый в отряде — `BotSpawner.lua:1491` (или `:725`, `:1427`).
3. `m_BotCreator:SetAttributesToBot` (только Lua: класс, цвет) — `:1493`; `SetVarsWay` — `:1494`.
4. `_SpawnInEntity` → `_SpawnBot` — `:1495` → `:1503-1517` → `:1523`:
   - `_SetBotWeapons` (только Lua-таблица оружия) — `:1540`;
   - `selectedKit = MpSoldier` (SoldierBlueprint), если nil — `:1543-1546`;
   - `_SetKitAndAppearance` → **`Player:SelectUnlockAssets(кит, {внешность}, перки)`** — `:1548` → `:1973/1975/1979/1981`;
   - `BotManager:SpawnBot` — `:1550` → **`Player:CreateSoldier(selectedKit, transform)`** — `BotManager.lua:905`,
     `soldier.maxHealth = …` — `:912`, **`Player:SpawnSoldierAt(soldier, transform, pose)`** — `:913`;
   - если `player.soldier == nil` — выход (`BotSpawner.lua:1552-1555`);
   - **`soldier:ApplyCustomization(CustomizeSoldierData)`** (оружие, activeSlot, removeAllExistingWeapons=false) — `:1557`, данные `:1999-2121`.
5. Если спавн в технику — `_EnterVehicleEntity` после `_SpawnBot` (`:1516`).

### Ветка Spawn (TDM/SDM/GM), `:1331-1342`
`GetBot` (CreatePlayer) → `SetAttributesToBot` → `_SelectLoadout` (`:914-939`: `_SetBotWeapons`, `selectedKit`,
**`SelectUnlockAssets`**) → `_TriggerSpawn` (`:942`) — `FireEvent(ServerPlayerEvent("Spawn", player, …))` на
`ServerCharacterSpawnEntity` своей команды (`:970-1066`). Тело создаёт сам движок. На следующих тиках `OnEngineUpdate`
(`:170-245`), когда `player.soldier ~= nil`, мод вызывает `_ApplyCosumizationAfterSpawn` → `soldier:ApplyCustomization` (`:191/216/236`, `:1566-1568`).

**Итог (факт):** в обеих ветках мод делает выбор (`SelectUnlockAssets`) ДО того, как появится тело, в том же вызове Lua.
`Player:SelectWeapon` fun-bots не вызывает вообще (оружие задаётся через `ApplyCustomization`; `EIASelectWeapon*` — это ввод).

## 2. Биндинги VU: адрес → что делают → функции и поля движка

| Биндинг | veniceext | Что делает (по дизассемблеру) | Движок bf3.exe (VU) | Аналог в нашем сервере |
|---|---|---|---|---|
| `PlayerManager:CreatePlayer` | `0x10B99110` (ссылка на RTTI `ServerPlayerCreateMessage`, vtable `0x1782DBD4`, `0x10B991C1`) | 1) `[0x18E752E4]`=`0xA0C250`(0,-1,0) — создать игрока; 2) `[0x18E752E8]`=`0xA14060`(игрок, имя); 3) **`ServerPlayerCreateMessage`** (ctor `0x1F85FE0`, хэш `0x3051E83C`), игрок в msg, рассылка `MessageManager` `0x4A6050` (`[0x236B608]`); 4) **≈70 лицензий** боту: `0x816A50(имя)` на extent `игрок+0x690` ×N (`le, b2k, xp2, xp3, xp4, xp5, pwp, spec, speca, pepper1..6, dogtags, …, premium, xp2_ks…xp5_ks, …`), затем `0x816A90` (лицензии из конфига); 5) `0xA99C10` → `+0x49C=1` + **`ServerPlayerLevelLoadedMessage`** (`0x52D00BE9`); 6) `0xAA03C0` → для всех extent'ов vfunc+8, `+0x49D=1`; 7) `0xA998F0(0)` → если `+0x49C`,`+0x49D`, `!+0x49E`: **`ServerPlayerEnteredLevelMessage`** (`0x09B7EA47`), `+0x49E=1`, `+0x4A0=0` (у человека ещё `NetworkFirstPlayerEnteredMessage` в его соединение); 8) команда/отряд через объект VU (`vfunc+0xC(игрок, teamId,1,0)`, `vfunc+8(игрок, squadId,0)`); 9) событие VU (хэш `0xD6F2F1CA`) | `0xA0C250`, `0xA14060`, `0x816A50`, `0x816A90`, `0xA99C10`, `0xAA03C0`, `0xA998F0` | createPlayer `0x00E644B0` + addToArray `0x00E6CA00` (по смыслу); лицензия `0x011764E0` (thiscall на extent `игрок+[0x0285BAD8]`, бит в `[extent+0xDC]`); лицензии из конфига `0x01176520`; levelLoaded `0x00E19380`; extents-init `0x00E208E0`; enteredLevel `0x00E18FB0` (ret 4) — сервер сам зовёт его каждый тик из `ServerPlayer::update` (`0x00E38273`) |
| `Player:SelectUnlockAssets` | overload: `0x1098E670` (кит, внешность) и `0x1098EB70` (кит, внешность, анлоки) — рег. `0x1098E500-0x1098E51F` | проверка `CharacterCustomizationAsset` / `UnlockAsset` / `UnlockAssetBase`; alloc 0x48 (`[0x18E74CE8]`), ctor **`NetworkPlayerSelectedUnlockAssetsMessage`** (`[0x18E7525C]`=`0x1FB11C0`, хэш `0xF7531939`): `+0x38` кит, `+0x3C` внешность, `+0x40` анлоки; **синхронный вызов обработчика сервера** `[0x18E74FE4]`=`0xAB16F0` (ecx=игрок) — `0x1098EB13` / `0x1098F68E`; затем удалить сообщение | `0xAB16F0` = обработчик выбора: кит → `0x11BE570` + флаг репликации `0x40` на `+0x374`; внешность → очистка и заполнение вектора `+0x4B8` (с проверкой `vtbl+0x18(asset)`); анлоки → набор `+0x94` + флаг `0x20`; слоты оружия `+0x50C..+0x510`; **если уже есть тело (`+0x3C4`) — `applyCustomization`** (`0xAB1D96`) | **`0x00E331B0`** (тот же обработчик: `0x008C2660` кит + `0x01A35BA0(0x40)`, `+0x4B8`, `0x00E2C1D0`/`0x00E299A0` перки + `0x01A35BA0(0x20)`, в конце `0x00E322F0`) — `re\selectunlock_decomp.txt` |
| `Player:CreateSoldier` | `0x1098B5D0` | если `[игрок+0x3C4] != 0` → nil (у игрока уже есть тело — второе не создаётся); иначе `[0x18E75204]`=`0x81FC60` (ecx=`игрок+0x690`, args: blueprint, игрок, transform). Тело только создаётся: не садится, `+0x3C4` не пишется | `0x81FC60` (`[игрок+0x31C]` → создание сущности из blueprint `0xA8E150`) | **`0x011821F0`** (тот же код: `[игрок+0x31C]`, `0x00DD4470`, создание сущности); this = `игрок + [0x0285BAD8]` |
| `Player:SpawnSoldierAt` | `0x10990700` | приведение к `ServerSoldierEntity`; `[0x18E75208]`=`0x816810` (ecx=`игрок+0x690`, args: солдат, transform, pose); затем **`ServerPlayerRespawnMessage`** (ctor `[0x18E75368]`=`0x1F860D0`, хэш `0xC935A574`, игрок в msg+0x28) → `MessageManager` `0x4A6050` | `0x816810` = VeniceServerPlayerExtent::spawnSoldier: старое тело убрать; `setPose` `0xAB7E40`; `setTransform` vfunc+0x64; `setSoldier` `0xAB4910` (`+0x3C4`); **`applyCustomization(игрок, солдат, 0)` `0xAB0B90`**; инициализация/спавн сущности `0xA995E0` | **`0x011760E0`** (вход функции — перед ним `CC CC`; код 1:1: `0x5E1C60`, `0x00DFCC50` setPose, vfunc+0x6C, `0x00E37180` setSoldier, **`0x00E322F0(игрок, солдат, 0)`**, `0x00E18980`); this = `игрок + [0x0285BAD8]`; args (солдат, transform*, pose), ret 0xC. Внимание: старая запись MODLOG (~стр. 869) «0x011760e0 — середина функции» неверна |
| `SoldierEntity:ApplyCustomization` | `0x109BBF20` (проверка `CustomizeSoldierData`) | синхронно `[0x18E74FE8]`=`0xAB0B90`(cdecl: `[солдат+0x108]` (владелец-игрок), солдат, CustomizeSoldierData) — `0x109BC22B` | applyCustomization `0xAB0B90` (читает `+0x4B8`, `+0x50C`, `+0x94`) | `0x00E322F0` (наш `kApplyCustomRva`) — в нём 3-й аргумент = данные кастомизации (у VU сюда идёт объект `CustomizeSoldierData`) |
| `Player:SelectWeapon` | `0x1098F810` | сообщение (ctor `[0x18E75258]`), анлоки (`UnlockAssetBase`) → вектор `+0x3C`; синхронно `[0x18E74FE0]`=`0xAB15C0` (`0x1098FF50`) | `0xAB15C0` (обработчик выбора оружия, внутри вызывает applyCustomization) | `0x00E33040` (по смыслу; вызывает `0x00E322F0`) |
| `Player.soldier` / `hasSoldier` | `0x1098C960` / `0x1098D540` | `[игрок+0x3C4]` | — | то же (уже сделано в 135) |

## 3. Ответ на главный вопрос

**Факт.** В VU нет ни одного отложенного шага: каждый биндинг сразу вызывает функцию движка в потоке Lua.
К моменту посадки тела у игрока уже записаны кит (`+0x90`, флаг репликации `0x40`), внешность (`+0x4B8`),
перки (`+0x94`, флаг `0x20`) и слоты оружия (`+0x50C`). Пишет их **движковый обработчик выбора** (`0xAB16F0`, у нас `0x00E331B0`):
VU собирает то же сообщение, что прислал бы живой клиент (`NetworkPlayerSelectedUnlockAssetsMessage`), и вызывает обработчик напрямую.
Затем `spawnSoldier` движка (`0x816810`, у нас `0x011760E0`) сам вызывает `applyCustomization(игрок, солдат, 0)` ДО
инициализации тела (`0xA995E0`, у нас `0x00E18980`). Модель, иконки и оружие тело получает в момент посадки.
Своё тело движок боту VU не даёт: тело появляется только по `SpawnSoldierAt` (или по событию `Spawn` в TDM — и там мод тоже уже вызвал `SelectUnlockAssets`).

**Флага «customization done» VU не ставит.** `ServerPlayerCustomizationDoneMessage` встречается в VU только как имя в реестре типов.
VU рассылает такие сообщения движка: `ServerPlayerCreateMessage`, `ServerPlayerLevelLoadedMessage`, `ServerPlayerEnteredLevelMessage`
(при CreatePlayer) и `ServerPlayerRespawnMessage` (при SpawnSoldierAt). Клиенту выбор доходит через флаги репликации
`+0x374` (`0x40` — кит, `0x20` — анлоки), их ставит сам обработчик выбора.

## 4. Ник над головой и вторая иконка (с учётом находки по прогонам 145/146)

**Факты из кода VU.** Специальной записи «ник» нет. Но `CreatePlayer` у VU доводит бота до полного состояния «в уровне»,
как у живого игрока:
- `+0x49C` = 1 и `ServerPlayerLevelLoadedMessage` (у нас `0x00E19380`; в нашем сервере его зовёт только `onCreatePlayerMessage`
  `0x00E48490` → `0x00E48829`, а этот путь у нас выключен по умолчанию: `LEGACYEXT_BRIDGE_ONCREATE`);
- `+0x49D` = 1 (у нас `0x00E208E0`; сервер зовёт его для игроков, которые уже есть на загрузке уровня (`0x00E5F1D0` из `0x00D61BF8`, ServerLevel),
  и для человека в `clientLinked` `0x00E3A2E0`). **Бот, созданный нашим мостом после загрузки уровня, его не получает (гипотеза, по коду)**;
- `+0x49E` = 1 и `ServerPlayerEnteredLevelMessage` — у нас `0x00E18FB0`. Его сервер сам зовёт каждый тик из `ServerPlayer::update` (`0x00E38273`),
  но срабатывает он только при `+0x49C && +0x49D`. На `EnteredLevel` подписаны точки спавна (`0x00DE2388`, «Got ServerPlayerEnter message»),
  PointSystem (`0x01320359`) и другие слушатели (`0x00DC94C1`, `0x00DD419B`, `0x00DDB845`, `0x012C2DE4`, `0x012E6615`, `0x0132C78E`);
- лицензии в `[extent+0xDC]` (у нас `0x011764E0` / `0x01176520`). От них может зависеть проверка `vtbl+0x18(asset)` в обработчике выбора (гипотеза).

**Повторная рассылка данных существующих игроков новому клиенту.** Отдельной функции «разослать всех игроков подключившемуся»
в разобранном коде не найдено. `NetworkCreatePlayerMessage` (`0xD204CF07`) у нас создаётся только в `ClientConnection::onConnected`
(`0x00ED4060`) — это сторона клиента, не сервер. Человеку при входе в уровень сервер шлёт `NetworkFirstPlayerEnteredMessage`
(`0xB8D1A4F0`, ctor `0x02301150`) в его соединение (ветка в `0x00E18FB0`, отправка `0x00E4DE80`).
**Гипотеза:** данные игрока клиент получает при создании ghost'а игрока и потом по флагам `+0x374`. Боты VU обычно создаются
до входа людей и при этом показывают ник и иконки. Значит, у VU при входе клиента ghost'ы существующих ботов уходят ему в
полном состоянии, а наши «ранние» боты остаются неполными: у них нет `+0x49C/+0x49D/+0x49E` (и, возможно, лицензий).
Это согласуется с прогонами 145/146 (ранние боты — без ника и второй иконки навсегда, поздние — в порядке), но не доказано.

**Почему «поздние» боты правильные (факт по нашему коду).** Путь позднего создания (`LateCreateEngineImpl`,
`VextPlayerManager.cpp` ~2760-2810) возвращает `nativePtr` синхронно и тело НЕ сажает. Поэтому следующие вызовы мода
(`SelectUnlockAssets` → `SetBotKit/Appearance/Perks`) попадают в движок до `SpawnSoldierAt` — как в VU.
Ранний путь (`NativeCreateOne`, ~2320-2675) ставит игрока в очередь (`nativePtr = nil` у мода), затем сам сажает тело
(`NativeSpawnOne` или отложенно `g_deferredSpawn`) независимо от мода. Выбор мода в это время лежит в `pendingKit` и
`selectedAppearanceName`/`selectedPerkNames`, и в движок его пишут только при привязке (`PollCreatedBots`, ~909-950).
Это и есть «тело раньше выбора».

## 5. Что поменять у нас (рекомендации)

1. **(Факт о VU → делать так же.) Убрать собственную посадку тела у ранних ботов.** Тело должно появляться только по
   `Player:SpawnSoldierAt` мода. Ранний путь должен создавать игрока и ничего не сажать (как `LateCreateEngineImpl`).
   Тогда «ждать выбор 15 с» (сборка 146) не нужен: порядок обеспечит сам мод.
2. **(Факт.) `SelectUnlockAssets` — через движковый обработчик, а не по полям.** Вызывать `0x00E331B0` (thiscall, ecx = игрок, arg = сообщение)
   с собранным `NetworkPlayerSelectedUnlockAssetsMessage`: ctor сообщения у нас — функция с `push 0xF7531939; push 0xC67D277B`
   (поиск дал `0x02334D20`/`0x02334D70`; какая из них ctor — сверить дизассемблером, правило 18). Поля: `+0x38` кит, `+0x3C` вектор внешности,
   `+0x40` вектор анлоков, `+0x44` bool. Обработчик сам выставит `+0x90/+0x4B8/+0x94/+0x50C`, флаги `0x40/0x20` на `+0x374`
   и, если тело уже есть, вызовет applyCustomization. Если `nativePtr` ещё нет — копить и вызывать при привязке, но тело до этого не сажать.
3. **(Факт.) `SpawnSoldierAt` → `0x011760E0`** (this = `игрок + [0x0285BAD8]`, args: солдат из `0x011821F0`, указатель на transform, pose), а не точка спавна.
   Внутри уже есть `applyCustomization(игрок, солдат, 0)`. После этого — `ServerPlayerRespawnMessage` (ctor с `push 0xC935A574; push 0xF3C93DD3`
   = `0x02307700`, игрок в msg+0x28 — смещение сверить), разослать через MessageManager (`0x00549A20`, ecx = `[0x02B3EE2C]`, как в `0x00E19380`).
   `CreateSoldier` → `0x011821F0(blueprint, игрок, transform)` на том же extent.
4. **(Гипотеза, проверять по логу) Довести бота до «в уровне», как VU в CreatePlayer.** Сразу после createPlayer: `0x00E19380` (levelLoaded),
   `0x00E208E0` (extents, `+0x49D`). `0x00E18FB0` сервер вызовет сам из `ServerPlayer::update`, или вызвать с аргументом 0.
   Плюс лицензии: `0x011764E0(имя)` на extent по списку VU, затем `0x01176520`.
   **Сначала пассивная проверка без правок движка:** прочитать байты `+0x49C/+0x49D/+0x49E/+0x4A0` и `[extent+0xDC]`
   у «раннего» бота, «позднего» бота и у человека. Если у поздних они такие же нулевые, как у ранних, — причина
   ника/иконки в другом (тогда смотреть, когда создаётся ghost и есть ли флаги `+0x374` при создании).
5. Мелочь в нашем Lua (факт): при привязке внешность переприменяется только внутри `if p.pendingKit ~= nil`
   (`VextPlayerManager.cpp` ~930-936). Если кит уже был применён, а внешность пришла позже, внешность не уйдёт в движок.

## Инструменты (scratchpad, только чтение)
`fn.py VA [n] [exe]` (дизассемблер с подписями строк), `table.py` (таблица движковых функций VU → `table.txt`),
`refs.py`, `callers.py exe VA…` (поиск `call rel32`), `find.py exe "байты ??"`, `offuse.py exe off [lo hi]`,
`vtname.py`, `rtti.py`, `hashes.txt` (djb2-xor всех имён сообщений из строк VU).
