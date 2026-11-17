# Число ботов без правки мода: отчёт (20.10.2026, только чтение)

Корень мода: `C:\Games\BF-Portable\Battlefield_3_Server\Instance\Admin\Mods\fun-bots\ext\` (далее `ext\`).

## 1. Что есть у мода

- **RCON (server):** `ext\Server\Commands\RCON.lua`. Все команды регистрируются через `RCON:RegisterCommand(имя, RemoteCommandFlag.RequiresLogin, cb)` (стр. 268, 299), если `Config.DisableRCONCommands` не true (`Config.lua:176`, сейчас false).
  - `funbots.config.<ИмяНастройки> [значение]` (стр. 266-288) создаётся для КАЖДОГО ключа `Config`. Без аргумента читает, с одним аргументом пишет через `SettingsManager:UpdateSetting(имя, значение)`.
    - `funbots.config.InitNumberOfBots 12`
    - `funbots.config.SpawnMode 3` (число) или `... balanced_teams` (имя, поиск подстрокой)
    - `funbots.config.SpawnInBothTeams true|1`
  - `funbots.spawn <Amount> <Team>` (стр. 130-159): разовый спавн (`SpawnWayBots`), режим не меняет.
  - `funbots.kickAll`, `funbots.killAll`, `funbots.kickBot <Name>`: ставят `Globals.SpawnMode = "manual"` (бот-менеджер остановлен).
  - `funbots.saveall` пишет Config в `mod.db` (FB_Settings). `funbots.restore` сбрасывает на Default. Эти две НЕ нужны и не вызывать (трогают файл мода).
  - `funbots` без аргументов выводит список команд.
- **Console:Register:** в серверном коде мода НЕ используется. В `ext\Client\ConsoleCommands.lua` есть клиентская консоль (NetEvents `ConsoleCommands:SetConfig` -> `Server\Commands\Console.lua`, право `UserInterface.Settings`), это путь для игрока, не для лаунчера.
- **Чат-команд** для числа ботов нет. UI (`UIServer.lua`, строка 755) тоже вызывает те же `Globals.SpawnMode = Config.SpawnMode; UpdateBotAmountAndTeam()`.
- **Подписок на событие RCON** у мода нет (только RegisterCommand).
- **mod.db / DB:** `SettingsManager:OnExtensionLoaded` (SettingsManager.lua:47-152) при загрузке накладывает FB_Settings на Config. Это файл мода, не годится.

## 2. Как применяется

`UpdateSetting` (SettingsManager.lua:219-309): проверяет тип и диапазон (`InitNumberOfBots`: Range 0..128, `SettingsDefinition.lua:366-376`), пишет `Config[имя]` ВРЕМЕННО (`Update(..., true, false)`, в БД не пишет). Для настроек с `UpdateFlag.AmountAndTeam` (InitNumberOfBots, SpawnMode, SpawnInBothTeams, TeamSwitchMode, BalancePlayersIgnoringBots) вызывает `Globals.SpawnMode = Config.SpawnMode` и `BotSpawner:UpdateBotAmountAndTeam()`.
- Применение сразу, в том же вызове (плюс периодический пересчёт каждые 2 с, `BotSpawner.lua:116-119`). Следующий раунд не нужен.
- Если в рантайме было `funbots.kickAll/killAll`, `Globals.SpawnMode` стал "manual"; любое `funbots.config.SpawnMode|InitNumberOfBots` его возвращает к `Config.SpawnMode`.
- Значения SpawnMode (`Shared\Constants\SpawnModes.lua`): manual=0, fixed_number=1, increment_with_players=2, balanced_teams=3, keep_playercount=4. Сейчас `Config.SpawnMode = balanced_teams`.
- Семантика числа (BotSpawner.lua:556-589, 649-682): в `balanced_teams` InitNumberOfBots считается НА КОМАНДУ (общий итог примерно вдвое больше; заметка в Config.lua:40: 7 -> 13, 15 -> 29). В `fixed_number` при `SpawnInBothTeams` число делится на команды (`floor(N / NrOfTeams)`). В `keep_playercount` тоже делится. Для итога "всего N ботов" лаунчеру нужно переводить в то, что ждёт режим. Сейчас лаунчер пишет «на команду» (BotsConfig.cs, комментарий), логику перевода сохранить.

## 3. RCON в VU и у нас

- VU (`VU-Docs\content\vext\ref\server\library\rcon.md`): `RCON:RegisterCommand(command, flags, [context,] callback(command, args, loggedIn) -> string{})` регистрирует команду в консоли сервера. Движок сам принимает RCON-пакет с этим именем и вызывает callback; ответ (`string{}`, первое слово обычно "OK"/"ERROR") уходит клиенту. `SendCommand` обратно шлёт команду в консоль сервера. Флаг `RequiresLogin`: клиент должен сделать `login.plainText`.
- У нас (`evbots-dll\src\VextRcon.cpp`):
  - `RCON:RegisterCommand` (стр. 79-115) только кладёт команду в локальную Lua-таблицу `commands` (handle -> callback). Ни в сам сервер BF3, ни в какой приёмник она нигде не передаётся.
  - Входящей диспетчеризации НЕТ: по поиску `commands[` и `funbots.` во всём EVBots-dev/ (cpp, h, cs, ps1, cmd) обращений к таблице кроме Register/Deregister нет; callback из RCON-пакета никто не вызывает.
  - `RCON:SendCommand` настоящий (с сборки 186): Frostbite RCON клиент к 127.0.0.1:47900, `login.plainText`, env `EVBOTS_RCON_PORT` / `EVBOTS_RCON_PASSWORD` (`EVBotsRconSend`, стр. 297-335). Это исходящий канал DLL -> сервер.
  - Перехват RCON `onMessage` есть только для событий движка (Level:Loaded, RoundOver; `VextPlayerManager.h:31`), а не для команд.
  - Вывод: `RCON 127.0.0.1:47900` -> `funbots.config.InitNumberOfBots 12` сейчас вернёт от настоящего сервера BF3 `UnknownCommand`. Мод свою команду "зарегистрировал", но достучаться до неё нельзя. Сломан именно путь "лаунчер -> RCON -> мод", как у VU он работать не будет, пока не реализована входящая сторона.
- `Console:Register` (`LuaDebugConsole.cpp:174`) клиентская библиотека (по VU: только client), для серверного мода fun-bots не нужна.

## 4. Рекомендация

Правильная команда (как на сервере VU): `funbots.config.InitNumberOfBots <N>` по RCON с логином. Чтобы она реально дошла, надо дописать в DLL (код EVBots, не мод) входящий диспетчер. Варианты по возрастанию честности:

1. **(Как у VU, предпочтительно)** В `VextRcon.cpp` связать `commands` с настоящим RCON сервера: перехват обработки RCON-команд движка (там, где ставится перехват onMessage / таблица консольных команд сервера), для имён, найденных в `commands`, вызывать callback и возвращать его `string{}` как ответ RCON. Требует реверса точки (сверка по правилу 18), в этом отчёте её адрес не искал.
2. **(Быстро, без реверса движка)** Своё локальное приёмное окно в DLL: например, DLL слушает 127.0.0.1:<свободный порт> (или именованный канал) и по строке `команда арг...` вызывает `commands[имя].callback(имя, args, true)` в Lua-стейте (то же, что делал бы движок). Лаунчер шлёт `funbots.config.InitNumberOfBots 12` туда. Для мода выглядит один в один: тот же callback, те же аргументы.
3. **(Без входящего канала вообще)** Лаунчер задаёт число через переменную окружения серверу (`EVBOTS_BOTS=<N>`, не файл мода), DLL после `Extension:Loaded` мода (команды уже зарегистрированы в `__init__` мода) вызывает `commands["funbots.config.InitNumberOfBots"].callback(...)` один раз. Минус: меняется только при старте сервера, не в рантайме.

Момент вызова: после загрузки мода (RCON-команды регистрируются при `require('Commands/RCON')` в `Server\__init__.lua:66-67`, т.е. сразу при старте мода) и желательно до/около `Level:Loaded`; повторный вызов в любой момент действует сразу (≤ немедленно + пересчёт каждые 2 с). Значение держится до рестарта сервера (в mod.db не пишется, `funbots.saveall` не вызывать). Режим по умолчанию balanced_teams не трогать; менять только если нужно: `funbots.config.SpawnMode 3`.

Итоговый порядок для лаунчера: убрать `BotsConfig.Write` (MainWindow3.cs:96); в UI оставить «ботов на команду» (0..128, как Range мода); после готовности сервера (`LUA-MODE READY`) отправить команду через выбранный в п. 1-3 канал; логин и порт RCON не нужны для варианта 2/3, для варианта 1 пароль есть (`EVBOTS_RCON_PASSWORD`).
