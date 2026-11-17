# TeamInfo: 128 команд для Royale, 17.11.2026

Реализованы `C:/Games/EVBots-dev/evbots-dll/src/TeamCapacity.cpp` и `.h`, родитель интегрировал раннюю установку. Итоговый серверный пакет содержит13 согласованных изменений, клиентский5. Контракт памяти и жизненный цикл проверены машинным кодом, MSVC x86 fixtures и серверным прогоном родителя05:27:45 (DLL70D7CD66): runtime GameModeSettings127, регистрация TeamInfo id126/count127, XP5_003 finalized, VEH0/ERR0, 30Hz. Этот дочерний агент игры не запускал и память процессов не менял. Полный клиентский Ready/матч остаётся отдельной проверкой.

## Источники

Локальный статический анализ pefile/Capstone, без сети/REA MCP/записи в процессы.

| Артефакт | SHA256 |
|---|---|
| `D:/Games-cache/royale/server-runtime-v3/bf3_server_Final.exe` | `f39f7623063cfde9c449a0bdc9a57448d2d137d6def00b17ea04800eda955470` |
| `C:/Games/Battlefield 3/zbf3.exe` | `8201c5e14667d511f402c424d5c87a3a5ed2d9fdf81c67c914a145a9d3bc42b9` |
| `%USERPROFILE%/AppData/Local/VeniceUnleashed/client/vubin/vu-core.dll.ev-orig` | `44f4dd2a55362ad63e7b5b4f060c8411a569544c3a0981bea3591737f0b9f163` |

VA ниже — preferred image base `00400000` для exe, `10000000` для VU core. Адаптер прибавляет delta фактической базы.

## Почему прежний сервер портил память

TeamInfo vanilla — 17 указателей по 4 байта, затем count по `+44`, размер `48`. Серверный setter `00880DD0` проверяет id<17, но после диагностики/assert продолжает запись:

```
00880F67 mov [edi+esi*4],ebp
00880F6A mov ecx,[edi+44]
00880F6D lea eax,[esi+1]
... max(oldCount,id+1)
00880F76 mov [edi+44],eax
```

Поэтому id17 сначала пишет TeamEntityData* поверх count, а id18 и далее выходят за allocation. Отключение assert или один cmp-патч память не расширяет. Диагностика duplicate после первой ошибки закономерна для повреждённого объекта. Связь последующего DEP EIP0 с конкретным повреждённым указателем дампа не доказана этим анализом; причина выхода за границы доказана напрямую кодом.

## Что делает VU

VU использует retail layout и hook installer `1018758C`, включая:

| Vanilla retail | VU replacement | Действие |
|---|---|---|
| `011D5960` | `10199290` | Выделить `204`, нулить 128 slots и `+200` |
| `011D06A0` | `101992D0` | Записать `[this+id*4]`, inc `[this+200]` |
| `011CC4B0` | `101992F0` | Вернуть `[this+200]` |

Ключевые инструкции VU constructor: `push 204; call [108D0D78]; mov [edx+200],0; mov ecx,80; rep stosd; mov eax,edx; ret`. Адрес allocator-global устанавливается `1012920A: mov [108D0D78],00424B90`, то есть это вызов native функции клиента, а не предположение о CRT malloc.

VU setter сравнивает `cmp edx,80; ja ...`, то есть пропускает также id128, который перекрывает count. Адаптер намеренно принимает только 0..127; Royale требует максимум126. VU setter считает регистрации через inc, а не max(id)+1. Для валидных уникальных TeamEntityData это поведение сохранено. Повторный id в адаптере даёт явный ERR и сохраняет первую регистрацию/count; VU увеличивал count повторно. Это защитное отличие относится только к ошибочному входу, не к нормальной загрузке Royale.

VU constructor возвращает новый allocation вместо полученного `this`. В нашем адаптере такого второго allocation нет: точечно меняется размер исходного вызова native allocator. Это сохраняет точную пару allocator/free и не оставляет старый блок48 байт.

## Создание и освобождение

| Этап | Dedicated server | Retail client |
|---|---|---|
| Создатель Level | `00851FD0` | `0119B790` |
| Native allocation CALL | `00852000 → 00430700` | `0119B7BF → 0041BE10` |
| Аргументы | this=allocator, size48, align10, tag0; ret0C | this=allocator, size48, align10; ret08 |
| Единственный direct ctor CALL | `0085200B → 00885370` | `0119B7CA → 011D5960` |
| Сохранение указателя | `00852017: Level+68` | `0119B7D6: Level+64` |
| Удаление Level | `00850090` | `0119A290` |
| Native free | `008500A7 → 0042B190`, allocator Level+34, ptr Level+68, tag0 | `0119A2A5 → 00418290`, allocator Level+34, ptr Level+64 |

Dedicated free извлекает размер/offset из allocation header (`0042B1D3 [ptr-4]`), а не из константы48. Размер204 с тем же allocator/alignment поэтому штатно освобождается. TeamInfo не содержит vtable, владения TeamEntityData или собственного destructor: Level освобождает только этот блок. Constructor возвращает `this`, поэтому замена сохраняет указатель на единственный native allocation.

## Обращения к данным

Native direct-call audit всех известных функций выполнен по executable section; адреса-кандидаты проверялись дизассемблированием и сравнением с VU/retail. Никакие непохожие функции с таким же коротким getter не патчатся.

- Server getter `00880D30`: граница17 и `[this+id*4]`; заменён полностью на checked128. Его прямые callers: `8853A5,8853D0,8854D5,EB2FA4,EB2FB7,FA7060,1185885,124874F,124FECE,136AB7C,136AC24,136B4B3,136BB8C,136BCD0,136BE09,136C082,136DED6,136F7CB,13713B6,1371756,1373491,13740CD,137475B,1374BB3,1374C8D,1375034,1375293,137B63E,13C80C9`.
- Server setter `00880DD0`: все три callers `DCF651,FB409A,FB9E1C` перехватываются общей заменой.
- Server count `0087AFD0`: все11 callers `EA0798,EA080C,EB2BB0,EB2D0B,EB2D21,11712F1,118D025,118D05E,118D138,118D1B1,124873A`. EA/EB/117/124 получают TeamInfo через Level+68. Семейство118D находится внутри `118CFB0`, принимающей TeamInfo аргументом; её direct caller `12ED1F8`. Retail аналог consumer1248720 — `7799B0`, и он вызывает именно VU-перехваченный `11CC4B0`.
- Client getter `011D0690` уже адресует указатель по full32-bit id; VU оставляет его штатным. Мы заменяем его для явного bounds128, сохраняя ABI ret4.
- Client setter `011D06A0`: callers `A5DA51,10607BA,1065A0C`.
- Client count `011CC4B0`: единственный прямой caller `7799CA`, как в VU.
- Другой client getter `011D07E0`, также `[this+44]`, не относится к этому hook: его callers `723DC5,724299,7243C2,724492`; например723DC5 получает другой объект через owner+60. Он не меняется, как и в VU.

Это доказательство охватывает TeamInfo allocation/accessors/lifecycle в проверенных binaries. Оно не является доказательством всех прочих систем команд (сетевая сериализация, scoreboard, squad и т.п.). Их возможные отдельные лимиты нельзя автоматически считать исправленными этим модулем. Здесь не внедрены guessed patches других подсистем.

## Изменяемые диапазоны

| Назначение | Server VA/len/bytes | Client VA/len/bytes |
|---|---|---|
| ctor | `885370 /12 /8bc1568d7044c70600000000` | `11d5960 /12 /8bc1568d7044c70600000000` |
| set | `880dd0 /14 /81ec30010000558bac2438010000` | `11d06a0 /8 /8b4424088b542404` |
| get | `880d30 /14 /81ec30010000568bb42438010000` | `11d0690 /10 /8b4424048b0481c20400` |
| count | `87afd0 /8 /8b4144c3cccccccc` | `11cc4b0 /8 /8b4144c3cccccccc` |
| allocate CALL | `852000 /5 /e8fbe6bdff` | `119b7bf /5 /e84c0628ff` |

Каждый диапазон покрывает целые инструкции; count включает штатный INT3 padding после ret. Все адреса и байты сверены `team_capacity_verify.py`: PASS. Перед runtime-записью все пять диапазонов и12 байт аргументов allocation проверяются полностью; хотя бы одно несовпадение — ни одной записи. Запись идёт через существующий `PatchBytes`/ev-core, с expected bytes. Ошибка во время установки откатывает уже поставленные точки с журналом. Это атомарность на этапе startup: установка разрешена только пока поток движка ещё не создаёт Level. Если rollback не удался, возвращается false с FATAL; продолжать engine startup нельзя.

## Проверки

`build_team_capacity_fixture.cmd` собрал отдельный32-битный exe MSVC18. Это тестовый процесс, не игра. Пройдены:

- 128 уникальных регистраций в обратном порядке, чтение каждого указателя, count128;
- id17,126,127 не портят соседнюю память; guard до/после204 сохранены;
- id128/-1 и duplicate17 диагностированы, не изменили valid storage/count;
- повторный ctor очищает все128 slots/count;
- реальные detour-функции вызваны через native `__thiscall` типы: this вECX, аргументы на стеке, Get ret4, Set ret8, Count/Ctor ret0;
- native allocator mock получает size204/alignment10, серверный третий аргументtag0 сохранён;
- ESP до/после всех ABI-вызовов совпал.

Результаты `TEST TeamCapacity_thiscall_stack_and_allocator PASS` и `TEST TeamCapacity_file_bytes_and_instruction_boundaries PASS`. ASM скомпилированного fixture: `team_capacity_fixture.asm`. Негативные тесты специально печатают ERR — это ожидаемая проверка, не runtime регрессия.

## Интеграция родителем

1. Добавить `TeamCapacity.cpp` в действующий список компиляции; include `.h` в раннем bootstrap. Существующие файлы ребёнком не менялись.
2. Вызывать `InstallTeamCapacity(isDedicatedServer)` в обоих процессах ДО первого Level и до работающего engine thread. До вызова доступны ev-core hooks/logging. При false остановить Royale startup; не запускать игру с ConquestLargeModifier126 при штатном48-byte layout.
3. `EVBOTS_TEAM_CAPACITY=0` — явное выключение; функция возвращает false и объясняет невозможность расширенного Royale.
4. `TestTeamCapacityMemory()` можно включить в тест-хост; она не обращается к памяти движка.
5. После первого расширенного allocation нельзя снять модуль/патчи или выгружать DLL. Полный restart процесса — необходимый путь отключения. Не вызывать существующий массовый откат byte patches при живом Level.
6. Runtime критерии: оба процесса сообщают5 verifyPASS и installed; allocation516; регистрация17/126 без TeamInfo.cpp range/duplicate; уровень и Ready проверяются заново. Финальная runtime проверка ребёнком не выполнялась.

Дополнительно по review родителя: до сверки и непосредственно перед записью проверяется отсутствие Level. Server chain: `*[02877210]→+D0 server→+74 currentLevel/+7C loadingLevel`; client: `*[022E3790]→+B4 client→+C context→+10 Level`. Ненулевой Level либо исключение чтения запрещают установку. Это защитная проверка позднего вызова, не замена синхронизации startup с потоком движка. Перед первой записью выполняется проверенный `GetModuleHandleEx(FROM_ADDRESS|PIN)` для DLL с обработчиками: byte patches не входят в HookRegistry, потому DLL нельзя выгружать при их наличии. После этих дополнений x86 fixture пересобран, PASS.

## Следующий серверный блокер и lifecycle resources

Родитель выполнил серверный прогон 05:05:29 DLL2E834F25: TeamCapacity128 PASS, ошибки id17 больше нет. Следующая диагностика — ServerPlayerSquadManager.cpp304, `No team size information available for team (Team3)`.

Native `0118CFB0` вызывает `0116A050`, затем перебирает count TeamInfo и запрашивает `GameModeTeamSize` из массива структуры GameModeSize. `0116A050→01164A20` использует GameModeSettings, cached pointer `02B5D7A4`, лениво получаемый `01161220` через SettingsManager type `02C106C0`. Settings+0C — information, первый GameModeInformation+4 — sizes, GameModeSize stride1C, teams+10. Входной массив оставался3, а мод оригинально расширяет его до127 через `GameModeSettingsModifier.lua` (GameModeTeamSize playerCount127/squadSize4). Поэтому новый guessed native squad patch не вводился.

Лог05:05:07.788 показал GetSettings(GameModeSettings)=0 на раннем Extension:Loaded. Handler cb54 на C4DCACFF|AD413546 зарегистрирован, но не выполнен и не падал. Проверен native getter: `0053FBC0` — TLS scope вокруг lookup `0053F7A0→0053D880`, без создания отсутствующих settings. Scan EngineResources использует правильную hash-map+60, узлы key/value/next по0/4/8. Проблема связана с моментом доставки: существующий endPostChunks перебирает лишь текущий compartment, ранее загруженные глобальные partitions не доставлялись после ранней регистрации watches.

В `EngineResources.cpp` после backup `C:/Games/backup/2026-11-17/royale-spawn/EngineResources-webloading-pre.cpp` внесено:

- Native `Level:LoadResources` сначала выполняет catch-up уже загруженных watched partitions по всем существующим compartments. Только новые по штатному `TakeDelivered`; `Partition:Loaded` повторно не рассылается. Callback вызывается в native InstanceLoad scope, на том же потоке движка. Lock не удерживается во время Lua. Следующий проход подхватывает новые watches, зарегистрированные обработчиком.
- Read-only диагностика печатает pointers и teams-count runtime GameModeSettings и GUID-asset после catch-up. Она позволяет в следующем игровом прогоне проверить, что изменился активный объект, либо выявить отдельный clone.
- `ReadLoadIdentity` принимает отсутствующий либо пустой GameMode. VU core `101A1940` создаёт пустую строку, ищет key GameMode, при отсутствии выходит из поиска на101A19FA и всё равно посылает event E660B83E на101A1B11/101A1B1D; original вызывается позже101A1B55. GameMode не подменяется строкой Conquest. У Web_Loading событие теперь может выполнить штатный wrong-map unsubscribe мода до RegisterEntityResources.
- Реально нечитаемые options/значения и неверные размеры буферов остаются ошибкой. Guard null/cap стоит перед первой записью.

Собран `EngineResources.cpp` отдельно MSVC x86, PASS. Fixture `resource_lifecycle_fixture.cpp` использует извлечённые функции production (файл `resource_lifecycle_extracted.inc`): PASS для fresh/dedup across domains, рекурсивного watch, native callback scope, пустого/некорректного domain; missing/empty/valid/unreadable GameMode. Runtime результат этих исправлений ожидается от следующего родительского прогона; наличие127 в активном settings пока не утверждается.

## Уточнение после прогона998275EB и второй storage команды

В прогоне родителя05:14 catch-up delivered0, runtime GameModeSettings F79AF750 teams3, GUID asset отсутствует. Следовательно catch-up сам не решает ранний Extension:Loaded: действующий runtime объект появляется позже. Родитель внедрил one-shot EnsureNativeModStartup; минимальный hook ребёнка в VextResourceManager вызывает его внутри DcCallbackScope в OnLevelLoadResources, затем публичный `eng::DeliverPreloadedWatchedPartitions()` и только затем обычный LoadResources event. Ранний вызов catch-up из EngineResources Detour удалён. Публичная обёртка проверяет native Load phase. Таким образом штатный GameModeSettingsModifier получает готовый runtime объект при единственном Extension:Loaded; второй dispatch события не добавляется. Родитель отдельно занимается fallback на настоящем engine frame для горячего Lua restart.

Ещё до следующего игрового прогона статический аудит выявил отдельный опасный fixed_vector сервера:

```
0285BB38 begin =0285BB48
0285BB3C end   =0285BB48
0285BB40 limit =0285BBD0
0285BB44 allocator =0
```

Ёмкость `(BBD0-BB48)/8=17`. Native registerTeams118D304 пишет8 байт через end и двигает end без reserve. Исправление runtime teams127 без переноса этого массива повредило бы соседние globals. Пакет TeamCapacity теперь включает атомарно проверяемую16-byte замену header на `gTeamSizes[128]` в закреплённой DLL; allocator0 сохранён. Это статический fixed_vector: нет прямых code refs к capacity/allocator/inline backing, только указателиbegin/end. Все43 абсолютные refs кbegin и37 кend сгруппированы по16 функциям и проверены скриптом team_size_audit.py. Исходный inline-buffer имеет только две ссылки изheader; original capacity — только одну. Ничего не освобождается и не выделяется сторонним allocator: статическое хранилище живёт до завершения закреплённого процесса.

VU использует собственный Balancing::TeamSquadManager2 (RTTI108C95F4), а не vanilla fixed_vector17. `105252F0` выделяет420 байт, begin/end=this+28, capacity=begin+3F8, то есть127 entries по8. `105259F0` читает GameModeSettings sizes и наполняет эту таблицу; `10525390` возвращает squadSize, `10525450` — число записей. Наш storage128 согласован с TeamInfo128 и покрывает все127 команд Royale. Это эквивалент расширенной ёмкости, не перенос всей VU балансировки.

Скан статических inline fixed_vector17x8 по всем неисполняемым секциям обеих PE нашёл только server0285BB38; совпадающего объекта в retail client нет. Client остаётся с пятью TeamInfo изменениями; серверный squad-manager глобал туда не переносится. Отсутствие этого конкретного layout не означает отсутствия любых других ограничений команд в клиенте.

Аудит потребителей выявил второй локальный буфер `int[17]` в1175A60. Агент visual_environment доказал расширение кадра178→338, сдвиг четырёх обращений к аргументам на1C0 и два соответствующих epilogue. Log-buffer+58 используется в другой ветке strategy и не одновременно с индексируемым массивом; memset44 можно сохранить, потому цикл1175B10..B39 записывает все индексы1..count-1 до единственного читающего цикла1175B50. Полные инструкции, включённые в тот же all-or-none пакет:

| VA | До | После |
|---|---|---|
|1175A60|81EC78010000|81EC38030000|
|1175A6B|8B8C247C010000|8B8C243C030000|
|1175A8A|80BC249001000000|80BC245003000000|
|1175AEF|8B9C2494010000|8B9C2454030000|
|1175C88|81C478010000|81C438030000|
|1175D28|8B9C2494010000|8B9C2454030000|
|1175D8C|81C478010000|81C438030000|

Итого сервер13 точек (5 TeamInfo +1 header TeamSize +7 selector), клиент5. При несовпадении любой точки не пишется ни одна; rollback покрывает весь пакет. Backup до этого этапа: `TeamCapacity-teamsizes-pre.cpp` в royale-spawn.

Дополнены file/Capstone проверки: исходный header17, все7 инструкций и неизменность длины после patch — PASS. Fixture выполняет настоящие26 байт native118D304..31D с перенастроенными адресами end в собственной памяти тестового процесса;128 последовательных вставок `{127,4}` дали end==capacity, оба guard сохранены: `TEST TeamSize_native_insert_128_slots_and_guards PASS`. Это не запуск игры. Полный selector fixture выполняет другой агент; runtime запуск ожидает родитель.

Review lifecycle завершён: все80 raw absolute refs к TeamSize begin/end/cap/allocator/backing разобраны в инструкции, missing0. Единственные записи —118D0BC `end=begin` и118D30F `end+=8`. Измененияbegin/cap/allocator либо возврата наoriginalinlinebuffer нет. Вне executable ссылок на адрес header нет; inline backing285BB48 ссылается только из двух initial pointer values, capacity285BBD0 — из одного. Не найдено CRT destructor/free или передачи header адресом: native не воспринимает перенесённый staticstorage как отдельную динамическую allocation. Память DLL закреплена PIN.

Агент visual_environment сообщил7209 PASS/0 FAIL в x86 /W4 /WX selectorfixture:1200 differential scenarios исходного/исправленного кода для≤17 slots,1200 expanded scenarios18..128 против независимой модели, чтениеcount127,110152 вызоваCount,281 diagnostic paths (после расширения теста неизвестного mode), сохранениеESP/EBX/ESI/EDI. Подробный контракт: `team-selector-128-proof.md`. Игровой процесс этим fixture не запускался. После этого пакет отмечен родителю готовым к серверному прогону.

## Результат серверного прогона и предел native балансировки

Родительский прогон05:27:45, DLL70D7CD66, server PID320/backend5380: GameModeSettings runtimeF79AF750 teams127, watched catch-up1; карта XP5_003 загружена и finalized, VEH0/ERR0, 30Hz. Read-only spawn_probe обнаружил5 character spawns, нужный Royale GUID67A2C146 создан и active. Это закрывает прежние out-of-range17, отсутствие teamSize3 и переполнение native fixed_vector. Работа кнопки Ready и клиентской внешности этим результатом не утверждается.

Остались32-bit team masks в `ServerPlayerSquadManager::balanceTeams`1190260:11904A5..AE и11905E4..ED (`1 << team`). Единственный прямой вызов1190260 —12F5286 при первом player-entry обработчика сообщенияF524339E; перед ним onNewRound117EA40, затем enterPlayer1190D30. Блок масок запускается только если уменьшилось число команд (global2B5D7B4, write118D3BF) либо менялась стратегия с предыдущей0 (2B5D7B5, write118D534). При расширении первоначального списка до127 первое условиеfalse; первоначальная отсутствующая strategy имеет sentinel3, поэтому второеfalse. При последующих уменьшениях списка до32+ rotate-mask насыщаетсяFFFFFFFF и не различает high teams. Это отдельное ограничение vanilla смены режимов/автобаланса; пакет не переносит всю VU TeamSquadManager2.

Оригинальный Royale выставляет `vars.autoBalance=false`, а BRTeamManagerServer назначает TeamId2..100 явно через BRPlayer.ApplyTeamSquadIds. Native setter/addToTeam не вызывает balanceTeams; сообщение E207A0→E20540 имеет hashF3C93DD3, отличный от entryF524339E. Поэтому обнаруженные маски не являются блокером этого explicit Ready/назначения команд. Смена режима с уменьшением числа команд требует отдельной проверки/переноса VU manager, универсальная корректность такого сценария не заявляется.

Во время этой проверки обнаружено, что прежние Lua Player.teamId/squadId жили только в raw table. Исправлены в отдельном текущем пакете; см. `player-properties-and-human-appearance.md`.
