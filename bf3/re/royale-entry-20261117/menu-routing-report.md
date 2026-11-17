# F10, Team и Inventory

17.11.2026. Изменена только рабочая копия `D:/Games-cache/royale/working/Mods/VU-BattleRoyale/ext/Client/UI/Hud.lua` и `UI/EscMenu.lua`, по прямому поручению родителя с разрешением пользователя. Бэкапы: `C:/Games/backup/2026-11-17/royale-spawn/Hud-menu-routing-pre.lua`, `EscMenu-routing-pre.lua`. Исходный архив Royale и fun-bots не изменены.

F10 прежде требовал soldier!=nil, поэтому после неудачного Ready с пустым персонажем ничего не происходило. Кнопки Team/Inventory EscMenu содержали только TODO-лог.

Теперь F10 и Team вызывают общий `Hud:OpenTeamLobby()`. В None/Warmup открывается прежний DeployScreen, в том числе без солдата. Despawn отправляется только при существующем солдате и только при первом открытии лобби. Уже открытый deploy не создаётся повторно. При открытии через Esc сначала выполняется штатный OnResume. Карта закрывается.

В WarmupToPlane, Plane, PlaneToFirstCircle, Match и EndGame путь не открывает deploy и не отправляет Despawn: он открывает Esc и `OnMenuTeamDetails()`, показывающий состав команды. Этот callback реализует родитель в TypeScript. Showroom не менялся.

Inventory использует общий `GetInventoryUnavailableReason()`: необходим загруженный уровень, существующий живой soldier, отсутствие spectator/deploy/options и фазы самолёта. При отказе Esc остаётся открыт и вызывается `OnMenuUnavailable(message)`. При успехе штатный OnResume закрывает Esc, затем существующий OpenInventory закрывает карту, выставляет inventory state и включает мышь. TAB также вызывает тот же OpenInventory, закрытие TAB сохраняется.

Циклический require не добавлен: Hud передаёт себя в EscMenu callback; EscapeMenu делегирует нужные команды этому объекту. Остальные команды resume/options/quit сохранены.

`menu_routing_fixture.lua` загружает фактические полные исходники Hud и EscMenu с контролируемыми engine/UI границами: **35 PASS**. Проверены F10 с/без soldier, idempotence deploy, menu delegation, все пять защищённых фаз с/без soldier, guards level/player/options, inventory success/mouse/map и 11 вариантов отказа с сохранением Esc, обычный resume. Это проверка Lua-маршрутов; реальные WebUI callbacks/мышь и Ready требуют совместного прогона родителем после сборки TS.
