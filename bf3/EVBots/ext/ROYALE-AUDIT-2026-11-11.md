# Battle Royale для EVBots — проверка 11.11.2026

## Запрос и результат

**Текущий checkpoint:** зависимости и оригинальные shared/client/server init
загружаются; Royale.Compat 34/34 и core 89/89. Собраны UI/Compass, выбор профилей
в Launcher, native resources/Blueprint/Clone/Spectator. Полная DLL:
`6DDFD5023CAF6D6BC2A89C69C7ED6316DAC8961BC56D146AB532F1F43E532F8C`.
Настоящий WebView без игры показал инвентарь, иконки и работу focus-чата.
Рабочая копия мода получила LAN-конфиг и исправление зависания раунда без выживших;
оригиналы upstream сохранены отдельно. Последующие value types/collision/terrain
ещё в работе. Игрового матча и итогового установщика пока нет. Актуальные детали —
верхние записи STATE/MODLOG; описания прежних ошибок ниже оставлены как история этапов.

### Первоначальная проверка (история)

Пользователь хочет переключаться между fun-bots и королевской битвой без ботов
из нашего комплекта. У пользователя и друга нет архива: друг видел ролик YouTube.
Найден наиболее подходящий проект BF3BR/VU-BattleRoyale. Точное совпадение с роликом
не установлено: ссылки на ролик в разговоре нет.

**Рабочий Battle Royale не установлен и новый установщик не собран.** Проверка
обнаружила ошибки загрузки на нынешней EVBots.dll и необходимые API-заглушки.
Простое добавление папки мода и переключателя не даёт работающий режим.
EVBots, Battlefield LAN, BF-Portable и fun-bots не изменены; игра не запускалась.

## Продолжение 11.11 — работа в текущих исходниках, без развёртывания

По следующему запросу пользователя перенос начат в исходниках EVBots. Оригинальные
Lua-файлы Royale остаются неизменными. Добавлены 9 enum-таблиц по VU-Docs (включая
VoipTransmissionMode и SpectatorCameraMode), SettingsManager с сохранением данных
вне папки мода, корректный отказ при ошибках init и событий запуска.

Ревью выявило и исправило восстановление GetSetting до Declare (оригинальный
Royale иначе перезаписывает настройки значениями по умолчанию), изоляцию по имени
папки вместо отображаемого Name, передачу владельца мода в Hooks, JSON-ключи с NUL.
Проверены пути отказа/restart, сохранение нативных process-lifetime hooks при
перезапуске Lua и отмена запуска WebUI без второго живого потока. Лаунчер теперь
распознаёт LUA-MODE FAILED сразу. Эти изменения сами по себе не дают работающий матч.

Пользователь явно выбрал **именно оригинальный VU BattleRoyale**, отвергнув
предложенный облегчённый режим. Далее он отдельно разрешил параллельных агентов,
попросил использовать готовые материалы `re/`, улучшить оригинальный мод и следить
за C:. Упрощённый собственный режим не создаём.

Последняя связанная DLL до native resource hooks: x86,
SHA-256 `06BEF026CCB71E2B903A02C990531EDF80DA22F02E59D7744F529E6D0752413A`.
Royale.Compat — 30/30, включая оригинальные shared/client init и Extension:Loaded.
При Extension:Unloading оригинала ещё отсутствует SpectatorManager.
Royale.Server — original server init PASS без игры. Core-only EVBots.Test — 89/89,
fun-bots integration явно SKIP. Один первый запуск упал при FreeLibrary в
ntdll!RtlFreeHeap+0x1c; под отладчиком и 35 повторами не воспроизведён.
Это **не признано исправленным**. Логи: `D:/Games-cache/royale/core-*.txt`.

После этой сборки найдены в уже готовом `re/vu/veniceext.strings.txt:28669`
ещё два дефекта MiddleClass: VU class.class публикует `_G[name]`, а наша обёртка
после lua_setglobal повторно читала уже снятый элемент стека. Исправлены в исходнике;
проверка добавлена в Royale.Compat, общей новой сборки на этот момент ещё нет.

Зависимости: скачан и проверен VEManager v0.2.0-rc.1,
commit `46b5c213155381b1cab812535b80ca87f8708ca6` (16 файлов, 121473 байта).
Его исходный init опирается на глобальные классы; именно здесь найдена ошибка выше.
Для отсутствующего частного Compass найдена MIT-основа того же автора breaknix:
kapraran/vu-compass, `f928fe86526e85d88af44a77705d3fedc8305232`, 1.4.1;
29 файлов / 224718 байт, Git blob SHA-1 совпали. Нужна совместимая реализация
Compass:CreateMarker/RemoveMarker, в публичной основе этих событий нет.
Оригиналы зависимостей — `D:/Games-cache/royale/dependencies-original/`,
рабочие копии — `D:/Games-cache/royale/working/Mods/`.

UI: 199 исходных blobs / 8,34 МБ проверены в `D:/Games-cache/royale/ui-build/`.
Собирается оригинальный React UI с локальными loading images, определением
WebUI.Call по наличию функции и GameAssetUrl с VU-fallback fb://.
Оригинальная UI-карта — только XP5_003 (Kiasar Railroad); Alborz пока не предлагать.
fb://-текстуры предстоит читать из установленной игры, не включать в публикацию.
Для анализа взят MIT Frostbite-Scripts `a07ee6d38ecc62598dfc9fdf990be067d9f21929`;
не запускать его полный dumper (он дампит всё и удаляет свою temp-папку).
Нужна выборочная обработка UI.toc/CAS с небольшим кешем на D:.

### Найденные нативные операции — статические данные, НЕ готовые патчи

| Операция | Обёртка VU | Слот VU → zbf3 | Dedicated server |
|---|---|---|---|
| MountSuperBundle | 10BFCAE0 / 10BFCAC0 / 10BFCAA0 | 18E75020 → 004BC7C0 | 0054A9F0; единственное совпадение 28 байт с маской direct call |
| AddRegistry | 10BFBAE0 | 18E74FB4 → 005760F0 | 006040C0; структурное сопоставление массивов +8/+C/+10/+14 и регистрации |
| Blueprint client | 10C83C60 / 10C839F0 | 18E74E24 → 00FA4A60 → 00FA15B0 | — |
| Blueprint server | 10C8CDF0 / 10C8CB30 | 18E7508C → 00A8E150 → 00A8B4D0 | 00DB3530 → 00DB03B0; именованная assert-запись |

Референс регистраций: `re/client-k1/regdis.txt:320`, регистраторы 10BFCB30,
10C84890, 10C8D990. Таблицы и функции дополнительно сверены Capstone по бинарникам.
10C8D4D0 из slotusers-client относится к CreateEntity, не к Blueprint.
У dedicated 00DB356E ссылается на assert record 02C55568: ServerEntityFactory.cpp,
строка 502, `fb::ServerEntityFactory::createFromBlueprint` (025B5690).
Конструктор ServerEntityFactoryParams — 00D90080, record 02C54550, строка 50;
transform копируется в params+30 размером 0x40. У VU вызов фабрики имеет форму
cdecl(outBus, nativeParams, blueprint, parentRepresentative), вокруг него есть
переключение контекста/TLS и refcount. При networked=true нужна дополнительная
регистрация IDs/подуровня, а после создания — настоящие EntityBus/Init/Destroy.

AddRegistry dedicated проверяет [02B1BBCC]!=0; диагностическая строка 02474008
запрещает использование вне NetworkDataRegistry::initLevel/exitLevel (0060414D).
Значит адрес нельзя просто вызвать из произвольного Lua-тика. В EVBots ещё нет
необходимых событий/хуков Level:LoadResources, Level:RegisterEntityResources,
ResourceManager:LoadBundles, Terrain:Load, VisualTerrain:Load.

SHA-256 исследованных файлов:
- veniceext.dll: `c34ad1823f662b1a7515ae28d0f646d633f9b8f2b7ad6dd1e9df7922fd912ee8`;
- zbf3.exe: `8201c5e14667d511f402c424d5c87a3a5ed2d9fdf81c67c914a145a9d3bc42b9`;
- bf3_server_Final.exe: `f39f7623063cfde9c449a0bdc9a57448d2d137d6def00b17ea04800eda955470`.

Runtime-проверок этих трёх операций не было, адреса в исполняемый код не внесены.

## Зафиксированный оригинал

- Репозиторий: https://github.com/BF3BR/VU-BattleRoyale
- Ветка development, commit `f5212d7d82adb4ee9b3a9d28e52d5186ecc01cc8`.
- В mod.json: Version 1.0.0; авторы KVN, Flash_Hit, breaknix, Janssent, kiwidog.
- Готовых GitHub Releases нет: https://github.com/BF3BR/VU-BattleRoyale/releases
- Руководитель проекта сообщает о приостановке разработки:
  https://kvn.hu/battle-royale-website/ (там же ссылка на трейлер).
- LICENSE в дереве этой ревизии не найден; поле license GitHub API — null.
  Лицензия на перенос/распространение не установлена, авторское разрешение не получено.
  Исходники использованы только для локального исследования и не включены в dist.
- `HasWebUI=true`, зависимости `veniceext ^1.4.0`,
  `VU-BattleRoyale-Compass ^1.0.0`, `VEManager ^0.2.0-rc.1`;
  `RequiredXPacks=[1,3,5]`.
- VEManager найден: https://github.com/BF3RM/VEManager . Совместимая ревизия ещё не выбрана.
  Compass в публичных репозиториях BF3BR не найден; адрес
  https://github.com/BF3BR/VU-BattleRoyale-Compass возвращает 404,
  поиск GitHub repositories по точному имени вернул 0. Это не доказывает отсутствие
  приватной или переименованной копии.
- В ревизии нет собранного ui.vuic; есть React/TypeScript UI, react-scripts 4.0.3,
  node-sass 6.0.1 и vextpack ^0.1.5. UI и npm-зависимости не скачивались целиком и не собирались.

## Проверки и доказательства

Материалы: `C:\Games\trash\2026-11-11\royale-audit\`.
Это текущие материалы исследования; не считать их ненужным мусором при уборке.

1. `tree.json` — дерево зафиксированного commit. В `source\` скачаны 168 Lua-файлов,
   mod.json, ui/package.json и ui/config-overrides.js (171 файл, 14 345 578 байт).
   Все 171 Git blob SHA-1 совпали с API, несовпадений 0. Оригиналы не редактировались.
2. `check-syntax.lua` + `lua-files.txt` проверяют loadfile без исполнения скриптов;
   `C:\Games\tools\lua54\lua54.exe`: **168 проверено, 0 ошибок**.
   Результат: `syntax-result.txt`. Это только синтаксис, не совместимость с движком.
3. Изолированный тест-хост использует копии текущих EVBots.Test.exe, EVBots.dll,
   lua.dll и ev-core.dll. EVBOTS_MODS указывает только на каталог исследования;
   EVBOTS_WEBUI=0, EVBOTS_TESTS=0, EVBOTS_LOG_VERBOSE=1.
4. `logs\EVBots.log`: shared init Royale не завершается — в
   `ext/Shared/Configs/2dTreeConfig.lua:143` отсутствует `ShaderParameterType`.
   Client init также не завершается — `ext/Client/UI/Hud.lua:18`, нет `SettingsManager`.
   Определения этих API есть в локальной VU-Docs, их нельзя заменять догадками.
5. `testhost-with-core.txt`: 89 тестов ядра успешны, 1 провален — штатный тест ожидает
   FunBotClient/Registry, но в этом опыте загружался Royale. **89/90 не является
   результатом проверки Royale.** Его загрузка провалена по журналу выше.
6. Первый запуск тест-хоста без ev-core.dll завершился AV внутри теста перехватов.
   После добавления штатной ev-core.dll тест-хост завершился без AV. Первое падение
   нельзя приписывать Battle Royale; журнал — `testhost-output.txt`.

Важный дефект диагностики EVBots: после ошибки shared init журнал всё равно пишет
`LUA-MODE READY`, а ниже `Failed to load EVBots module`. Лаунчер сейчас ориентируется
на READY; при расширении мод-системы нужен отдельный признак успешности всех init.

## Матрица адаптации

| Область | Наблюдение | Что требуется |
|---|---|---|
| Выбор модов | LuaRuntime.cpp:746, LoadMods перечисляет все папки с mod.json; ModList не фильтрует загрузку | Общий выбор активного набора на сервере, клиенте и в раздаче; порядок зависимостей; отказ при ошибке init |
| Лаунчер | Проверки файлов, список карт, RCON-команды привязаны к fun-bots | Профили модов; отдельные карты/параметры Royale; отсутствие загрузки fun-bots в Royale; переключение при остановленном сервере |
| Базовый API | Нет ShaderParameterType и SettingsManager, подтверждено запуском | Реализация по VU-Docs с проверками, без маскирующих заглушек |
| Ресурсы | BundleManager.lua вызывает MountSuperBundle/AddRegistry и LoadBundles/Terrain hooks | В VextResourceManager.cpp:398/414 методы пустые, в VextClient.cpp:798/809 — заглушки; нужны настоящие операции движка |
| Создание объектов | BRAirdropManager.lua:132 и Client/Visuals/OOCFires.lua:49 вызывают CreateEntitiesFromBlueprint | VextEntityManager.cpp:2009 и VextClient.cpp:840 возвращают nil; требуется полноценное создание объектов сервера и клиента |
| UI и компас | Исходники UI есть, готового ui.vuic нет; события Compass используются в PingClient.lua | Собрать и проверить HUD/инвентарь в клиенте, найти или отдельно восстановить совместимый Compass |
| Условия матча | MinPlayersToStart=99, DisableWinningCheck=true, EnableWhitelist=true | Отдельная согласованная конфигурация локального теста; оригинал хранить неизменным |
| Карты | MapsConfig содержит XP5_003 и XP3_Alborz | Проверить DLC, ресурсы и модификации карты; не предлагать произвольные карты BF3 |
| Выпуск | Полного проверенного комплекта нет; условия распространения не установлены | Не включать в рабочий установщик до реального матча и выяснения условий распространения |

## Последовательность дальнейшей работы

1. Закрыть происхождение/комплектность зависимостей, особенно Compass и совместимого VEManager.
2. Реализовать корректную загрузку только выбранных модов с зависимостями и ошибками init;
   сохранить поведение fun-bots и его файлы. Проверить сервер/клиент и переходы между профилями.
3. Добавлять недостающие API на стороне EVBots; сначала реальные ресурсы/blueprint entities,
   затем клиентский интерфейс и механики Royale. Каждая нативная точка — по правилам сверки байтов.
4. Только после этого настроить LAN-матч без ботов и проверить на двух людях: вход,
   высадка, лут, зона, смерть, победа, следующий матч, возврат в fun-bots.
5. Собрать установщик после игровых проверок. Сейчас не заявлять рабочую совместимость.

## Диск и границы работы

До проверки свободно около 5,97 ГиБ, после — около 5,95 ГиБ. Исследование с тестовыми
бинарниками и журналами занимает около 17 МиБ. Полная история Git и node_modules
не скачивались. Уборка диска, память, AGENTS.md и навыки не изменялись.
Это отдельный холодный технический отчёт; нового универсального навыка пока не выявлено.
