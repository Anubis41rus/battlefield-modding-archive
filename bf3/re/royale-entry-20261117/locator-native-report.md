# Серверный LocatorEntity в фазе Plane

В ручном прогоне после Ready 13:53:53 сервер каждый PreSim отклонял запись GunshipServer.lua:275 с ошибкой SpatialEntity transform override is not verified for this entity. У LocatorEntity свой getter/setter, поэтому проверка общих виртуальных функций исключала штатную запись маршрута.

В локальном dedicated PE SHA256 F39F7623063CFDE9C449A0BDC9A57448D2D137D6DEF00B17EA04800EDA955470 подтверждены TypeInfo 02BE7AE8, таблица 02472C20, getType 005E46C0, getter +58 = 005E46F0, setter +5C = 005EDBB0. Getter копирует 64 байта из this+20. Setter копирует матрицу в свою структуру и, при подключённом выходе this+60, записывает выход и вызывает DataBus-обработчик 004E1830. Относительный вызов подтверждён дизассемблированием, операция выполняется штатным setter с сохранением связанного выхода.

В NativeSpatial.cpp этот путь доступен только dedicated LocatorEntity с точным TypeInfo, vtable и адресами виртуальных методов. Проверяются полные байты getType, getter и setter, а также 22 байта входа диспетчера и принадлежность страниц образу. Stock путь сохраняет проверку собственного onTransformChanged, frame/TLS/weak generation, проверка конечности векторов и флаг EVBOTS_SPATIAL_TRANSFORM применяются и к Locator.

MSVC x86 /W4 /WX compile-only прошёл, 8 сравнений с PE и 11 проверок фактического кода пропуска override прошли. Лог locator-native-test.log. Изолированный тест не исполняет игровой setter. Бэкап C:/Games/backup/2026-11-17/royale-round-fixes/NativeSpatial-build-next-pre.cpp. Движение Locator и синхронизацию самолёта требуется подтвердить следующим ручным прогоном.
