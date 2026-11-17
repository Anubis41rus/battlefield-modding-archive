# Настоящее имя CapturePointEntity — 09.11.2026

Статическая сверка по локальным veniceext.dll, zbf3.exe и bf3_server_Final.exe. Код игры не выполнялся, новый Ghidra-анализ не запускался, исходники не менялись.

## Результат

**VU действительно получает имя точки из LocationNameSid связанного CharacterSpawn, но предварительно обходит link-свойство CP.Spawners.** Брать первый произвольный child в bus нельзя. Отдельного поля `name` в CapturePointEntityData для этого нет.

Точная VU-цепочка находится в уже удержанном `re/vu/decomp/chunk01.decomp.txt`, `FUN_10E06C60` (строки 1284655–1284815). Функция разрешает `ServerCapturePointEntity`, строит запрос по ключу `{cp.data, 0xECEA613A}`, фильтрует `SpawnReferenceObjectData`, затем среди результатов ищет `ServerCharacterSpawnEntity` и копирует его runtime `LocationNameSid` в std::string. Если подходящего связанного спавна нет — возвращает пустую строку. Область регистрации класса — `0x10E07510`; связанный getter записывается в `0x10E0762F`, свойство `name` — строка `0x177C1E18` в `0x10E07867`. Прочие строковые свойства у CP в локальной Docs отсутствуют.

`0xECEA613A` = -320184006, имя **Spawners** подтверждено в `Venice-EBX/Gameplay/Level_Setups/Components/CapturePointPrefabFlagpole.txt:623–633`. Это LinkConnection, а не просто равенство bus. В этом prefab есть два CharacterSpawn и отдельная ссылка Vehicles; фильтр типа в VU существенен.

## Чтение имени заданного спавна

- VU slot `0x18E751BC` инициализируется значением клиентского `0x00A298E0` по `veniceext:0x1070256A`.
- Клиентский getter `0x00A298E0` получает контекст bus, читает data через property-reader `0x01FCD000` и разрешает runtime override; возвращает **адрес fb::String**, то есть `const char**`, затем VU разыменовывает один раз в CString.
- Точный серверный аналог — **`0x00E44F00`**, `thiscall` с единственным `this=CharacterSpawnEntity`, без стековых аргументов, `ret` без очистки. В C++ вызов допустимо описать `const char** (__fastcall*)(void* self, void* unusedEdx)`.
- Он вызывает `0x02351500`. Сверено по байтам: `lea eax,[ecx+0x120]`, хеш `0x58D1C621` (**LocationNameSid**), `ret 8`. Клиентский `0x01FCD000` имеет ту же структуру с тем же offset/hash. Прямое чтение `[data+0x120]` обходит runtime override и не равно getter.
- Пролог серверного getter: `83 EC 14 56 8B F1 8B 4E 10 8B 01 8B 40 30 8D 54 24 0C 52 FF D0 8B 4E 14`.
- Полный короткий getter: `mode-spawn-name-getter-20261109.txt`. Вспомогательное сравнение кандидатов — `mode-native-name-candidates-20261109.txt`, прямой native query — `mode-native-cp-query-20261109.txt`.

## Точный запрос CP.Spawners на нашем сервере

Нативный CP делает ту же операцию в `0x01337AE0` (`re/capture/c2.txt:660–737`, свежая capstone-сверка — `mode-native-cp-query-20261109.txt`). Минимального отдельного CP:getName в найденной цепочке нет: VU сам обходит связи.

Воспроизводимая последовательность без нового патча:

1. Сохранить `data=[cp+0x14]` и `bus=[cp+0x10]`, проверить тип CP и живые указатели.
2. Создать scratch arena размером **0x18**, `0x00440B90`: `thiscall(arena) -> arena`, без стековых аргументов. Функция меняет TLS allocator context.
3. Создать fixed result list размером **0xA48**, `0x00E49200`: `thiscall(list, &arenaPtr) -> list`, `ret 4`. Размер подтверждён разницей нативных стековых размещений: list=`esp+0x1AC`, arena=`esp+0xBF4`.
4. Получить bus context (8 байт; буфер можно выделить с запасом) виртуальным методом **bus.vtable+0x30**, `thiscall(bus, contextOut)`.
5. Вызвать **`0x005E1260`**, `cdecl`, шесть аргументов:
   `QueryLinks(arenaPtr, &key8{data,0xECEA613A}, SpawnReferenceObjectDataTI=0x02C02D9C, &busContext, bus, list)`.
   Нативный вызов `0x01337B2E–0x01337B5A` очищает 0x18 байт стека после возврата.
6. `list[0]` — первый node; конец — сам адрес list. Node имеет шаг/размер **0x28**. Следующий node — `[node]`.
7. **`0x005CD2D0`**, `thiscall(node+8, 1) -> entity`, `ret 4`, воспроизводит VU lookup с флагом 1. Тот же helper в CP использует 0 для собственных действий; для имени VU явно передаёт 1.
8. Отфильтровать `ServerCharacterSpawnEntity` по TypeInfo **0x02C16B84**, затем вызвать **`0x00E44F00`** и скопировать полученный CString с ограничением длины. Первая подходящая связанная сущность — поведение VU; команда как отдельный параметр не передаётся.
9. Cleanup, точный порядок нативного `0x01337C64–0x01337C9A`:
   `0x00E44030(list)`;
   `list.next=list.prev=&list`;
   `0x0043F510(arena)`;
   `0x00E44030(list)` ещё раз.

Arena ctor/dtor: `mode-query-arena-20261109.txt`; list ctor: `mode-query-list-20261109.txt`; cleanup: `mode-query-cleanup-20261109.txt`.

## Требования и границы

- Scratch arena обновляет поле TLS контекста `+0x118`. Требуется корректный game TLS/поток движка; это не безопасная замена обычного field-read на произвольном Lua-таймере. Вызов и cleanup должны выполняться в одном подходящем потоке.
- Cleanup обязателен и при исключении; копировать имя нужно до очистки. Каждый новый прямой native-вызов должен быть закрыт проверкой кода по принятому в проекте правилу. Ни один из этих вызовов не требует изменения игрового exe.
- `0x005E1260` может собирать связи через вложенные prefab/bus, поэтому ручное сравнение соседей bus не воспроизводит контракт.
- Эта последовательность подтверждена статически по двум независимым реализациям (VU wrapper и нативные CP операции). Она ещё не прогонялась внутри EVBots. Нельзя объявлять игровой gunship исправленным до проверки интеграции.
