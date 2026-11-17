# Настоящие клиентские Raycast / SpatialRaycast

17.11.2026. Изменены только VextClientRaycast.cpp/.h. Бэкапы — `C:/Games/backup/2026-11-17/royale-spawn/VextClientRaycast-build-next-pre.cpp` и `.h`.

## Реализация

- `InstallClientSpatialRaycast(L)`: устанавливается после RegisterVextClient и RegisterVextBlueprintEntities. Использует stock SpatialQueryManager `0083B9B0`, this = GameWorld (`*023813A0`) +844. Возвращает настоящие Entity* через CaptureWeakPeer/AdoptWeakPeer и общий evbots.BlueprintEntity. Поэтому instanceId совпадает с созданными модом сущностями лута. Возвращаемые элементы сохраняют native order; не подменяются rigidBody от CollisionRaycast.
- ABI подтверждён VU `10B4886D..10B48929`: label, output fixed_vector, from, to, filter, flags; native функция заканчивается ret18h. Native `0083ADFB` читает range min/max, затем append Entity*. Collector содержит begin/end/capacity/allocator/fixed, inline1024 элемента. Рост `00837830` использует allocator `00495780`; освобождение overflow — `004957E0`. Пакет прологов проверяется целиком до query. Engine heap освобождается также при отказе/исключении.
- `InstallClientPhysicsRaycast(L)`: следует отдельному интерфейсу VU `10B48A99..10B48B2B`: Level+BC → PhysicsManager+40 → virtual slot0, `bool(this,label,from,to,hit,flags,null)`. Это не GameWorld CollisionRaycast. Hit0x40, alignment16 подтверждены аллокацией VU `10B40B1D..10B40BAD`. Выдаются position/normal, part/bone, native material и tracked rigidBody. Getter material `10A83CB6` берёт hit+24 непосредственно как DataContainer pointer.

Оба пути требуют реальный engine-frame, initialized Havok TLS, актуальный level/world и живые native handles. Для виртуального Raycast проверяется принадлежность vtable и executable target текущему образу bf3.exe; конкретный адрес виртуальной реализации заранее не зашит. ABI интерфейса доказан shipped VU, а не заменён предположением о CollisionRaycast. Отсутствие level/world и отсутствие попадания — допустимые nil/пустой массив. Отказ сигнатуры, чужой поток, неверная lifetime — явная ошибка. Нулевой луч не передаётся физике.

## Проверки и пределы

`client-query-proof.py` / `client-query-proof.txt` содержат дизассемблирование shipped VU и zbf3.exe, SHA256 обоих. `compile-client-query.cmd`: compile-only MSVC x86 с производственными /W4 /WX. `client-query-boundary-test.cpp` включает фактический production .cpp; 20 PASS проверок регистрации, типов аргументов, NaN/Inf/flags и запрета вне engine-frame. `test-client-query.cmd`, `client-query-boundary-test.log`.

Этот host не вызывает физику игры. Нативные реальные попадания и SpatialRaycast по конкретному луту должны быть проверены следующим игровым прогоном родителя. Игры этим агентом не запускались; основная DLL не собиралась. Начальная compile-команда без WIN32_LEAN_AND_MEAN попала в конфликт Windows-макроса small в старом DebugRenderer-коде; повтор с точными production defines прошёл.
