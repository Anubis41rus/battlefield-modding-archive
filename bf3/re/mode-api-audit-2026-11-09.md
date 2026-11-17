# Контракты режимов — независимая сверка 09.11.2026

Только чтение исходников, fun-bots, локальной VU-Docs, EBX и уже удержанных материалов. Игра/сервер/бэкенд не запускались. Исходники и мод не менялись. Актуальная рабочая правка родительского агента — 263; выпущенная 262 этим отчётом не заменяется.

| Контракт | Результат по исходникам | Граница |
| --- | --- | --- |
| Conquest `team`, `flagLocation`, `isControlled`, `isCaptureEnabled`, `isAttacked` | `l_em_cp` читает настоящие поля; `refreshCp` обновляет ту же обёртку, которую держит GameDirector. Использование модом согласуется с VU-Docs. | Адреса повторно не подтверждались игровым наблюдением; обновление раз в 0,5 с. |
| `CapturePoint:Captured/Lost` | При получении контроля и ненейтральной команде — Captured; при потере контроля — Lost. Мод на Captured берёт текущую команду, на Lost ставит нейтральную. Противоречия для обычного захвата не найдено. | Опрос может пропустить промежуточные состояния; это не идентичная VU доставка каждого нативного события. |
| Начальные команды Conquest | После `NodeCollection:FinishedLoading` `_InitFlagTeams` получает обёртки флагов и обновляет цели по их командам и позиции. | Начальная загрузка с нулём сущностей остаётся риском таймаута; это не наблюдение текущего прогона. |
| Имена флагов и основные цели | Синтетические `EVBOTS_CP#` уникальны, а HQ имеет суффикс HQ. Обычный GameDirector переводит их в маршрутные цели по позиции. | Суффикс HQ выводится из `CapturableType==1` (`CTUnableToChangeTeam`), а не из настоящего имени. Это не универсальная эквивалентность всех типов точек. |
| Имена флагов и gunship | **Подтверждённая несовместимость исходной реализации:** `GameDirector:OnCapturePointCaptured` прямо сравнивает `capturePoint.name` с `ID_H_US_*`; синтетическое имя никогда не совпадёт. | Касается списка XP3/XP5 в `GetGunshipObjectiveName` для ConquestSmall/Large. Не доказывает причину симптома на MP_017. Последующая точная цепочка чтения имени восстановлена, см. `capture-point-name-2026-11-09.md`; интеграция/игровая проверка — у родительского агента. |
| `bus.entities` у флага и спавна | Мост группирует CharacterSpawn и VehicleSpawn по одному настоящему указателю bus; это соответствует тем двум типам, которые BotSpawner ищет для обычного спавна/отсева техники. | Полный `EntityBus.entities` VU включает и другие сущности; они здесь не перечисляются. Ошибочная связь конкретной карты без runtime не доказана. |
| Rush спавн у A/B на товарища | **Штатная ветка мода:** `GetSpawnPath` явно пробует squadPlayers, затем teamPlayers; нужны soldier и `isAllowedToSpawnOn`. Только после этого выбирает базу текущего сектора. | Место появления рядом с A/B само по себе не доказывает спавн на MCOM. Нельзя исправлять тотальным base-only Rush. |
| `isAllowedToSpawnOn` | Getter обращается к живому native-полю с собственным указателем бота/человека. VU-Docs описывает разрешение другим игрокам появляться на данном игроке. | При отсутствии/ошибке native-чтения возвращает true; это разрешающий fallback, а не подтверждение разрешения движка. Наблюдения такого сбоя в жалобе нет. Setter свойства пока обычный rawset, но fun-bots его не назначает в проверенных местах. |
| `SquadNone` | `GetPlayersBySquad` буквально фильтрует `teamId` и `squadId`; для нулевого отряда вернёт игроков с нулевым отрядом. | VU-Docs не устанавливает особое поведение для SquadNone. На основании одной Docs нельзя объявить этот фильтр ошибкой; общий team-spawn мод всё равно пробует отдельно. |
| Определение режима | `SharedUtils:GetCurrentGameMode` читает опцию GameMode живого уровня; `Level:Loaded` передаёт её `FunBotServer:SetGameMode`, который заново задаёт флаги Rush/Conquest/Tank. Кэш старого режима в этой цепочке не найден. | **Несогласованный fallback:** `EngineLevelInfoRaw` может вернуть true с пустым mode; `EngineLevelReady` это принимает; `EngineLevelNameAndMode` тогда объявит ConquestSmall0, а SharedUtils вернёт nil. При недоступной опции событие и getter расходятся. Достижимость на живой ротации не подтверждена. |
| Ротация, порядок загрузки целей | Мост вызывает Destroy/сброс, Loaded, затем RoundReset. `NodeCollection` загружается своей стейт-машиной на последующих тиках, после чего мод сам шлёт FinishedLoading и строит цели. Ошибки двойного продвижения стартовой стадии по этому порядку не найдено. | Число сущностей и ready не доказывают правильную загрузку всех путей конкретной карты. Сброс старых native событий и TTL-списков разбирается в отдельном Rush-отчёте и правке 263. |

## Точные источники

- `EVBots-dev/evbots-dll/src/VextEntityManager.cpp`: `l_em_cp`, `spawnEntities`, `vspawnEntities`, `EVBotsBusEntities`, `cpEntity`, `EntityManager._Poll`.
- `EVBots-dev/evbots-dll/src/VextPlayerManager.cpp`: `Player.__index`, `EVBotsPlayerByPtr`, `GetPlayersBySquad`, `EngineLevelInfoRaw`, `EngineLevelReady`, `EngineLevelNameAndMode`, `l_bridge_level_info`, обработка `LevelSignals`.
- `EVBots-dev/evbots-dll/src/VextUtils.cpp`: `SharedUtils:GetCurrentGameMode`.
- `fun-bots/ext/Server/GameDirector.lua`: строки 54–69, 112–150, 884–923, 1221–1276, 1297–1502, 1896–1934, 2094–2145.
- `fun-bots/ext/Server/__init__.lua`: `OnLevelLoaded`, `SetGameMode`; `NodeCollection.lua`/`NodeEditor.lua`: отложенная загрузка путей.
- `VU-Docs/content/vext/ref/server/{type/capturepointentity.md,type/player.md,event/capturepoint_captured.md,event/capturepoint_lost.md,library/playermanager.md}`.

## Почему пока нельзя подставить data.name для gunship

В настоящей рефлексии `CapturePointEntityData` (33 поля, размер C0) **нет** `Name`, `NameSid` или `LocationNameSid`; в локальной Docs их также нет. `emData(cp.data).name` не является установленным источником.

У `SpawnReferenceObjectData` есть `LocationNameSid`, offset `+0x120` — это проверено существующим `re/capture/fields.py` по выбранному серверному exe, вывод `mode-spawn-fields-20261109.txt`. Но это поле дочернего спавна: `CapturePointPrefabFlagpole.txt` связывает разные входы для team1 и team2 с отдельными `LocationNameSid`; значения по умолчанию `TEAM 1 Untitled` и `TEAM 2 Untitled`. Тождество с `CapturePointEntity.name` или конкретным `ID_H_US_B` не доказано. Без восстановления VU-геттера/нативного CP имени выбирать первый child нельзя.

Проверен существующий VU decomp в области ServerCapturePointEntity и прежний `re/capture`; установлен type-info resolver `FUN_10E07C10`, но точный getter имени и его native-цепочка в рамках этой узкой проверки не восстановлены. Это открытый вопрос, не основание выдумывать offset.

**Позднее уточнение в этой же сессии:** дополнительная точечная сверка восстановила VU getter `FUN_10E06C60`: он запрашивает link-свойство CP.Spawners (`0xECEA613A`), выбирает связанный CharacterSpawn и читает runtime LocationNameSid. Это конкретная связь, а не первый сосед в bus. Серверный getter имени заданного спавна `0x00E44F00`, запрос связей `0x005E1260`; точный ABI, контейнеры и cleanup описаны отдельно в `capture-point-name-2026-11-09.md`. Предыдущий абзац сохраняет границу первого прохода; вопрос native-источника теперь разрешён статически. Игровой проверки по-прежнему нет.

Подтверждённые новые пункты: несовместимость имён gunship и несогласованный fallback режима при пустой опции. Остальные перечисленные пути не дали нового подтверждённого дефекта нашего слоя в пределах source-проверки. В игре ничего из этой матрицы не проверялось.
