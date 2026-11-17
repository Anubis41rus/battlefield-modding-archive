# Royale — статус 12.11.2026 (стенд закрыт, parity-safe по умолчанию)

## Состояние
- Стенд остановлен (`run\stop.ps1`), процессов `bf3`/`bf3_server_Final`/`EV Backend` нет.
- По умолчанию восстановлено: `server-runtime-v3\Data -> C:\Games\BF-Portable\Battlefield_3_Server\Data`, `XP5_003.lua` = `backup\2026-11-12\XP5_003.lua.parity-no-coop` (без COOP_009).
- Рабочие моды: `D:\Games-cache\royale\working\Mods\{VU-BattleRoyale,VEManager,VU-BattleRoyale-Compass}` (fun-bots не трогался). Бэкапы правок: `C:\Games\backup\2026-11-12\`.

## Проверки (12.11)
- Overlay-сервер `D:\Games-cache\royale\server-assets-csharp-v3\Data` (Linux overlay) стабильно готов, VEH 0, ERR 0: XP5_003 full (COOP_009) ~32 с, no-COOP ~29 с (`LUA-MODE READY`/`LEVEL-MODE READY`).
- Parity/vanilla server Data + XP5_003 с COOP_009 superbundle/bundle **падает**: `MountSuperBundle(Levels/COOP_009/COOP_009)` unknown superbundle, затем AddRegistry/resource lifecycle ошибки и crash dump. Поэтому для vanilla server data COOP_009 в конфиге XP5_003 исключён.
- Автоматическая попытка join в эту сессию не дала подключённого игрока (`Connected players: 0`, клиент не остался в процессе), явных `Level Checksum Failed` / `MismatchingContent` в проверенных логах не найдено. Нужна ручная проверка join с человеком.
- Примечание по parity: `BundleManager` монтирует `Levels/COOP_009/COOP_009` (без Win32/Linux префикса) — движок резолвит платформу; стоковый Win32-клиент уже содержит COOP в `Update\Patch\Data\Win32\Levels\COOP_009`. Отдельный Win32 client asset overlay, вероятно, не обязателен для XP5_003, но требуется подтвердить join/checksum на реальном клиенте.

## Сделанные правки
- **Стенд (`EVBots-dev\stand\run\`, синтаксис OK):** Royale quiet env/readiness, `Prepare-StandRoyaleServer` (перепривязка Data), `Prepare-StandRoyaleClient` (изолированный client runtime `D:\Games-cache\royale\client-runtime`), `join.ps1 -ClientGameDir`, автоконфиг XP5_003 по data-режиму (`overlay -> full-overlay`, иначе `parity-no-coop`; `EVBOTS_ROYALE_XP5_CONFIG=keep`). Скрипты: `start-royale.cmd`, `start-royale-vanilla.cmd`, `start-royale-full.cmd`, `start-royale-full-nojoin.cmd`, `join-royale.cmd`.
- **VEManager client:** nil-guards `VisualEnvironmentManager` в `GetState/InitializePresets/UpdateVisibility/GetDefaultValue`.
- **VU-BattleRoyale client:** `ConnectionHelper:GetNode` pcall/fallback портов (`inValue`↔`in`), `HudUtils` безопасное построение showroom/UI graph и портов, `Hud.lua` nil-safe `OnLevelFinalized` level name — устраняет цикл ошибок `ActionNode: нет поля 'inValue'`/VE nil в client path (требует проверки на живом клиенте).
- **BundleManager/guards:** устойчивость mount/AddRegistry/VE/FireEffects сохранена.

## Режимы запуска
- **Parity (рекомендуется для join со стоковым клиентом):** `explorer.exe "C:\Games\EVBots-dev\stand\run\start-royale.cmd"`; join — `run\join-royale.cmd` (опц. `-ClientGameDir ...`). XP5_003 без COOP_009.
- **Full overlay (полный серверный контент COOP, эксперимент для клиента):** `explorer.exe "C:\Games\EVBots-dev\stand\run\start-royale-full-nojoin.cmd"` затем join стоковым Win32-клиентом и проверить отсутствие `Level Checksum Failed`/`MismatchingContent (18)`.
- Статус/остановка: `run\status.ps1`, `run\stop.ps1`.

## Осталось
- Подтвердить join клиента на full-overlay сервере (stock Win32 client) и при необходимости реализовать Win32 client overlay prep (`EVBots-dev\launcher\src\EVLauncher\RoyaleAssets.cs` / `Prepare-StandRoyaleClient`) для true parity.
- Прогнать и починить полный BR-цикл на XP5_003 (warmup/самолёт/спавн/трансформация/лут/аирдроп/зоны/элиминация/победа/новый раунд) — требуется **≥2 человека**.
- При воспроизведении клиентского краша — анализ `stand\logs\EVBots-client.log`, `VU-BattleRoyale-client.log`, `VEManager-client.log`, crash dump; добить оставшиеся модовые ошибки (Airdrop blueprint/resource и др.) на актуальном прогоне.
- Финализировать конфиги/окружение, при необходимости пересобрать launcher (`EVBots-dev\launcher`, dotnet) и пакет.

## Главный оставшийся блокер: full content + checksum parity клиент/сервер
- Overlay `server-assets-csharp-v3\Data` — Linux-формат (COOP Win32→`Linux/Levels`, rename путей, фильтрация client-only ресурсов, перепакованы XP3, сгенерированы Linux layout/cas/initfs; `EVBots-dev\launcher\src\EVLauncher\RoyaleAssets.cs`, `overlay-manifest.json`). Стоковый Win32-клиент не использует Linux-layout/data; `-skipChecksum*` движком не учитывается → при несовпадении контента `Level Checksum Failed`/`SecureReason_MismatchingContent (18)`.
- Нужна подготовка **Win32-клиентских** ассетов, эквивалентных по содержимому серверному overlay (COOP/XP3 в Win32-форме, client layout/cas/patch), и запуск клиента из изолированного runtime (`Prepare-StandRoyaleClient`, `-ClientGameDir`) в паре с overlay-сервером. Это отдельная реализация (текущий RoyaleAssets — server-oriented).

## Ещё нужно
- Полный цикл BR (warmup/самолёт/спавн/трансформация/лут/аирдроп/зоны/элиминация/победа/новый раунд) — не проверен; нужен parity-корректный runtime и **≥2 подключённых человека**.
- Клиентский краш — в этой сессии не воспроизведён, нужен повтор с логами в parity-режиме (`stand\logs\EVBots-client.log`, loader/runtime/crash dump).

## Запуск
- Parity: `explorer.exe "C:\Games\EVBots-dev\stand\run\start-royale.cmd"`; клиент — `run\join-royale.cmd` (опц. `-ClientGameDir ...`).
- Overlay (эксперимент, сервер готов; клиенту нужна Win32 asset parity): `start-royale-full.cmd -NoJoin`.
- Статус/остановка: `run\status.ps1`, `run\stop.ps1`.