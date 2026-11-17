# RCON при загрузке Royale — 17.11.2026

Готовый исходник: `C:/Games/EVBots-dev/evbots-dll/src/VextRcon.cpp`. Бэкап: `C:/Games/backup/2026-11-17/royale-spawn/VextRcon-load-dispatch-pre.cpp`. Другие production-файлы не менялись. Игровые процессы, память игры и сеть не использованы.

## Причина и исправление

В оригинальном Royale `Server/__init__.lua:185` вызывает SetupRconVariables из **Level:LoadResources**, а не Level:Loaded. В журнале run70D7 первый кадр — поток11380, команды настроек — поток13756. Два уже реализованных native vu.DesertingAllowed/DestructionEnabled успешно меняли SettingsManager в том же callback, остальные шли по TCP к серверу, который ожидал завершения загрузки. После таймаута Lua возвращал выдуманное `{command,"100"}`.

Теперь native dispatcher доступен в потоке кадра **или внутри настоящего TLS scope eng::IsInNativeLevelLoadResources()**. Синтетическое событие Lua scope не создаёт. Вызов остаётся синхронным и завершает изменение до возврата callback. Native отказ не повторяется через TCP к себе. Standalone без dedicated модуля не запускает ни native dispatcher, ни TCP. Существующий внешний путь для вызовов вне native scope сохранён.

Lua возвращает реальные слова обработчика; UnknownCommand не изменяется. Недоступный транспорт/исключение/пустой ответ дают `{ "ERROR", diagnostic }`, пригодный для `#result`. Удалены оба выдуманных значения100 и64. Это диагностический контракт EVBots для недоступного движка; точное имя транспортной ошибки VU не заявляется.

Исправлена также собственность ответа: прежний detour сохранял указатели в локальные строки native обработчика и разыменовывал после возврата dispatcher. Теперь строки копируются во время detour. Вложенный вызов имеет собственные packet/connection/reply; внешний capture восстанавливается, включая SEH-отказ. Sequence атомарный.

## Доказательства

- Лог `C:/Games/EVBots-dev/stand/logs/EVBots.log`, записи05:27:51–05:27:53: frame11380, команды13756, timeout1500мс, fake100; обе native boolean записи с readback verified.
- VeniceEXT SHA256c34ad1823f662b1a7515ae28d0f646d633f9b8f2b7ad6dd1e9df7922fd912ee8: регистрация SendCommand10BA755A связывает10BA7EC0/10BA7BB0. Оба синхронно получают сервис через1072F6C0→virtualD4, вызывают virtual0C и преобразуют возвращённые строки в Lua. В этих обёртках нет TCP или ожидания следующего Lua tick.
- Native dispatcher/response ABI сохранён: server00DB7F40, response00DB7570, существующая сверка10байт response prologue не ослаблена. Штатные обработчики уже использовались нашим frame-путём; изменено разрешение строго native load scope, а не произвольного потока.

## Проверки

- Production x86 `/O2 /MT /W4 /WX /std:c++17`: PASS, `compile-rcon-load.log`.
- Lua syntax: chunks1, bad0; git diff --check PASS.
- `test-rcon-load.cmd` / `rcon-load-test.log`: **80 PASS**. Включены предыдущие57 settings checks. Новые проверки: native loading callback, точные слова/авторизация packet, синхронный setter, уничтоженный native buffer после ответа, вложенные вызовы, missing reply, SEH, отсутствие TCP retry, чужой thread, прежний frame path, standalone, UnknownCommand, Lua transport exception/empty response.
- Harness использует актуальный production source. Единственное заменённое тело — установка машинного hook (`InstallSendWordsHookOnce`), поскольку harness не содержит игру по жёсткому адресу. Native dispatcher — fixture; чтение packet, capture, routing и Lua chunk настоящие. SettingsManager/module/thread — прежние явно синтетические fixture. Попытки TCP перехватываются sentinel; connectCalls0. Это не проверка живого матча.

### После живого F818D265

Родитель подтвердил настоящие OK для stock vars05:52:27, включая autoBalance false. Новый отказ05:52:44 был конфликтом порядка установки: dispatch-hook дополнительно требовал stock bytes responseentry, уже заменённые нашим ранним SendWordsDetour. Исправлено строго: допускаются исходные10bytes либо **ownedinstalled flag + точныйE9 на нашdetour + нетронутыйtail5 + точныйtrampoline сoriginalmov иjmpback**. Инойjump/модификация/нетownership отвергаются; dispatcherprologue иUnknownCommandmarker всё ещё обязательны. Повторная проверка уже установленного response-hook тоже проверяет ownershipbytes. Бэкап `VextRcon-owned-hook-pre.cpp`.

Production W4/WX сноваPASS. `rcon-load-test.log`: **90 PASS** после добавления обоих порядков Dispatch→SendWords и earlySendWords→Dispatch, foreignjump/ownership/tail/trampoline/dispatcher/UnknownCommand corruption cases. Native игровые hookinstallation в harness по-прежнему подменены; тестируется настоящий gate и response path. Следующий runtime порядок установки проверяет родитель.

## Четыре оставшиеся команды VU

Пока stock dispatcher возвращает UnknownCommand для `vu.ColorCorrectionEnabled`, `vu.SunFlareEnabled`, `vu.SuppressionMultiplier`, `vu.SquadSize`. Они не подменены одноимёнными полями или OK. ServerBanner также не реализован (условный UseOfficialImage), не влияет на физику/спавн. FrequencyMode и два native boolean доступны.

Предварительно подтверждено по vu-core: SquadSize client handler1018B440 пишет собственный global10874314 и четыре immediate в клиентском коде008637DF/0091CC7D/0091DA12/0091D9DC. ColorCorrection client handler1018AA30 вызывает собственный manager101F3840, меняющий поля effect+3C/+3D. SunFlare client handler1018B9F0 пишет собственныйglobal10874308; его consumers10113FE2/10114002 требуют отдельного разбора. SuppressionMultiplier RCON registration101B131C→handler101E6DC0. Репликация/серверные эффекты этих четырёх команд ещё исследуются; эту пачку можно собирать независимо.
