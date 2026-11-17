# Ограниченный аудит вызываемых API Royale / VEManager

17.11.2026. Источники: фактические моды `D:/Games-cache/royale/working/Mods/VU-BattleRoyale/ext` и `VEManager/ext`; наша реализация `C:/Games/EVBots-dev/evbots-dll/src`; локальные VU-Docs; для ABI — shipped PE и локальные декомпилы. Игровой прогон не выполнялся. Это список доказанных границ, не заявление о полной готовности мода.

## Доказанные дефекты и работа по ним

| Путь мода / условие | Дефект EVBots | Состояние |
|---|---|---|
| Server/Types/BRTeam.lua:146, удаление команды | DispatchLocal проверял объект команды как сериализуемую таблицу, отвергал методы/циклы | Исправлен LuaEvents.cpp; 21 тест PASS; local-events-report.md |
| Server/GunshipServer.lua:221/238, движение Plane | SpatialEntity(table с _ptr) терял запись transform в оболочке | Исправлен VextSpatial.cpp; 11 тестов PASS; spatial-forward-report.md |
| Server/GunshipServer.lua:230, поиск локатора | LocatorEntity отсутствовал в списке типов round iterator | Передан TypeInfo 02BE7AE8 родителю для интеграции |
| Client/GunshipCamera.lua:85, каждый кадр камеры Plane | Native CameraEntityData.transform — LiveDataContainer LinearTransform; Live_Index не предоставлял LookAtTransform | Родитель добавляет делегирование фактической геометрии |
| Client/CommonSpatialRaycast.lua:59 → BRLooting:GetLootPickup / Hud:OnSpatialRaycast | VextClient.cpp SpatialRaycast безусловно возвращал {}; нет entity.instanceId, сбрасывается выбранный лут, не показывается revive prompt | Реализован настоящий SpatialQuery + weak Entity handles, parent интегрирует install; игровой результат ещё не проверен |
| Client/GunshipClient.lua:71, свободное падение рядом с землёй | Raycast безусловно nil, поэтому не посылается OpenParachute | Реализован native PhysicsRayCaster; parent интегрирует install; игровой результат ещё не проверен |
| Client/PingClient.lua:429/451; SpectatorClient.lua:125; OOCFires.lua:29; Shared/Utils/RaycastHelper.lua:37 | Тот же Raycast nil: пропадает ground ping, проверка стены камеры, высота земли для эффекта; helper возвращает исходный y как fallback | Покрывается той же новой реализацией, не пять разных API |
| Client/UI/Hud.lua:910..924, Client/BRTeam.lua:101 | Старый player.soldier имел только transform/несколько констант: health и weaponsComponent отсутствовали | Новый VextClientSoldier читает реальное health, state, weapon ammo/fireLogic/name; 31 host checks PASS |
| Client/UI/Hud.lua:747..749; BRTeamManagerClient.lua:114/127 | soldier.teamId/player/isInteractiveManDown отсутствовали; запись teamId создавала теневое Lua-поле | Native getter/setter и владелец реализованы, учитывается TeamCapacity128 |
| Client/SpectatorClient.lua:98 | ragdollComponent отсутствовал, вызов GetActiveWorldTransform(46) падал | Реализовано чтение настоящего active QuatTransform по native skeleton bound; nil только по допустимым состояниям |
| Client/GunshipClient.lua:95/133 → GunshipCamera:GetTransform | JumpOut передаёт native LinearTransform userdata, тогда как VextNetCodec принимал geometry только как table | Родитель исправил codec: приватный C classifier проверяет настоящий LiveDc/kShapeStruct/kValueType и whitelist geometry. Actual codec/extracted classifier11 PASS: идентичные wire bytes, roundtrip JumpOut, nested value, изменение backing transform, отказ произвольным userdata/class/invalid shape. Игровой прыжок ещё не проверен |
| Server/__init__.lua:185/807, настройки из Level:LoadResources | Stock RCON уходил по TCP к серверу из load thread и возвращал fake100 | VextRcon разрешает настоящий native load scope, владеет копией ответа, исключает TCP fallback из callback и ложные значения. Родитель в F818D265 подтвердил native OK для autoBalance и прочих vars; повторный hook gate исправлен,90 isolated PASS; rcon-load-report.md |
| Там же, vu.SuppressionMultiplier0 | Отсутствовала команда; подавление оставалось stock100 | NativeRconGameplay: настоящий server consumer01338C70, x86 cdecl7args, factor percent/100. Следующая интеграция родителем;36 checks включая Squad, реальный матч не проверен |
| Там же, vu.SquadSize4 | Отсутствовала команда VU | Узкий getter/no-op4 по native GameModeSettings и active array; иные размеры Unsupported. Client startup должен подтвердить4 native constants. NativeRconGameplay готов для интеграции; rcon-gameplay-report.md |

Оставшиеся RCON параметры этого пути: ColorCorrectionEnabled/SunFlareEnabled и их репликацию реализует visual agent; roundStartPlayerCount0/roundRestartPlayerCount0 и native clamp исследует team_limit agent. Их не объявляем готовыми только потому, что native RCON теперь возвращает честный ответ. Условный vu.ServerBanner не поддержан, влияет только на баннер VU browser.

Player:Created, full-vector iterator, CharacterSpawnReferenceObjectData constructor, native typeInfo исправляет родитель. VisualEnvironmentManager исследует отдельный агент; повторный независимый патч не делается.

## Geometry: точная область использования

Единственный найденный вызов geometry-метода на native LinearTransform поле — GunshipCamera.lua:85 `self.m_Data.transform:LookAtTransform(camera,world)`. Он меняет self.left/up/forward/trans и возвращает void/nil (VU-Docs lineartransform.md и фактический LTMethods). Следующие строки присваивают left/forward, умноженные на -1. PushLiveValue уже переводит native Vec2/Vec3/Vec4 в штатные Lua value-типы, поэтому их операторы и Clone доступны.

GunshipServer и SpectatorClient вызывают LookAtTransform на обычном LinearTransform() значении; MapLoader строит LinearTransform из JSON-таблиц. Translate/Inverse/ToQuatTransform на native полях в этих двух модах не найдены. Native Clone структуры уже реализован в Live_Clone через DcPushValue; остальные найденные Clone относятся к DataContainer и не являются geometry-пробелом.

## Инвентарь, лут, ESC: что не следует ошибочно назвать заглушкой

- BRInventory.lua:631 вызывает Soldier:ApplyCustomization. В VextPlayerManager.cpp есть фактическая ветка native CustomizeSoldierData для человека, CaptureHumanSoldier/ApplyHumanSoldierCustomization; старые комментарии о заглушке неверны. Настоящий порядок Ready → живой soldier → customization требует игрового прогона.
- Записи primaryAmmo/secondaryAmmo доходят до native WeaponFiring +F4/+F8; есть проверка существования компонента и ограничение ёмкостью магазина. Отсутствие тела/оружия даёт отказ по состоянию, не безусловную заглушку.
- ClientUtils:GetCameraTransform использует native __cu_view. Родитель удалил старую эвристику «камера дальше30м от солдата → глаза/пауза300вызовов»: она ошибочно отбрасывала реальные Plane/showroom камеры. Если renderer не готов, возвращаетсяnil; это обработано CommonSpatialRaycast/BRLooting. Actual Lua block6 PASS: далёкая камера, оси, проекция, nil и восстановление без cooldown. Живой Plane ещё проверяется.
- InputManager клавиши и кнопки мыши переопределены __im_key/__im_mouse; концепты — __im_concept. По начальным определениям в VextClient.cpp нельзя считать E/ESC неработающими.
- ESC использует ClientUIGraphEntity итератор, RegisterEventCallback и FireEvent. Native обработчики этих операций существуют; точное обнаружение GUID зависит от итератора/data. Кнопки team и inventory в EscMenu.lua сами содержат `INFO: ... is missing.`: это незавершённость текущего мода, не недостающий API EVBots.
- SpectatorManager явно отказывает в remote FirstPerson, но Royale просит чужого игрока с false (third person), а true только своего при выходе. Этот предел не блокирует найденную цепочку.
- CreateEntitiesFromBlueprint и CreateEntity имеют native implementation, проверку realm/TLS/срока жизни. Отказ при offline proxy вместо native DC — реальная несовместимость входа; нельзя лечить возвратом фальшивой сущности.

## Дополнительные использованные ограничения

InputManager:SetCursorPosition был пустой заглушкой: Chat.lua:201 и PingClient.lua:244 вызывают её для центрирования курсора. Родитель реализовал Win32 ClientToScreen/SetCursorPos для активного окна собственного процесса с проверкой конечности/диапазона координат. Это UX-дефект открытия chat/ping wheel, не причина отсутствия soldier. WebUI:GetScreenWidth/Height имеют реальные override через __webui_size; они не являются пробелом.

## Native лучи: доказано на момент аудита

Исходный NativeSpatial реализует только transform getter/setter, NativeCollision — подписки на столкновения. Новые реализации добавлены в VextClientRaycast; подробный ABI и пределы проверки — client-query-report.md. VU SpatialRaycast `10B48770` вызывает слот `18E752CC` → клиент `0083B9B0`; выдаёт реальные Entity*. VU Raycast `10B48A40` вызывает virtual slot 0 физического raycaster `[level+BC]+40`. Подмена SpatialRaycast списком rigidBody из CollisionRaycast не использовалась. ClientSoldier/weapon/ragdoll доказательства — client-soldier-report.md.
