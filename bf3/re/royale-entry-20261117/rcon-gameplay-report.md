# Native gameplay RCON: Suppression и SquadSize

Новые исходники `C:/Games/EVBots-dev/evbots-dll/src/NativeRconGameplay.cpp` и `.h`; существующие файлы запуска не менялись. Для следующей сборки добавить cpp в список build, include h в LuaRuntime и вызвать `RegisterNativeRconGameplay(L)` **после** `RegisterVextRcon(L)` только на сервере. На клиенте вызвать `VerifyNativeSquadSizeClient()` как обязательную startup-проверку для узкого контрактаSquadSize4; приfalse не объявлять этот контракт поддержанным. Игра и память игрового процесса не использованы.

## SuppressionMultiplier

Реализован настоящий consumer: dedicated `SuppressionUtil::suppressEnemies`01338C70. Команда хранит процент, detour передаёт шесть исходных32-bit аргументов без изменений, седьмой float умножает на `float(percent)/100`. Возврат оригинала — void, ABI cdecl, очистка стека вызывающей стороной28байт. Штатный pipeline продолжает рассчитывать подавление и передавать результат. При0 native01338F09 умножает на0, итоговый integer становится0,01338F42 пропускает применение01329090. Не реализован отдельный Lua hook Server:SuppressEnemies; здесь только команда, используемая Royale.

Доказательства VU: core.ev-orig SHA44f4dd2a55362ad63e7b5b4f060c8411a569544c3a0981bea3591737f0b9f163, registration101B131C→101E6DC0. Setter101E712C пишет floatglobal1087430C=integer/100; gameplayconsumer101CFA70 умножает аргумент7 в101CFA98. VU wrapper101CF960 передаёт7 слов, вызывает original108D81AC, затем add esp1C. Hookinstall101ACCB6 связывает с retail/listen-server native0083A3F0. Dedicated аналог01338C70 подтверждён literaltag02696E48 `SuppressionUtil_suppressEnemies`, парой nativecallers012C66E8/012C6818 и совпадающей цепочкой расчёта/передачи. Не переносится клиентский адрес в dedicated.

VU multiplier — собственное состояние серверного callback. Его единственный gameplayconsumer находится перед штатным применением подавления; отдельная сетевая репликация числа не обнаружена и для этой реализации не добавлена. Клиент получает результат по штатному engine pipeline. Граница: визуальное подавление и разброс в настоящем матче пока не проверены.

Перед установкой проверяются целый11-byte пролог, обе call-target/cleanup пары, инструкции floatconsumer и skipzero, profilerpush и настоящая строка с учётом base relocation. Все проверки происходят до единственной установки через InlineHook. DLL закрепляется; trampoline публикуется ядром до записи перехода. Требуется настоящий server frame или native Level:LoadResources. `EVBOTS_SUPPRESSION=0` запрещает setter/доступ. Отказ подписи, pin или hook даёт ERROR и не меняет значение. Для stock100 hook не нужен: исходный consumer проверен и коэффициент1 оставлен.

Команда: getter `{OK,percentage}`, setter `{OK}` только после применения к установленному consumer. Принимается один десятичный unsigned32 percentage; мусор/отрицательное/дробь/лишний аргумент →InvalidArguments. Scalingfloat имеет обычное округлениеfloat, как уVU. Getter повторяет VU `float(scale*100)→CVTTSS2SI`, включая signed-indefinite при переполнении очень большого процента. Текущие0/100 точны.

## Проверки

- `rcon-gameplay-proof.py/.txt`: реальныеPE SHA/адреса/fileoffsets/Capstone, assert двух callers, tag, пролога, floatconsumer и четырёх client squad constants.
- `test-rcon-gameplay.cmd`: **36 PASS**, actual production source, PE загружен как неисполняемые данные; подменены только Module/thread/pin/InlineHook install и lookup синтетическихGameModeSettings. Проверены реальные байты, all-or-none отказ, failedhook, no-engine/env-off, getterCVTTSS2SIoverflow, actualLua, configured/active squad readback и все4clientlimits по отдельности.
- Тест ABI вызывает production detour и получает результат настоящим x86 naked machine-code receiver: сохранены6 opaque args, caller stackcanary, seventhfloat0 и0.375 при50% от0.75. Это проверка ABI обработчика, не запуск native игрового кода.
- Production compile x86W4/WX PASS (`compile-rcon-gameplay.log`), Lua chunks2/bad0 дляGameplay+VextRcon, gitdiffcheckPASS; общийobj/build не тронут.

## SquadSize — граница доказанной реализации

Retail client имеет ровно4 в четырёх местах VU patchset:008637DE `cmp eax,128` при stride32,0091CC7B `cmp eax,4`,0091D9DA и0091DA10 `cmp esi,4`. VU client handler1018B440 пишет собственныйglobal10874314 и эти immediates.

Dedicated не имеет эквивалентной постоянной4: `joinSquad`0118DD91 читает native arraybegin из0285BB38, затем0118DD97 берёт `begin[team].squadSize` (stride8,offset4), сравнивает число игроков и разрешает вход только приcount<size. Массив наполняется из активного GameModeSettings; Royale GameModeSettingsModifier.lua29 задаёт4 добавленным командам.

Реализован только честный getter/no-op для4: настоящий singleton GameModeSettings проходит TypeOf/IsKindOfName; его cache, если заполнен, должен совпасть. Native getter01164A20 задаёт layout: information stride28, sizes stride1C, teams stride8. Проверены инструкции singleton/information/stride и consumer joinSquad. Во время LoadResources, когда current table ещё пустая, читаются **все** information/sizes/teams готового GameModeSettings. Каждая игровая команда с playerCount>0, кроме neutral0, должна иметь squadSize4; хотя бы одна такая команда обязательна. Поэтому выбранный режим не угадывается. Если current table уже наполнена, она должна согласоваться. Пустые/неготовые массивы, смена singleton, invalid bounds, иная текущая или настроенная ёмкость дают ERROR. Данные не меняются. Getter возвращает `{OK,"4"}`, setter4 — `{OK}` только после этой проверки. Другие размеры → Unsupported, malformed/0 → InvalidArguments.

Client helper сверяет4 целых инструкции, поэтому контракт возможен только с интеграцией этой проверки при startup. Полная репликация произвольного размера отсутствует; общий канал настроек визуального агента не дублирован. Это узкая совместимость текущего значения4 Royale, не полная реализация vu.SquadSize.
