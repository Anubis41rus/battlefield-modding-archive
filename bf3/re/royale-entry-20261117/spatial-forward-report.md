# SpatialEntity: запись через существующий native wrapper

17.11.2026. Исправлен только `C:/Games/EVBots-dev/evbots-dll/src/VextSpatial.cpp`: ветка каста Lua-таблицы с `_ptr` теперь передаёт записи в исходную сущность через `__newindex`. Раньше `SpatialEntity(entity).transform = value` создавал поле на временной оболочке и не вызывал родной setter. Эта цепочка реально используется в GunshipServer.lua:221/238 для самолёта и LocatorEntity.

Бэкап: `C:/Games/backup/2026-11-17/royale-spawn/VextSpatial-build-next-pre.cpp`.

Контракт: чтение и запись одной исходной сущности; неизвестные поля и запрет изменения определяет исходная сущность. Не вводится произвольное разрешение полей native userdata. Повторная запись не создаёт shadow field. Ошибки readonly/type исходного setter не скрываются. Ветка чистого офлайн LinearTransform не изменена.

`spatial-forward-test.lua` извлекает фактические Lua-чанки VextGeometry.cpp и VextSpatial.cpp. 11 PASS, 0 FAIL: запись transform, повторная запись, отсутствие shadow, общая видимость через два каста, неизвестные/nil поля, запреты readonly/type, прежняя офлайн-ветка. Лог `spatial-forward-test.log`. На бэкапе тот же тест падает на первом присваивании transform: `spatial-forward-before.log`. `git diff --check` проходит. Это unit-проверка границы вызова setter, не игровой прогон.

## LocatorEntity: доказанная регистрация сервера

PE `D:/Games-cache/royale/server-runtime-v3/bf3_server_Final.exe`: строка LocatorEntity VA `02471694`, TypeInfoData `02BE7650`, функция регистрации `02263CE0` передаёт data `02BE7650` и this `02BE7AE8` в `004CFC60`. getType `005E46C0` и `00D94DD0` возвращают `02BE7AE8` (`mov eax,02BE7AE8; ret`).

Для родительской интеграции: `NativeBlueprint::kRoundTypes`: `{"LocatorEntity",0x02BE7AE8}`; `VextClientEntities` Lua roundTypes: `LocatorEntity=true`. Эти два файла данным агентом не изменялись. Существующий мод GunshipServer.lua ищет LocatorEntity с data GUID B7C9767E-4154-49F9-B934-F80923BB82C0, поэтому одного исправления SpatialEntity setter недостаточно без регистрации итератора.
